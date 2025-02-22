/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/gpu/runtime/graph_launch.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "tensorflow/compiler/xla/runtime/custom_call.h"
#include "tensorflow/compiler/xla/runtime/executable.h"
#include "tensorflow/compiler/xla/service/gpu/non_atomically_upgradeable_rw_lock.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/concurrent_region.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/conv.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/gemm.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/kernel_launch.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/support.h"
#include "tensorflow/compiler/xla/service/service_executable_run_options.h"
#include "tensorflow/tsl/profiler/lib/scoped_annotation_stack.h"
#include "tensorflow/tsl/profiler/lib/traceme.h"
#include "tensorflow/tsl/profiler/lib/traceme_encode.h"

#if GOOGLE_CUDA
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_graph.h"
#endif  // #if GOOGLE_CUDA

namespace xla {
namespace gpu {

using tsl::profiler::TraceMe;
using tsl::profiler::TraceMeEncode;

using xla::runtime::Arguments;
using xla::runtime::AsyncTaskRunner;
using xla::runtime::CustomCall;
using xla::runtime::Executable;
using xla::runtime::FunctionRef;
using xla::runtime::FunctionType;
using xla::runtime::MemrefDesc;
using xla::runtime::MemrefType;
using xla::runtime::StridedMemrefView;

#if GOOGLE_CUDA
using se::gpu::OwnedCudaGraph;

// Captures Gpu graph by running given function in capture mode.
static absl::StatusOr<OwnedCudaGraph> CaptureGraph(
    const ServiceExecutableRunOptions* run_options,
    runtime::FunctionRef function_ref, Arguments<MemrefDesc>& args,
    CustomCall::UserData user_data);
#endif  // GOOGLE_CUDA

//===----------------------------------------------------------------------===//
// CUDA graphs caching.
//===----------------------------------------------------------------------===//

StreamExecutorGraphInstances* GraphInstances::operator()(
    se::StreamExecutor* executor) {
  absl::MutexLock lock(&mutex_);
  return &graphs_[executor];
}

CapturedFunctionExecutionCount* CapturedFunctionExecutionCounts::operator()(
    se::StreamExecutor* executor) {
  absl::MutexLock lock(&mutex_);
  return &counts_[executor];
}

bool GraphInstances::InstantiatedAllGraphs(
    const ServiceExecutableRunOptions* run_options,
    const Executable& executable) {
  if (executable.num_functions() == 1) return true;

  absl::MutexLock lock(&mutex_);
  return instantiated_.contains(run_options->stream()->parent());
}

Status GraphInstances::InstantiateAllGraphs(
    const ServiceExecutableRunOptions* run_options,
    const Executable& executable, const CustomCall::UserData& user_data,
    void* ptr) {
  // We have only "main" function in the executable.
  if (executable.num_functions() == 1) return OkStatus();

  absl::MutexLock lock(&mutex_);
  se::StreamExecutor* executor = run_options->stream()->parent();

  // All Gpu graphs are already instantiated for a given executor.
  if (instantiated_.contains(executor)) return OkStatus();

  VLOG(3) << "Instantate all Gpu graphs in executable " << executable.name();

  TraceMe trace("cuda.graph.instantiate_all");

  // Initialize graph instances snapshot for a given executor.
  StreamExecutorGraphInstances::Snapshot instances =
      graphs_[executor].snapshot();

  // Instantiate all Gpu graphs by calling graph capture functions with fake
  // arguments. Once we'll execute them first time for real, they'll be updated
  // with correct pointers.
  for (unsigned ordinal = 1; ordinal < executable.num_functions(); ++ordinal) {
    if (!absl::StartsWith(executable.function_name(ordinal),
                          "xla.gpu.cuda.graph.capture"))
      continue;

    VLOG(3) << "Instantiate Gpu graph defined by capture function @"
            << executable.function_name(ordinal) << " (ordinal = " << ordinal
            << ")";

    TraceMe trace_instantiation([&] {
      return TraceMeEncode("cuda.graph.instantiate", {{"ordinal", ordinal}});
    });

    FunctionRef function_ref = executable.function_ref(ordinal);

    const FunctionType& signature = executable.signature(ordinal);
    assert(signature.num_results() == 0 && "unexpected number of results");
    Arguments<MemrefDesc> args(signature.num_operands());

    // Prepare arguments for the graph capture function.
    for (size_t j = 0; j < signature.num_operands(); ++j) {
      auto* memref = llvm::dyn_cast<MemrefType>(signature.operand(j));

      if (!memref)
        return absl::InternalError(absl::StrFormat(
            "Unsupported capture function argument type #%d", j));

      if (memref->sizes().size() != 1)
        return absl::InternalError(
            absl::StrFormat("Unsupported capture function memref rank #%d: %d",
                            j, memref->sizes().size()));

      std::array<int64_t, 1> sizes = {memref->size(0)};
      std::array<int64_t, 1> strides = {1};

      args.emplace_back<MemrefDesc>(memref->element_type(), ptr,
                                    /*offset=*/0, sizes, strides);
    }

#if GOOGLE_CUDA
    // Instantiate a Gpu graph with fake arguments.
    auto instantiate = [&]() -> absl::StatusOr<GraphInstance> {
      TF_ASSIGN_OR_RETURN(
          auto g, CaptureGraph(run_options, function_ref, args, user_data));
      TF_ASSIGN_OR_RETURN(auto e, se::gpu::InstantiateCudaGraph(std::move(g)));
      return GraphInstance(0, std::move(e));
    };

    TF_ASSIGN_OR_RETURN(GraphInstance * instance,
                        instances.GetOrCreate(ordinal, instantiate));
    (void)instance;
#endif  // GOOGLE_CUDA
  }

  instantiated_.insert(executor);
  return OkStatus();
}

//===----------------------------------------------------------------------===//
// Helper structure to hash the remaining arguments' memref pointers.
//===----------------------------------------------------------------------===//

struct RemainingArgsPtrs {
  CustomCall::RemainingArgs args;
  se::DeviceMemoryBase* temp_buffer;

  template <typename H>
  friend H AbslHashValue(H h, const RemainingArgsPtrs& m);
};

template <typename H>
H AbslHashValue(H h, const RemainingArgsPtrs& m) {
  for (size_t i = 0; i < m.args.size(); ++i) {
    if (auto memref = m.args.get<StridedMemrefView>(i); succeeded(memref))
      h = H::combine(std::move(h), memref->data);
  }
  return std::move(H::combine(std::move(h), m.temp_buffer->opaque()));
}

//----------------------------------------------------------------------------//
// Runs capture function exported by the executable to constuct a CUDA graph.
//----------------------------------------------------------------------------//

#if GOOGLE_CUDA

static bool InDebugMode() {
#ifdef NDEBUG
  return false;
#endif
  return true;
}

// Forwards custom call arguments to an arguments container that can be passed
// to an executable function.
static absl::Status ForwardArguments(CustomCall::RemainingArgs fwd_args,
                                     Arguments<MemrefDesc>& args) {
  for (size_t i = 0; i < fwd_args.size(); ++i) {
    if (auto memref = fwd_args.get<StridedMemrefView>(i); succeeded(memref)) {
      args.emplace_back<MemrefDesc>(memref->dtype, memref->data, /*offset=*/0,
                                    memref->sizes, memref->strides);
      continue;
    }

    return absl::InvalidArgumentError("Unsupported argument type");
  }

  return OkStatus();
}

static absl::StatusOr<OwnedCudaGraph> CaptureGraph(
    const ServiceExecutableRunOptions* run_options,
    runtime::FunctionRef function_ref, Arguments<MemrefDesc>& args,
    CustomCall::UserData user_data) {
  // We capture graph on a borrowed stream because we do not want to
  // accidentally record any concurrent kernel launches from other XLA
  // executables.
  se::StreamExecutor* executor = run_options->stream()->parent();

  // Initialize (with memoization) BlasSupport here because cublasCreate fails
  // during cuda graph capturing.
  if (function_ref.RequiresBlas()) {
    if (!executor->AsBlas()) {
      return absl::InternalError("Failed to initialize BLAS support");
    }
  }

  StatusOr<StreamPool::Ptr> capture_stream =
      run_options->BorrowStream(executor->device_ordinal());

  if (!capture_stream.ok())
    return absl::InternalError(
        absl::StrFormat("Failed to borrow a stream for graph capture: %s",
                        capture_stream.status().message()));

  TraceMe trace([&] {
    return TraceMeEncode("cuda.graph.capture",
                         {{"ordinal", function_ref.ordinal()}});
  });

  // TODO(ezhulenev): Pass graph capture context explicitly to the custom calls
  // via UserData to be able to detect when executing custom call in graph
  // capture mode. Currently we rely on the fact that we know for sure that
  // operations in the graph capture function do not need anything except the
  // main stream (we capture only kernel launches).
  ExecutableRunOptions capture_run_options;
  capture_run_options.set_stream(capture_stream->get());

  const ServiceExecutableRunOptions capture_opts(capture_run_options);
  user_data.insert(&capture_opts);

  // Collect all emitted diagnostic messages.
  std::string diagnostic;
  runtime::DiagnosticEngine diagnostic_engine;
  AppendDiagnosticToString(diagnostic_engine, &diagnostic);

  // Prepare options for executing graph capture function.
  Executable::ExecuteOpts opts;
  opts.custom_call_data = &user_data;
  opts.diagnostic_engine = &diagnostic_engine;

  // Graph capture function should not launch any async tasks.
  opts.async_task_runner = reinterpret_cast<AsyncTaskRunner*>(0XDEADBEEF);

  // Create a graph from running the graph capture function.
  auto captured = se::gpu::CaptureCudaGraph(capture_stream->get(), [&]() {
    return function_ref(args, runtime::NoResultConverter{}, opts,
                        /*verify_arguments=*/InDebugMode())
        .status();
  });

  if (!captured.ok()) {
    return InternalError("CaptureCudaGraph failed (%s): %s",
                         diagnostic.empty() ? "<no details>" : diagnostic,
                         captured.status().ToString());
  }
  return std::move(*captured);
}

static absl::Status RunGraphWithoutCapture(
    const ServiceExecutableRunOptions* run_options,
    runtime::FunctionRef function_ref, CustomCall::RemainingArgs fwd_args,
    CustomCall::UserData user_data) {
  // Prepare options for executing graph capture function.
  Executable::ExecuteOpts opts;
  opts.custom_call_data = &user_data;

  TraceMe trace([&] {
    return TraceMeEncode("cuda.graph.run_no_capture",
                         {{"ordinal", function_ref.ordinal()}});
  });

  // Collect all emitted diagnostic messages.
  std::string diagnostic;
  runtime::DiagnosticEngine diagnostic_engine;
  AppendDiagnosticToString(diagnostic_engine, &diagnostic);

  opts.diagnostic_engine = &diagnostic_engine;

  // Graph capture function should not launch any async tasks.
  opts.async_task_runner = reinterpret_cast<AsyncTaskRunner*>(0XDEADBEEF);

  Arguments<MemrefDesc> args(fwd_args.size());
  TF_RETURN_IF_ERROR(ForwardArguments(fwd_args, args));

  auto executed =
      function_ref(args, runtime::NoResultConverter{}, opts, InDebugMode());
  if (!executed.ok()) {
    return InternalError("RunGraphWithoutCapture failed (%s): %s",
                         diagnostic.empty() ? "<no details>" : diagnostic,
                         executed.status().ToString());
  }
  return absl::OkStatus();
}

#endif  // #if GOOGLE_CUDA

//===----------------------------------------------------------------------===//
// Define the cuda graph launch custom call.
//===----------------------------------------------------------------------===//

static absl::Status LaunchGraph(
    const ServiceExecutableRunOptions* run_options,
    const DebugOptions* debug_options, const std::string* ptx,
    const std::vector<uint8_t>* cubin, se::DeviceMemoryBase* temp_buffer,
    StreamExecutorKernels::Snapshot* kernels,
    StreamExecutorConvRunners::Snapshot* convs,
    StreamExecutorGraphInstances::Snapshot* instances,
    CapturedFunctionExecutionCount::Snapshot* counts,
    GemmConfigs::Snapshot* gemm_config, runtime::Executable* executable,
    NonAtomicallyUpgradeableRWLock* gpu_lock,
    ConcurrentRegionStatus* region_status, CustomCall::RemainingArgs fwd_args,
    CustomCall::FunctionOrdinal capture) {
#if GOOGLE_CUDA
  VLOG(1) << "Launch Cuda Graph: ordinal = " << capture.ordinal;

  // Get a reference to exported function that captures the cuda graph.
  runtime::FunctionRef function_ref = executable->function_ref(capture.ordinal);

  // Compute the hash of the buffer arguments.
  size_t ptrs_hash = absl::HashOf(RemainingArgsPtrs{fwd_args, temp_buffer});

  // Forwards user data required for launching kernels.
  auto user_data = [&] {
    return CustomCall::UserData(run_options, debug_options, ptx, cubin,
                                temp_buffer, kernels, convs, executable,
                                gemm_config, gpu_lock, region_status);
  };

  TF_ASSIGN_OR_RETURN(std::unique_ptr<std::atomic<uint64_t>> * get_count,
                      counts->GetOrCreate(capture.ordinal, [] {
                        return std::make_unique<std::atomic<uint64_t>>(0);
                      }));

  int64_t count = (*get_count)->fetch_add(1);
  int64_t num_runs_to_instantiate =
      debug_options->xla_gpu_cuda_graph_num_runs_to_instantiate();

  // TODO(ezhulenev): Cupti tracing leads to deadlocks in CUDA 11. Always fall
  // back on regular execution if we detect tracing activity.
#if CUDA_VERSION >= 12000
  bool is_profiling = false;
#else
  bool is_profiling = tsl::profiler::ScopedAnnotationStack::IsEnabled();
#endif

  if (count < num_runs_to_instantiate || is_profiling) {
    VLOG(3) << "Run gpu graph in op-by-op mode: ordinal = " << capture.ordinal;
    return RunGraphWithoutCapture(run_options, function_ref, fwd_args,
                                  user_data());
  }

  // Instantiate Gpu graph by running graph capture function.
  auto instantiate = [&]() -> absl::StatusOr<GraphInstance> {
    Arguments<MemrefDesc> args(fwd_args.size());
    TF_RETURN_IF_ERROR(ForwardArguments(fwd_args, args));

    TF_ASSIGN_OR_RETURN(
        auto g, CaptureGraph(run_options, function_ref, args, user_data()));

    TF_ASSIGN_OR_RETURN(auto e, se::gpu::InstantiateCudaGraph(std::move(g)));

    return GraphInstance(ptrs_hash, std::move(e));
  };

  TF_ASSIGN_OR_RETURN(GraphInstance * instance,
                      instances->GetOrCreate(capture.ordinal, instantiate));

  {
    // Lock graph instance for read only access. If we'll have to update the
    // graph, we'll update to a writer lock below.
    absl::ReaderMutexLock lock(instance->mutex.get());

    // If pointers did not change we can run captured graph.
    if (ptrs_hash == instance->ptr_hash) {
      TraceMe trace([&] {
        return TraceMeEncode("cuda.graph.launch_cached",
                             {{"ordinal", capture.ordinal}});
      });

      VLOG(3) << "Execute cached graph instance";
      return instance->exec.Launch(run_options->stream());
    }
  }

  // Otherwise we have to re-capture the graph and update the graph instance.
  VLOG(3) << "Update cached graph instance";

  Arguments<MemrefDesc> args(fwd_args.size());
  TF_RETURN_IF_ERROR(ForwardArguments(fwd_args, args));

  // Capture CUDA graph by running capture function.
  TF_ASSIGN_OR_RETURN(
      auto g, CaptureGraph(run_options, function_ref, args, user_data()));

  // At this point we have to grab a writer lock, because we might potentially
  // have concurrent execution of the cached graph instance.
  absl::WriterMutexLock lock(instance->mutex.get());

  // Update captured graph executable.
  TF_RETURN_IF_ERROR(instance->exec.Update(std::move(g)));

  // Update captured graph pointers hash.
  instance->ptr_hash = ptrs_hash;

  TraceMe trace([&] {
    return TraceMeEncode("cuda.graph.launch_updated",
                         {{"ordinal", capture.ordinal}});
  });

  return instance->exec.Launch(run_options->stream());

#else  // #if !GOOGLE_CUDA

  return absl::InternalError("Cuda graphs are not supported");

#endif  // #if GOOGLE_CUDA
}

//===----------------------------------------------------------------------===//

XLA_RUNTIME_DEFINE_CUSTOM_CALL(
    Launch, FunctionWrapper<LaunchGraph>(), checks,
    CustomCall::Bind("xla.gpu.cuda.graph.launch")
        .UserData<const ServiceExecutableRunOptions*>()
        .UserData<const DebugOptions*>()
        .UserData<const std::string*>()
        .UserData<const std::vector<uint8_t>*>()
        .UserData<se::DeviceMemoryBase*>()
        .UserData<StreamExecutorKernels::Snapshot*>()
        .UserData<StreamExecutorConvRunners::Snapshot*>()
        .UserData<StreamExecutorGraphInstances::Snapshot*>()
        .UserData<CapturedFunctionExecutionCount::Snapshot*>()
        .UserData<GemmConfigs::Snapshot*>()
        .UserData<Executable*>()
        .UserData<NonAtomicallyUpgradeableRWLock*>()
        .UserData<ConcurrentRegionStatus*>()
        .RemainingArgs()
        .Attr<CustomCall::FunctionOrdinal>("capture"));

void RegisterGraphLaunchCustomCalls(
    runtime::DirectCustomCallRegistry& registry) {
  registry.Register("xla.gpu.cuda.graph.launch", Launch);
}

}  // namespace gpu
}  // namespace xla

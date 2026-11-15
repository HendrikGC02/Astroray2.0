// SPDX-License-Identifier: Apache-2.0
// OptixContext. optixInit fills the function table that optix_function_table_definition.h
// defines; that header must be included in exactly one translation unit, this one. The
// table is NVIDIA's own loader state and is filled identically on every call.
// Ref: OptiX 9.1 Programming Guide, "Initialization"; optix_stubs.h.
#include "device/cuda/optix_context.h"

#include <cuda_runtime_api.h>
#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>

#include <mutex>
#include <string>

namespace astroray::host {
namespace {

// The callback may run on any thread that calls OptiX.
struct LogSink {
  std::mutex mutex;
  std::string text;
};

void log_callback(unsigned level, const char* tag, const char* message, void* data) {
  auto* sink = static_cast<LogSink*>(data);
  std::lock_guard<std::mutex> lock(sink->mutex);
  sink->text += "[" + std::to_string(level) + "][" + (tag ? tag : "") + "] " + (message ? message : "") + "\n";
}

}  // namespace

struct OptixContext::Impl {
  OptixDeviceContext ctx = nullptr;
  cudaStream_t stream = nullptr;
  LogSink sink;
};

OptixContext::OptixContext(Device& device, uint32_t device_index)
    : device_(device), index_(device_index), impl_(std::make_unique<Impl>()) {
  try {
    bind();
    // optixInit fills NVIDIA's process-wide function table; two scene init threads must not
    // fill it at once. A failure leaves the flag unset so a later scene may retry.
    static std::once_flag init_once;
    std::call_once(init_once, [&] { check(optixInit(), "optixInit"); });
    OptixDeviceContextOptions options = {};
    options.logCallbackFunction = &log_callback;
    options.logCallbackData = &impl_->sink;
    options.logCallbackLevel = 3;
    // A null CUcontext means the current one, which is the primary context CudaDevice retained.
    check(optixDeviceContextCreate(nullptr, &options, &impl_->ctx), "optixDeviceContextCreate");
    if (cudaStreamCreateWithFlags(&impl_->stream, cudaStreamNonBlocking) != cudaSuccess)
      throw DeviceError("GPU device unavailable: OptiX cudaStreamCreate failed");
  } catch (...) {
    if (impl_->ctx) optixDeviceContextDestroy(impl_->ctx);
    throw;
  }
}

OptixContext::~OptixContext() {
  if (cudaSetDevice(static_cast<int>(index_)) == cudaSuccess && impl_->stream) cudaStreamSynchronize(impl_->stream);
  if (impl_->ctx) optixDeviceContextDestroy(impl_->ctx);
  if (impl_->stream) cudaStreamDestroy(impl_->stream);
}

void OptixContext::bind() const {
  const cudaError_t e = cudaSetDevice(static_cast<int>(index_));
  if (e != cudaSuccess) throw DeviceError(std::string("GPU device unavailable: cudaSetDevice: ") + cudaGetErrorString(e));
}

void* OptixContext::handle() const { return impl_->ctx; }
void* OptixContext::stream() const { return impl_->stream; }

void OptixContext::check(int result, const char* call) const {
  if (result == OPTIX_SUCCESS) return;
  std::string log;
  {
    std::lock_guard<std::mutex> lock(impl_->sink.mutex);
    log = impl_->sink.text;
  }
  throw DeviceError(std::string("GPU device unavailable: OptiX ") + call + " failed: " +
                    optixGetErrorName(static_cast<OptixResult>(result)) + " (" +
                    optixGetErrorString(static_cast<OptixResult>(result)) + ")" +
                    (log.empty() ? "" : "\nOptiX log:\n" + log));
}

void OptixContext::sync() const {
  const cudaError_t e = cudaStreamSynchronize(impl_->stream);
  if (e != cudaSuccess)
    throw DeviceError(DeviceError::sticky_text(std::string("cudaStreamSynchronize (OptiX): ") + cudaGetErrorString(e)), true);
}

}  // namespace astroray::host

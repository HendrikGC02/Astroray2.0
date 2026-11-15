// SPDX-License-Identifier: Apache-2.0
// TraversalPipeline: see optix_pipeline.h. One Variant is a module, its program groups, the linked
// pipeline and its SBT; the plain variant is always built, the motion variant on request.
// Ref: OptiX 9.1 Programming Guide (modules, program groups, pipelines, SBT, optixLaunch);
// docs/decisions/0002-kernel-architecture.md D11 (programs, pipeline options, SBT), D14, D15.
#include "device/cuda/optix_pipeline.h"

#include <cuda_runtime_api.h>
#include <optix.h>
#include <optix_stack_size.h>
#include <optix_stubs.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "device/cuda/module_loader.h"
#include "kernel/launch_config.h"

namespace astroray::host {
namespace {

// Programs, in SBT order: raygen closest, raygen shadow, miss, triangle hit group.
constexpr uint32_t kProgramCount = 4;
constexpr uint32_t kRecordBytes = OPTIX_SBT_RECORD_HEADER_SIZE;  // no record data: every program reads the launch params
static_assert(kRecordBytes % OPTIX_SBT_RECORD_ALIGNMENT == 0, "SBT records stay aligned back to back");

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct Variant {
  OptixModule module = nullptr;
  OptixProgramGroup groups[kProgramCount] = {};
  OptixPipeline pipeline = nullptr;
  DeviceBuffer sbt;
  TraversalPipeline::Times times;
};

}  // namespace

struct TraversalPipeline::Impl {
  OptixContext& ctx;
  Owner owner;
  ModuleImage image;
  Variant plain, motion;
  bool has_motion = false;
  DeviceBuffer params;

  Impl(OptixContext& c, Owner o) : ctx(c), owner(o) {}

  void destroy(Variant& v) {
    for (OptixProgramGroup g : v.groups)
      if (g) optixProgramGroupDestroy(g);
    if (v.pipeline) optixPipelineDestroy(v.pipeline);
    if (v.module) optixModuleDestroy(v.module);
    ctx.device().free(v.sbt);
    v = Variant();
  }

  // Throws on failure; the caller destroys what was built.
  void build(Variant& v, bool motion_blur) {
    auto* octx = static_cast<OptixDeviceContext>(ctx.handle());
    OptixPipelineCompileOptions popt = {};
    popt.usesMotionBlur = motion_blur ? 1 : 0;
    popt.traversableGraphFlags =
        OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING | OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    popt.numPayloadValues = 5;    // t, primitive, instance, u, v
    popt.numAttributeValues = 2;  // triangle barycentrics
    popt.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
    popt.pipelineLaunchParamsVariableName = "ar_params";
    popt.usesPrimitiveTypeFlags = static_cast<unsigned int>(OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE);

    OptixModuleCompileOptions mopt = {};
    mopt.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    mopt.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    mopt.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;

    auto t0 = std::chrono::steady_clock::now();
    ctx.check(optixModuleCreate(octx, &mopt, &popt, reinterpret_cast<const char*>(image.bytes.data()),
                                image.bytes.size(), nullptr, nullptr, &v.module),
              "optixModuleCreate");
    v.times.module_ms = ms_since(t0);

    t0 = std::chrono::steady_clock::now();
    OptixProgramGroupOptions gopt = {};
    OptixProgramGroupDesc desc[kProgramCount] = {};
    desc[0].kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    desc[0].raygen = {v.module, "__raygen__intersect_closest"};
    desc[1].kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    desc[1].raygen = {v.module, "__raygen__intersect_shadow"};
    desc[2].kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    desc[2].miss = {v.module, "__miss__traverse"};
    desc[3].kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;  // the triangle hit group; no any-hit, no intersection
    desc[3].hitgroup.moduleCH = v.module;
    desc[3].hitgroup.entryFunctionNameCH = "__closesthit__tri";
    ctx.check(optixProgramGroupCreate(octx, desc, kProgramCount, &gopt, nullptr, nullptr, v.groups),
              "optixProgramGroupCreate");

    OptixPipelineLinkOptions lopt = {};
    lopt.maxTraceDepth = 1;
    ctx.check(optixPipelineCreate(octx, &popt, &lopt, v.groups, kProgramCount, nullptr, nullptr, &v.pipeline),
              "optixPipelineCreate");
    OptixStackSizes ss = {};
    for (OptixProgramGroup g : v.groups) ctx.check(optixUtilAccumulateStackSizes(g, &ss, v.pipeline), "optixUtilAccumulateStackSizes");
    unsigned dc_traversal = 0, dc_state = 0, continuation = 0;
    ctx.check(optixUtilComputeStackSizes(&ss, lopt.maxTraceDepth, 0, 0, &dc_traversal, &dc_state, &continuation),
              "optixUtilComputeStackSizes");
    // Graph depth 2: the TLAS and one GAS (single-level instancing).
    ctx.check(optixPipelineSetStackSize(v.pipeline, dc_traversal, dc_state, continuation, 2), "optixPipelineSetStackSize");

    std::array<uint8_t, kProgramCount * kRecordBytes> records = {};
    for (uint32_t i = 0; i < kProgramCount; ++i)
      ctx.check(optixSbtRecordPackHeader(v.groups[i], records.data() + size_t(i) * kRecordBytes), "optixSbtRecordPackHeader");
    v.sbt = ctx.device().allocate(LedgerCategory::TABLES, records.size(), owner);
    ctx.device().upload(v.sbt, 0, records.data(), records.size());
    v.times.pipeline_ms = ms_since(t0);
  }

  void launch(const Variant& v, uint32_t raygen, uint64_t traversable, const DeviceBuffer& kg, uint32_t width) {
    if (width == 0) throw std::invalid_argument("TraversalPipeline launch: width is 0");
    if (traversable == 0) throw std::invalid_argument("TraversalPipeline launch: no traversable (build the TLAS first)");
    ctx.bind();
    const ar::OptixLaunchParams p{traversable, reinterpret_cast<uint64_t>(kg.ptr)};
    const auto stream = static_cast<cudaStream_t>(ctx.stream());
    const cudaError_t e = cudaMemcpyAsync(params.ptr, &p, sizeof p, cudaMemcpyHostToDevice, stream);
    if (e != cudaSuccess) throw DeviceError(std::string("GPU device unavailable: cudaMemcpyAsync (OptiX params): ") + cudaGetErrorString(e));
    const CUdeviceptr base = reinterpret_cast<CUdeviceptr>(v.sbt.ptr);
    OptixShaderBindingTable sbt = {};
    sbt.raygenRecord = base + raygen * kRecordBytes;
    sbt.missRecordBase = base + 2 * kRecordBytes;
    sbt.missRecordStrideInBytes = kRecordBytes;
    sbt.missRecordCount = 1;
    sbt.hitgroupRecordBase = base + 3 * kRecordBytes;
    sbt.hitgroupRecordStrideInBytes = kRecordBytes;
    sbt.hitgroupRecordCount = 1;
    ctx.check(optixLaunch(v.pipeline, stream, reinterpret_cast<CUdeviceptr>(params.ptr), sizeof p, &sbt, width, 1, 1),
              "optixLaunch");
  }
};

TraversalPipeline::TraversalPipeline(OptixContext& context, Owner owner, const std::string& cache_dir)
    : impl_(std::make_unique<Impl>(context, owner)) {
  if (cache_dir.empty()) throw std::invalid_argument("TraversalPipeline: cache_dir is empty");
  context.bind();
  try {
    std::filesystem::create_directories(cache_dir);
    context.check(optixDeviceContextSetCacheLocation(static_cast<OptixDeviceContext>(context.handle()), cache_dir.c_str()),
                  "optixDeviceContextSetCacheLocation");
    impl_->image = load_module_image("optix_traverse");
    impl_->params = context.device().allocate(LedgerCategory::TABLES, sizeof(ar::OptixLaunchParams), owner);
    impl_->build(impl_->plain, false);
  } catch (...) {
    impl_->destroy(impl_->plain);
    context.device().free(impl_->params);
    throw;
  }
}

TraversalPipeline::~TraversalPipeline() {
  impl_->ctx.bind();
  impl_->ctx.sync();
  impl_->destroy(impl_->plain);
  impl_->destroy(impl_->motion);
  impl_->ctx.device().free(impl_->params);
}

void TraversalPipeline::create_motion_variant() {
  if (impl_->has_motion) return;
  impl_->ctx.bind();
  try {
    impl_->build(impl_->motion, true);
  } catch (...) {
    impl_->destroy(impl_->motion);
    throw;
  }
  impl_->has_motion = true;
}

bool TraversalPipeline::has_motion_variant() const { return impl_->has_motion; }
TraversalPipeline::Times TraversalPipeline::times() const { return impl_->plain.times; }
TraversalPipeline::Times TraversalPipeline::motion_times() const { return impl_->motion.times; }

void TraversalPipeline::launch_closest(uint64_t traversable, const DeviceBuffer& kg, uint32_t width) {
  impl_->launch(impl_->plain, 0, traversable, kg, width);
}
void TraversalPipeline::launch_shadow(uint64_t traversable, const DeviceBuffer& kg, uint32_t width) {
  impl_->launch(impl_->plain, 1, traversable, kg, width);
}

}  // namespace astroray::host

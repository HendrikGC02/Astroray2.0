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
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "device/cuda/module_loader.h"
#include "device/cuda/optix_accel.h"
#include "kernel/launch_config.h"

namespace astroray::host {
namespace {

// Program groups, in SBT order: raygen closest, raygen shadow, miss, then one hit group per
// instance sbtOffset (kSbtTriangle, kSbtLampDisk, kSbtLampSphere of optix_accel.h).
constexpr uint32_t kHitFirst = 3;
constexpr uint32_t kProgramCount = kHitFirst + kSbtHitGroups;
constexpr uint32_t kRecordBytes = OPTIX_SBT_RECORD_HEADER_SIZE;  // no record data: every program reads the launch params
static_assert(kRecordBytes % OPTIX_SBT_RECORD_ALIGNMENT == 0, "SBT records stay aligned back to back");

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct Variant {
  OptixModule module = nullptr;
  OptixModule builtin_sphere = nullptr;  // the built-in sphere intersection of the lamp spheres
  OptixProgramGroup groups[kProgramCount] = {};
  OptixPipeline pipeline = nullptr;
  DeviceBuffer sbt;
  TraversalPipeline::Times times;
};

}  // namespace

struct TraversalPipeline::Impl {
  OptixContext& ctx;
  Device& device;  // the SBT and launch parameters live here
  cudaStream_t stream;
  Owner owner;
  ModuleImage image;
  Variant plain, motion;
  bool has_motion = false;
  DeviceBuffer params;
  ar::OptixLaunchParams current{};  // what `params` holds; zero until set_params

  Impl(OptixContext& c, Device& d, void* s, Owner o) : ctx(c), device(d), stream(static_cast<cudaStream_t>(s)), owner(o) {}

  void sync() const {
    const cudaError_t e = cudaStreamSynchronize(stream);
    if (e != cudaSuccess)
      throw DeviceError(DeviceError::sticky_text(std::string("cudaStreamSynchronize (OptiX launch): ") + cudaGetErrorString(e)), true);
  }

  void destroy(Variant& v) {
    // Dependents first: pipeline, then the program groups it links, then the module they use.
    if (v.pipeline) optixPipelineDestroy(v.pipeline);
    for (OptixProgramGroup g : v.groups)
      if (g) optixProgramGroupDestroy(g);
    if (v.module) optixModuleDestroy(v.module);
    if (v.builtin_sphere) optixModuleDestroy(v.builtin_sphere);
    // The OptiX objects are gone even when the free below throws (a dead device).
    DeviceBuffer sbt = v.sbt;
    v = Variant();
    device.free(sbt);
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
    // Triangles (meshes, rectangle lamps), custom primitives (disk lamps), built-in spheres (sphere lamps).
    popt.usesPrimitiveTypeFlags = static_cast<unsigned int>(OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE |
                                                            OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM |
                                                            OPTIX_PRIMITIVE_TYPE_FLAGS_SPHERE);

    OptixModuleCompileOptions mopt = {};
    mopt.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    mopt.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    mopt.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;

    auto t0 = std::chrono::steady_clock::now();
    ctx.check(optixModuleCreate(octx, &mopt, &popt, reinterpret_cast<const char*>(image.bytes.data()),
                                image.bytes.size(), nullptr, nullptr, &v.module),
              "optixModuleCreate");
    OptixBuiltinISOptions bopt = {};
    bopt.builtinISModuleType = OPTIX_PRIMITIVE_TYPE_SPHERE;
    bopt.usesMotionBlur = motion_blur ? 1 : 0;
    bopt.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;  // as the lamp GAS of OptixAccel
    ctx.check(optixBuiltinISModuleGet(octx, &mopt, &popt, &bopt, &v.builtin_sphere), "optixBuiltinISModuleGet");
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
    // No any-hit anywhere (every lamp and material is opaque here).
    OptixProgramGroupDesc& tri = desc[kHitFirst + kSbtTriangle];  // meshes and rectangle lamps
    tri.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    tri.hitgroup.moduleCH = v.module;
    tri.hitgroup.entryFunctionNameCH = "__closesthit__tri";
    OptixProgramGroupDesc& disk = desc[kHitFirst + kSbtLampDisk];  // custom primitive
    disk.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    disk.hitgroup.moduleIS = v.module;
    disk.hitgroup.entryFunctionNameIS = "__intersection__lamp";
    disk.hitgroup.moduleCH = v.module;
    disk.hitgroup.entryFunctionNameCH = "__closesthit__lamp";
    OptixProgramGroupDesc& sphere = desc[kHitFirst + kSbtLampSphere];  // built-in sphere
    sphere.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    sphere.hitgroup.moduleIS = v.builtin_sphere;
    sphere.hitgroup.entryFunctionNameIS = nullptr;
    sphere.hitgroup.moduleCH = v.module;
    sphere.hitgroup.entryFunctionNameCH = "__closesthit__lamp";
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
    v.sbt = device.allocate(LedgerCategory::TABLES, records.size(), owner);
    device.upload(v.sbt, 0, records.data(), records.size());
    v.times.pipeline_ms = ms_since(t0);
  }

  void set_params(uint64_t traversable, const DeviceBuffer& kg) {
    if (traversable == 0) throw std::invalid_argument("TraversalPipeline launch: no traversable (build the TLAS first)");
    const ar::OptixLaunchParams p{traversable, reinterpret_cast<uint64_t>(kg.ptr)};
    if (p.traversable == current.traversable && p.kernel_globals == current.kernel_globals) return;
    device.upload(params, 0, &p, sizeof p);  // in stream order, after every launch that read the old values
    current = p;
  }

  void launch(const Variant& v, uint32_t raygen, uint32_t width) {
    if (width == 0) throw std::invalid_argument("TraversalPipeline launch: width is 0");
    if (current.traversable == 0) throw std::logic_error("TraversalPipeline launch: no launch parameters (set_params)");
    ctx.bind();
    const CUdeviceptr base = reinterpret_cast<CUdeviceptr>(v.sbt.ptr);
    OptixShaderBindingTable sbt = {};
    sbt.raygenRecord = base + raygen * kRecordBytes;
    sbt.missRecordBase = base + 2 * kRecordBytes;
    sbt.missRecordStrideInBytes = kRecordBytes;
    sbt.missRecordCount = 1;
    sbt.hitgroupRecordBase = base + kHitFirst * kRecordBytes;
    sbt.hitgroupRecordStrideInBytes = kRecordBytes;
    sbt.hitgroupRecordCount = kSbtHitGroups;
    ctx.check(optixLaunch(v.pipeline, stream, reinterpret_cast<CUdeviceptr>(params.ptr), sizeof(ar::OptixLaunchParams), &sbt, width, 1, 1),
              "optixLaunch");
  }
};

TraversalPipeline::TraversalPipeline(OptixContext& context, Device& device, void* stream, Owner owner)
    : impl_(std::make_unique<Impl>(context, device, stream, owner)) {
  if (!stream) throw std::invalid_argument("TraversalPipeline: no stream");
  context.bind();
  try {
    impl_->image = load_module_image("optix_traverse");
    impl_->params = device.allocate(LedgerCategory::TABLES, sizeof(ar::OptixLaunchParams), owner);
    impl_->build(impl_->plain, false);
  } catch (...) {
    impl_->destroy(impl_->plain);
    device.free(impl_->params);
    throw;
  }
}

namespace {
// A destructor never throws: a failed teardown step (a dead device) is printed, never dropped, and
// the steps after it still run.
template <class F>
void teardown(const char* step, F&& f) noexcept {
  try {
    f();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "astroray: TraversalPipeline teardown (%s): %s\n", step, e.what());
  } catch (...) {
    std::fprintf(stderr, "astroray: TraversalPipeline teardown (%s): unknown exception\n", step);
  }
}
}  // namespace

TraversalPipeline::~TraversalPipeline() {
  Impl& s = *impl_;
  teardown("bind", [&] { s.ctx.bind(); });
  teardown("sync", [&] { s.sync(); });  // a launch in flight still reads the SBT and the parameters
  teardown("plain variant", [&] { s.destroy(s.plain); });
  teardown("motion variant", [&] { s.destroy(s.motion); });
  teardown("launch parameters", [&] { s.device.free(s.params); });
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

void TraversalPipeline::set_params(uint64_t traversable, const DeviceBuffer& kg) { impl_->set_params(traversable, kg); }
void TraversalPipeline::launch_closest(uint32_t width) { impl_->launch(impl_->plain, 0, width); }
void TraversalPipeline::launch_shadow(uint32_t width) { impl_->launch(impl_->plain, 1, width); }
void TraversalPipeline::launch_closest(uint64_t traversable, const DeviceBuffer& kg, uint32_t width) {
  set_params(traversable, kg);
  launch_closest(width);
}
void TraversalPipeline::launch_shadow(uint64_t traversable, const DeviceBuffer& kg, uint32_t width) {
  set_params(traversable, kg);
  launch_shadow(width);
}

}  // namespace astroray::host

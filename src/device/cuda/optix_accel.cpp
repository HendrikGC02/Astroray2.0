// SPDX-License-Identifier: Apache-2.0
// OptiX acceleration structures. GAS: build with ALLOW_COMPACTION into a temporary arena
// (one allocation per batch, one stream sync per phase), read the compacted sizes, copy
// each into its own exact-size buffer. TLAS: instance array rebuilt every generation.
// Ref: OptiX 9.1 Programming Guide, "Acceleration structures" (optixAccelComputeMemoryUsage,
// optixAccelBuild, OPTIX_PROPERTY_TYPE_COMPACTED_SIZE, optixAccelCompact); the batching
// follows Cycles src/bvh/optix.cpp and pbrt-v4 src/pbrt/gpu/optix/aggregate.cpp.
#include "device/cuda/optix_accel.h"

#include <cuda.h>
#include <optix.h>
#include <optix_stubs.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel/geom/lamp_intersect.h"

namespace astroray::host {
namespace {

// Arena size at which a batch is flushed (a single larger GAS gets a batch of its own).
constexpr uint64_t kBatchBytes = 256ull << 20;

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
CUdeviceptr dptr(const DeviceBuffer& b) { return static_cast<CUdeviceptr>(reinterpret_cast<uintptr_t>(b.ptr)); }

struct Job {
  ar_key key = 0;
  const UploadedMesh* mesh = nullptr;
  CUdeviceptr vertex_ptr = 0;
  uint32_t geometry_flags = OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT;  // all M1a materials are opaque
  OptixBuildInput input = {};
  OptixAccelBufferSizes sizes = {};
  uint64_t out_off = 0, temp_off = 0, size_off = 0;
  OptixTraversableHandle handle = 0;
  uint64_t compacted = 0;
};

}  // namespace

OptixAccel::OptixAccel(OptixContext& context, Owner owner) : context_(context), owner_(owner) {}

OptixAccel::~OptixAccel() {
  for (auto& kv : gas_) free_gas(kv.second);
  for (Gas& g : lamp_gas_) free_gas(g);
  context_.device().free(tlas_instances_);
  context_.device().free(tlas_buffer_);
}

void OptixAccel::free_gas(Gas& g) { context_.device().free(g.buffer); }

uint64_t OptixAccel::gas_handle(ar_key key) const {
  auto it = gas_.find(key);
  return it == gas_.end() ? 0 : it->second.handle;
}
uint64_t OptixAccel::gas_bytes(ar_key key) const {
  auto it = gas_.find(key);
  return it == gas_.end() ? 0 : it->second.buffer.bytes;
}
uint64_t OptixAccel::gas_emitted_size(ar_key key) const {
  auto it = gas_.find(key);
  return it == gas_.end() ? 0 : it->second.emitted;
}
uint64_t OptixAccel::gas_uncompacted_bytes(ar_key key) const {
  auto it = gas_.find(key);
  return it == gas_.end() ? 0 : it->second.uncompacted;
}

std::vector<OptixAccel::TlasInstance> OptixAccel::read_tlas_instances() const {
  std::vector<OptixInstance> raw(tlas_instance_count_);
  if (raw.empty()) return {};
  context_.device().download(raw.data(), tlas_instances_, 0, raw.size() * sizeof(OptixInstance));
  std::vector<TlasInstance> out;
  for (const OptixInstance& o : raw) {
    TlasInstance t{o.instanceId, o.visibilityMask, o.flags, o.sbtOffset, {}, o.traversableHandle};
    std::memcpy(t.transform, o.transform, sizeof t.transform);
    out.push_back(t);
  }
  return out;
}

uint64_t OptixAccel::gas_bytes_total() const {
  uint64_t n = 0;
  for (auto& kv : gas_) n += kv.second.buffer.bytes;
  return n;
}

uint32_t OptixAccel::update_gas(const SceneUpload& upload) {
  context_.bind();
  Device& dev = context_.device();
  const auto meshes = upload.meshes();

  // Keys that left the scene (or lost every triangle) give their GAS back first.
  for (auto it = gas_.begin(); it != gas_.end();) {
    const UploadedMesh* m = upload.find(it->first);
    if (m && m->view.triangle_count > 0) {
      ++it;
      continue;
    }
    free_gas(it->second);
    it = gas_.erase(it);
    tlas_handle_ = 0;  // the TLAS references the freed GAS
  }

  std::vector<Job> jobs;
  for (const UploadedMesh* m : meshes) {
    if (m->view.triangle_count == 0) continue;
    auto it = gas_.find(m->key);
    if (it != gas_.end() && it->second.version == m->version) continue;
    Job j;
    j.key = m->key, j.mesh = m;
    jobs.push_back(j);
  }
  if (jobs.empty()) return 0;

  const auto t0 = std::chrono::steady_clock::now();
  OptixAccelBuildOptions options = {};
  options.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
  options.operation = OPTIX_BUILD_OPERATION_BUILD;
  auto ctx = static_cast<OptixDeviceContext>(context_.handle());
  auto stream = static_cast<CUstream>(context_.stream());
  // Device::free is not assumed synchronous: error paths wait for the stream first.
  auto quiet_sync = [&] {
    try {
      context_.sync();
    } catch (...) {
    }
  };

  for (Job& j : jobs) {
    j.vertex_ptr = static_cast<CUdeviceptr>(j.mesh->view.positions);
    OptixBuildInputTriangleArray& tri = j.input.triangleArray;
    j.input.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    tri.vertexBuffers = &j.vertex_ptr;
    tri.numVertices = j.mesh->view.vertex_count;
    tri.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    tri.vertexStrideInBytes = 12;
    tri.indexBuffer = static_cast<CUdeviceptr>(j.mesh->view.tri_vertex);
    tri.numIndexTriplets = j.mesh->view.triangle_count;
    tri.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
    tri.indexStrideInBytes = 12;
    tri.flags = &j.geometry_flags;
    tri.numSbtRecords = 1;
    context_.check(optixAccelComputeMemoryUsage(ctx, &options, &j.input, 1, &j.sizes), "optixAccelComputeMemoryUsage");
  }

  dev.synchronize();  // the uploads the builds read have landed
  uint32_t built = 0;
  for (size_t first = 0; first < jobs.size();) {
    // One batch: jobs [first, last) laid out in one arena.
    uint64_t cur = 0;
    size_t last = first;
    while (last < jobs.size() && (last == first || cur < kBatchBytes)) {
      Job& j = jobs[last];
      j.out_off = align_up(cur, OPTIX_ACCEL_BUFFER_BYTE_ALIGNMENT);
      j.temp_off = align_up(j.out_off + j.sizes.outputSizeInBytes, OPTIX_ACCEL_BUFFER_BYTE_ALIGNMENT);
      j.size_off = align_up(j.temp_off + j.sizes.tempSizeInBytes, OPTIX_ACCEL_BUFFER_BYTE_ALIGNMENT);
      cur = j.size_off + 8;
      ++last;
    }
    DeviceBuffer arena = dev.allocate(LedgerCategory::ACCELERATION, cur, owner_);
    try {
      const CUdeviceptr base = dptr(arena);
      for (size_t i = first; i < last; ++i) {
        Job& j = jobs[i];
        OptixAccelEmitDesc emit = {};
        emit.result = base + j.size_off;
        emit.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
        context_.check(optixAccelBuild(ctx, stream, &options, &j.input, 1, base + j.temp_off, j.sizes.tempSizeInBytes,
                                       base + j.out_off, j.sizes.outputSizeInBytes, &j.handle, &emit, 1),
                       "optixAccelBuild");
      }
      context_.sync();  // one sync per batch
      std::vector<Gas> fresh(last - first);
      try {
        for (size_t i = first; i < last; ++i) {
          Job& j = jobs[i];
          dev.download(&j.compacted, arena, j.size_off, 8);
          Gas& g = fresh[i - first];
          g.buffer = dev.allocate(LedgerCategory::ACCELERATION, j.compacted, owner_);
          g.version = j.mesh->version;
          OptixTraversableHandle h = 0;
          context_.check(optixAccelCompact(ctx, stream, j.handle, dptr(g.buffer), j.compacted, &h), "optixAccelCompact");
          g.handle = h;
        }
        context_.sync();
      } catch (...) {
        quiet_sync();  // in-flight compactions write into these buffers
        for (Gas& g : fresh) free_gas(g);
        throw;
      }
      for (size_t i = first; i < last; ++i) {
        auto it = gas_.find(jobs[i].key);
        if (it != gas_.end()) free_gas(it->second);
        tlas_handle_ = 0;  // a replaced GAS leaves the old TLAS pointing at freed memory
        fresh[i - first].emitted = jobs[i].compacted;
        fresh[i - first].uncompacted = jobs[i].sizes.outputSizeInBytes;
        gas_[jobs[i].key] = fresh[i - first];
        ++built;
      }
    } catch (...) {
      quiet_sync();  // in-flight builds read and write the arena
      dev.free(arena);
      throw;
    }
    dev.free(arena);
    first = last;
  }
  gas_builds_ += built;
  last_gas_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return built;
}

// The unit shape of one lamp type (kernel/geom/lamp_intersect.h): two triangles, a flat box for the
// custom disk, a built-in sphere. One GAS per shape, built uncompacted (a handful of primitives).
// Ref: OptiX 9.1 Programming Guide, "Custom primitives" (OptixBuildInputCustomPrimitiveArray) and
// "Spheres" (OptixBuildInputSphereArray, singleRadius).
void OptixAccel::ensure_lamp_gas(uint8_t lamp_shape) {
  const ar::LampShape shape = ar::LampShape(lamp_shape);
  Gas& g = lamp_gas_[lamp_shape];
  if (g.handle) return;
  Device& dev = context_.device();
  const float verts[12] = {-0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 0.0f, -0.5f, 0.5f, 0.0f};
  const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
  const float pad = ar::kLampDiskBoundPad;
  const OptixAabb box = {-1.0f, -1.0f, -pad, 1.0f, 1.0f, pad};
  const float centre[3] = {0.0f, 0.0f, 0.0f}, radius = 1.0f;
  const uint32_t geometry_flags = OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT;

  DeviceBuffer a, b, c;  // inputs; temp and output below
  DeviceBuffer out, temp;
  auto sync_quiet = [&] {
    try {
      context_.sync();
    } catch (...) {
    }
  };
  auto release_all = [&] {
    dev.free(a), dev.free(b), dev.free(c), dev.free(temp), dev.free(out);
  };
  try {
    auto put = [&](DeviceBuffer& buf, const void* src, uint64_t bytes) {
      buf = dev.allocate(LedgerCategory::TABLES, bytes, owner_);
      dev.upload(buf, 0, src, bytes);
    };
    CUdeviceptr vptr = 0, rptr = 0;
    OptixBuildInput in = {};
    if (shape == ar::LampShape::RECT) {
      put(a, verts, sizeof verts);
      put(b, idx, sizeof idx);
      vptr = dptr(a);
      OptixBuildInputTriangleArray& t = in.triangleArray;
      in.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
      t.vertexBuffers = &vptr;
      t.numVertices = 4;
      t.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
      t.vertexStrideInBytes = 12;
      t.indexBuffer = dptr(b);
      t.numIndexTriplets = 2;
      t.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
      t.indexStrideInBytes = 12;
      t.flags = &geometry_flags;
      t.numSbtRecords = 1;
    } else if (shape == ar::LampShape::DISK) {
      put(a, &box, sizeof box);
      vptr = dptr(a);
      OptixBuildInputCustomPrimitiveArray& cp = in.customPrimitiveArray;
      in.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
      cp.aabbBuffers = &vptr;
      cp.numPrimitives = 1;
      cp.strideInBytes = sizeof(OptixAabb);
      cp.flags = &geometry_flags;
      cp.numSbtRecords = 1;
    } else {
      put(a, centre, sizeof centre);
      put(b, &radius, sizeof radius);
      vptr = dptr(a), rptr = dptr(b);
      OptixBuildInputSphereArray& sp = in.sphereArray;
      in.type = OPTIX_BUILD_INPUT_TYPE_SPHERES;
      sp.vertexBuffers = &vptr;
      sp.numVertices = 1;
      sp.radiusBuffers = &rptr;
      sp.singleRadius = 1;
      sp.flags = &geometry_flags;
      sp.numSbtRecords = 1;
    }
    OptixAccelBuildOptions options = {};
    options.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;  // the pipeline's built-in sphere module matches
    options.operation = OPTIX_BUILD_OPERATION_BUILD;
    auto ctx = static_cast<OptixDeviceContext>(context_.handle());
    OptixAccelBufferSizes sizes = {};
    context_.check(optixAccelComputeMemoryUsage(ctx, &options, &in, 1, &sizes), "optixAccelComputeMemoryUsage");
    out = dev.allocate(LedgerCategory::ACCELERATION, sizes.outputSizeInBytes, owner_);
    temp = dev.allocate(LedgerCategory::ACCELERATION, std::max<uint64_t>(sizes.tempSizeInBytes, 1), owner_);
    dev.synchronize();  // the uploads the build reads have landed
    OptixTraversableHandle h = 0;
    context_.check(optixAccelBuild(ctx, static_cast<CUstream>(context_.stream()), &options, &in, 1, dptr(temp), temp.bytes,
                                   dptr(out), out.bytes, &h, nullptr, 0),
                   "optixAccelBuild");
    context_.sync();
    g.handle = h;
    g.buffer = out;
    out = DeviceBuffer();
  } catch (...) {
    sync_quiet();  // an in-flight build reads and writes these buffers
    release_all();
    throw;
  }
  release_all();
}

void OptixAccel::build_tlas(const SceneUpload& upload, const InstanceTable& table,
                            const std::vector<ar::LightRecord>& lights) {
  context_.bind();
  Device& dev = context_.device();
  std::vector<OptixInstance> inst;
  inst.reserve(table.instances.size());
  for (size_t i = 0; i < table.instances.size(); ++i) {
    const Instance& s = table.instances[i];
    if (s.light_index != ar::kNoLightIndex) {
      if (s.light_index >= lights.size())
        throw std::runtime_error("instance " + std::to_string(i) + " names light " + std::to_string(s.light_index) +
                                 " but the light table has " + std::to_string(lights.size()));
      const ar::LightRecord& rec = lights[s.light_index];
      const ar::LampShape shape = ar::lamp_shape(rec);
      if (shape == ar::LampShape::INVALID)
        throw std::runtime_error("instance " + std::to_string(i) + " names light " + std::to_string(s.light_index) +
                                 " which is not a lamp (a point lamp needs radius 0, a sphere lamp radius > 0)");
      if (s.mask & ar::kShadowMaskBit)
        throw std::runtime_error("instance " + std::to_string(i) + " is a lamp whose mask carries the shadow bit (0004 D8: a lamp never blocks a shadow ray)");
      if (shape == ar::LampShape::NONE) continue;  // a delta point lamp has no surface to hit
      ensure_lamp_gas(uint8_t(shape));
      OptixInstance o = {};
      ar::lamp_geometry_xform(rec, shape, o.transform);
      o.instanceId = uint32_t(i);
      o.sbtOffset = shape == ar::LampShape::DISK ? kSbtLampDisk : shape == ar::LampShape::SPHERE ? kSbtLampSphere : kSbtTriangle;
      o.visibilityMask = s.mask;  // never the shadow bit: a lamp blocks no shadow ray (0004 D8)
      o.flags = OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT;
      o.traversableHandle = lamp_gas_[size_t(shape)].handle;
      inst.push_back(o);
      continue;
    }
    const UploadedMesh* m = upload.find(s.geom_key);
    if (!m)
      throw std::runtime_error("instance " + std::to_string(i) + " references geometry key " + std::to_string(s.geom_key) +
                               " which was never uploaded");
    if (m->view.triangle_count == 0) continue;  // an empty mesh has nothing to hit
    auto g = gas_.find(s.geom_key);
    if (g == gas_.end() || g->second.version != m->version)
      throw std::runtime_error("geometry key " + std::to_string(s.geom_key) + " (instance " + std::to_string(i) +
                               ") has " + (g == gas_.end() ? "no GAS" : "a GAS older than its mesh data") +
                               ": update_gas must run before build_tlas");
    const uint64_t handle = g->second.handle;
    OptixInstance o = {};
    std::memcpy(o.transform, table.centre(uint32_t(i)).m, sizeof o.transform);
    o.instanceId = uint32_t(i);  // the index of its DeviceInstanceRecord, what a hit reports
    o.sbtOffset = kSbtTriangle;  // the mesh hit group (0002 D11)
    o.visibilityMask = s.mask;
    o.flags = OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT;  // every material is opaque in M1a
    o.traversableHandle = handle;
    inst.push_back(o);
  }
  DeviceBuffer next, next_inst;
  uint64_t next_handle = 0;
  if (!inst.empty()) {
    DeviceBuffer ibuf = dev.allocate(LedgerCategory::TABLES, inst.size() * sizeof(OptixInstance), owner_);
    DeviceBuffer arena;
    next_inst = ibuf;
    try {
      dev.upload(ibuf, 0, inst.data(), ibuf.bytes);
      OptixBuildInput in = {};
      in.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
      in.instanceArray.instances = dptr(ibuf);
      in.instanceArray.numInstances = uint32_t(inst.size());
      OptixAccelBuildOptions options = {};
      options.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
      options.operation = OPTIX_BUILD_OPERATION_BUILD;
      auto ctx = static_cast<OptixDeviceContext>(context_.handle());
      OptixAccelBufferSizes sizes = {};
      context_.check(optixAccelComputeMemoryUsage(ctx, &options, &in, 1, &sizes), "optixAccelComputeMemoryUsage");
      next = dev.allocate(LedgerCategory::ACCELERATION, sizes.outputSizeInBytes, owner_);
      arena = dev.allocate(LedgerCategory::ACCELERATION, std::max<uint64_t>(sizes.tempSizeInBytes, 1), owner_);
      dev.synchronize();
      OptixTraversableHandle h = 0;
      context_.check(optixAccelBuild(ctx, static_cast<CUstream>(context_.stream()), &options, &in, 1, dptr(arena),
                                     arena.bytes, dptr(next), next.bytes, &h, nullptr, 0),
                     "optixAccelBuild");
      context_.sync();
      next_handle = h;
    } catch (...) {
      try {
        context_.sync();  // an in-flight build reads and writes these buffers
      } catch (...) {
      }
      dev.free(ibuf);
      dev.free(arena);
      dev.free(next);
      throw;
    }
    dev.free(arena);
  }
  dev.free(tlas_buffer_);
  dev.free(tlas_instances_);
  tlas_instances_ = next_inst;
  tlas_instance_count_ = uint32_t(inst.size());
  tlas_buffer_ = next;
  tlas_handle_ = next_handle;
  ++tlas_builds_;
}

}  // namespace astroray::host

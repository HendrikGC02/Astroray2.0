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
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

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
        for (Gas& g : fresh) free_gas(g);
        throw;
      }
      for (size_t i = first; i < last; ++i) {
        auto it = gas_.find(jobs[i].key);
        if (it != gas_.end()) free_gas(it->second);
        gas_[jobs[i].key] = fresh[i - first];
        ++built;
      }
    } catch (...) {
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

void OptixAccel::build_tlas(const SceneUpload& upload, const InstanceTable& table) {
  context_.bind();
  Device& dev = context_.device();
  std::vector<OptixInstance> inst;
  inst.reserve(table.instances.size());
  for (size_t i = 0; i < table.instances.size(); ++i) {
    const Instance& s = table.instances[i];
    const uint64_t handle = gas_handle(s.geom_key);
    if (!upload.find(s.geom_key))
      throw std::runtime_error("instance " + std::to_string(i) + " references geometry key " + std::to_string(s.geom_key) +
                               " which was never uploaded");
    if (!handle) continue;  // a mesh without triangles has nothing to hit
    OptixInstance o = {};
    std::memcpy(o.transform, table.centre(uint32_t(i)).m, sizeof o.transform);
    o.instanceId = uint32_t(i);  // the index of its DeviceInstanceRecord, what a hit reports
    o.sbtOffset = 0;             // one triangle hit group (0002 D11)
    o.visibilityMask = s.mask;
    o.flags = OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT;  // every material is opaque in M1a
    o.traversableHandle = handle;
    inst.push_back(o);
  }
  DeviceBuffer next;
  uint64_t next_handle = 0;
  if (!inst.empty()) {
    DeviceBuffer ibuf = dev.allocate(LedgerCategory::TABLES, inst.size() * sizeof(OptixInstance), owner_);
    DeviceBuffer arena;
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
      dev.free(ibuf);
      dev.free(arena);
      dev.free(next);
      throw;
    }
    dev.free(ibuf);
    dev.free(arena);
  }
  dev.free(tlas_buffer_);
  tlas_buffer_ = next;
  tlas_handle_ = next_handle;
  ++tlas_builds_;
}

}  // namespace astroray::host

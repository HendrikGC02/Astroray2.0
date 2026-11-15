// SPDX-License-Identifier: Apache-2.0
// OptiX acceleration structures. GAS: build with ALLOW_COMPACTION into a temporary arena
// (one allocation per batch, one stream sync per phase), read the compacted sizes, copy
// each into its own exact-size buffer. TLAS: one instance array per snapshot.
// Ref: OptiX 9.1 Programming Guide, "Acceleration structures" (optixAccelComputeMemoryUsage,
// optixAccelBuild, OPTIX_PROPERTY_TYPE_COMPACTED_SIZE, optixAccelCompact); the batching
// follows Cycles src/bvh/optix.cpp and pbrt-v4 src/pbrt/gpu/optix/aggregate.cpp.
#include "device/cuda/optix_accel.h"

#include <cuda.h>
#include <optix.h>
#include <optix_stubs.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <cstring>
#include <set>
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
  const OptixGasJob* in = nullptr;
  CUdeviceptr vertex_ptr = 0;
  uint32_t geometry_flags = OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT;  // all M1a materials are opaque
  OptixBuildInput input = {};
  OptixAccelBufferSizes sizes = {};
  uint64_t out_off = 0, temp_off = 0, size_off = 0;
  OptixTraversableHandle handle = 0;
  uint64_t compacted = 0;
};

// Teardown cannot throw; a failure is printed, never dropped.
void release(Device* device, DeviceBuffer& b, const char* what) {
  if (!device) return;
  try {
    device->free(b);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "astroray: freeing %s failed: %s\n", what, e.what());
  }
}

}  // namespace

OptixGas::~OptixGas() { release(device, buffer, "a geometry acceleration structure"); }

OptixTlas::~OptixTlas() {
  release(device, instances, "an instance array");
  release(device, buffer, "an instance acceleration structure");
}

std::vector<std::shared_ptr<OptixGas>> OptixAccelBuilder::build_gas(const std::vector<OptixGasJob>& in_jobs) {
  std::vector<std::shared_ptr<OptixGas>> result(in_jobs.size());
  if (in_jobs.empty()) return result;
  context_.bind();
  Device& dev = context_.device();
  std::vector<Job> jobs(in_jobs.size());
  for (size_t i = 0; i < jobs.size(); ++i) jobs[i].in = &in_jobs[i];

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
    const DeviceMeshView& view = j.in->view;
    j.vertex_ptr = static_cast<CUdeviceptr>(view.positions);
    OptixBuildInputTriangleArray& tri = j.input.triangleArray;
    j.input.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    tri.vertexBuffers = &j.vertex_ptr;
    tri.numVertices = view.vertex_count;
    tri.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    tri.vertexStrideInBytes = 12;
    tri.indexBuffer = static_cast<CUdeviceptr>(view.tri_vertex);
    tri.numIndexTriplets = view.triangle_count;
    tri.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
    tri.indexStrideInBytes = 12;
    tri.flags = &j.geometry_flags;
    tri.numSbtRecords = 1;
    context_.check(optixAccelComputeMemoryUsage(ctx, &options, &j.input, 1, &j.sizes), "optixAccelComputeMemoryUsage");
  }

  dev.synchronize();  // the uploads the builds read have landed
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
      try {
        for (size_t i = first; i < last; ++i) {
          Job& j = jobs[i];
          dev.download(&j.compacted, arena, j.size_off, 8);
          auto g = std::make_shared<OptixGas>();
          g->device = &dev;
          g->buffer = dev.allocate(LedgerCategory::ACCELERATION, j.compacted, owner_);
          result[i] = g;
          OptixTraversableHandle h = 0;
          context_.check(optixAccelCompact(ctx, stream, j.handle, dptr(g->buffer), j.compacted, &h), "optixAccelCompact");
          g->handle = h;
          g->emitted = j.compacted;
          g->uncompacted = j.sizes.outputSizeInBytes;
          g->record = j.in->record;
        }
        context_.sync();
      } catch (...) {
        quiet_sync();  // in-flight compactions write into these buffers
        result.clear();
        dev.free(arena);
        throw;
      }
    } catch (...) {
      quiet_sync();  // in-flight builds read and write the arena
      dev.free(arena);
      throw;
    }
    dev.free(arena);
    first = last;
  }
  return result;
}

// The unit shape of one lamp type (kernel/geom/lamp_intersect.h): two triangles, a flat box for the
// custom disk, a built-in sphere. One GAS per shape, built uncompacted (a handful of primitives).
// Ref: OptiX 9.1 Programming Guide, "Custom primitives" (OptixBuildInputCustomPrimitiveArray) and
// "Spheres" (OptixBuildInputSphereArray, singleRadius).
std::shared_ptr<OptixGas> OptixAccelBuilder::build_lamp_gas(uint8_t lamp_shape) {
  const ar::LampShape shape = ar::LampShape(lamp_shape);
  context_.bind();
  auto g = std::make_shared<OptixGas>();
  g->device = &context_.device();
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
    g->handle = h;
    g->buffer = out;
    out = DeviceBuffer();
  } catch (...) {
    sync_quiet();  // an in-flight build reads and writes these buffers
    release_all();
    throw;
  }
  release_all();
  return g;
}

std::shared_ptr<OptixTlas> OptixAccelBuilder::build_tlas(const InstanceTable* table,
                                                         const std::vector<ar::LightRecord>& lights,
                                                         const MeshLookup& mesh, const LampLookup& lamp) {
  context_.bind();
  Device& dev = context_.device();
  auto tlas = std::make_shared<OptixTlas>();
  tlas->device = &dev;
  std::set<const OptixGas*> kept;
  const auto keep = [&](const std::shared_ptr<const OptixGas>& g) {
    if (kept.insert(g.get()).second) tlas->keep.push_back(g);
  };
  std::vector<OptixInstance> inst;
  const size_t n = table ? table->instances.size() : 0;
  inst.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const Instance& s = table->instances[i];
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
      const std::shared_ptr<const OptixGas> lg = lamp(uint8_t(shape));
      keep(lg);
      OptixInstance o = {};
      ar::lamp_geometry_xform(rec, shape, o.transform);
      o.instanceId = uint32_t(i);
      o.sbtOffset = shape == ar::LampShape::DISK ? kSbtLampDisk : shape == ar::LampShape::SPHERE ? kSbtLampSphere : kSbtTriangle;
      o.visibilityMask = s.mask;  // never the shadow bit: a lamp blocks no shadow ray (0004 D8)
      o.flags = OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT;
      o.traversableHandle = lg->handle;
      inst.push_back(o);
      continue;
    }
    const std::shared_ptr<const OptixGas> g = mesh(uint32_t(i), s.geom_key);
    if (!g) continue;  // an empty mesh has nothing to hit
    keep(g);
    OptixInstance o = {};
    std::memcpy(o.transform, table->centre(uint32_t(i)).m, sizeof o.transform);
    o.instanceId = uint32_t(i);  // the index of its DeviceInstanceRecord, what a hit reports
    o.sbtOffset = kSbtTriangle;  // the mesh hit group (0002 D11)
    o.visibilityMask = s.mask;
    o.flags = OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT;  // every material is opaque in M1a
    o.traversableHandle = g->handle;
    inst.push_back(o);
  }
  // Built even with no instance, so an empty scene has a valid structure that traces to a miss.
  DeviceBuffer arena;
  try {
    if (!inst.empty()) {
      tlas->instances = dev.allocate(LedgerCategory::TABLES, inst.size() * sizeof(OptixInstance), owner_);
      dev.upload(tlas->instances, 0, inst.data(), tlas->instances.bytes);
    }
    OptixBuildInput in = {};
    in.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    in.instanceArray.instances = dptr(tlas->instances);
    in.instanceArray.numInstances = uint32_t(inst.size());
    OptixAccelBuildOptions options = {};
    options.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    options.operation = OPTIX_BUILD_OPERATION_BUILD;
    auto ctx = static_cast<OptixDeviceContext>(context_.handle());
    OptixAccelBufferSizes sizes = {};
    context_.check(optixAccelComputeMemoryUsage(ctx, &options, &in, 1, &sizes), "optixAccelComputeMemoryUsage");
    tlas->buffer = dev.allocate(LedgerCategory::ACCELERATION, std::max<uint64_t>(sizes.outputSizeInBytes, 1), owner_);
    arena = dev.allocate(LedgerCategory::ACCELERATION, std::max<uint64_t>(sizes.tempSizeInBytes, 1), owner_);
    dev.synchronize();
    OptixTraversableHandle h = 0;
    context_.check(optixAccelBuild(ctx, static_cast<CUstream>(context_.stream()), &options, &in, 1, dptr(arena),
                                   arena.bytes, dptr(tlas->buffer), tlas->buffer.bytes, &h, nullptr, 0),
                   "optixAccelBuild");
    context_.sync();
    tlas->handle = h;
  } catch (...) {
    try {
      context_.sync();  // an in-flight build reads and writes these buffers
    } catch (...) {
    }
    dev.free(arena);
    throw;  // the TLAS's destructor frees what it holds
  }
  dev.free(arena);
  tlas->instance_count = uint32_t(inst.size());
  return tlas;
}

namespace {

// The snapshot's structures: its TLAS, the GAS of its meshes by key (for the next snapshot to
// share), and the lamp unit shapes.
class OptixSnapshot final : public SnapshotAccel {
 public:
  const void* kernel_accel() const override { return reinterpret_cast<const void*>(uintptr_t(tlas->handle)); }
  std::shared_ptr<OptixTlas> tlas;
  std::map<ar_key, std::shared_ptr<const OptixGas>> gas;
  std::shared_ptr<const OptixGas> lamp[4];  // by ar::LampShape
};

}  // namespace

std::shared_ptr<const SnapshotAccel> OptixAccelBuilder::build(const AccelInput& in, const SnapshotAccel* prev) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto* before = static_cast<const OptixSnapshot*>(prev);
  auto snap = std::make_shared<OptixSnapshot>();

  // A mesh keeps the previous snapshot's GAS when it was built from the same record object.
  std::vector<OptixGasJob> jobs;
  for (const auto& [key, m] : in.meshes) {
    if (!m.copy || m.copy->view.triangle_count == 0) continue;
    if (before) {
      const auto it = before->gas.find(key);
      if (it != before->gas.end() && it->second->record == m.record) {
        snap->gas[key] = it->second;
        ++snap->gas_reused;
        continue;
      }
    }
    jobs.push_back({key, m.copy->view, m.record});
  }
  const auto built = build_gas(jobs);
  for (size_t i = 0; i < jobs.size(); ++i) snap->gas[jobs[i].key] = built[i];
  snap->gas_built = uint32_t(jobs.size());

  const std::vector<ar::LightRecord> no_lights;
  snap->tlas = build_tlas(
      in.instances, in.lights ? *in.lights : no_lights,
      [&](uint32_t instance, ar_key key) -> std::shared_ptr<const OptixGas> {
        const auto mesh = in.meshes.find(key);
        if (mesh == in.meshes.end() || !mesh->second.record)
          throw std::runtime_error("instance " + std::to_string(instance) + " references geometry key " +
                                   std::to_string(key) + " which has no mesh");
        const auto g = snap->gas.find(key);
        return g == snap->gas.end() ? nullptr : g->second;
      },
      [&](uint8_t shape) -> std::shared_ptr<const OptixGas> {
        std::shared_ptr<const OptixGas>& l = snap->lamp[shape];
        if (!l && before && before->lamp[shape]) {
          l = before->lamp[shape];
          ++snap->gas_reused;
        }
        if (!l) {
          l = build_lamp_gas(shape);
          ++snap->gas_built;
        }
        return l;
      });
  snap->bytes = snap->tlas->buffer.bytes + snap->tlas->instances.bytes;
  for (const auto& kv : snap->gas) snap->bytes += kv.second->buffer.bytes;
  for (const auto& l : snap->lamp)
    if (l) snap->bytes += l->buffer.bytes;
  gas_builds_ += snap->gas_built, gas_reused_ += snap->gas_reused, ++tlas_builds_;
  snap->build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return snap;
}

// ---- OptixAccel: the version-tracking view the traversal tests use ----

uint64_t OptixAccel::gas_handle(ar_key key) const {
  auto it = gas_.find(key);
  return it == gas_.end() ? 0 : it->second.gas->handle;
}
uint64_t OptixAccel::gas_bytes(ar_key key) const {
  auto it = gas_.find(key);
  return it == gas_.end() ? 0 : it->second.gas->buffer.bytes;
}
uint64_t OptixAccel::gas_emitted_size(ar_key key) const {
  auto it = gas_.find(key);
  return it == gas_.end() ? 0 : it->second.gas->emitted;
}
uint64_t OptixAccel::gas_uncompacted_bytes(ar_key key) const {
  auto it = gas_.find(key);
  return it == gas_.end() ? 0 : it->second.gas->uncompacted;
}

std::vector<OptixAccel::TlasInstance> OptixAccel::read_tlas_instances() const {
  if (!tlas_ || tlas_->instance_count == 0) return {};
  std::vector<OptixInstance> raw(tlas_->instance_count);
  context_.device().download(raw.data(), tlas_->instances, 0, raw.size() * sizeof(OptixInstance));
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
  for (auto& kv : gas_) n += kv.second.gas->buffer.bytes;
  return n;
}

uint32_t OptixAccel::update_gas(const SceneUpload& upload) {
  // Keys that left the scene (or lost every triangle) give their GAS back first.
  for (auto it = gas_.begin(); it != gas_.end();) {
    const UploadedMesh* m = upload.find(it->first);
    if (m && m->view.triangle_count > 0) {
      ++it;
      continue;
    }
    it = gas_.erase(it);
    tlas_stale_ = true;  // the TLAS referenced the dropped GAS
  }
  std::vector<OptixGasJob> jobs;
  std::vector<uint64_t> versions;
  for (const UploadedMesh* m : upload.meshes()) {
    if (m->view.triangle_count == 0) continue;
    auto it = gas_.find(m->key);
    if (it != gas_.end() && it->second.version == m->version) continue;
    jobs.push_back({m->key, m->view, nullptr});
    versions.push_back(m->version);
  }
  if (jobs.empty()) return 0;
  const auto t0 = std::chrono::steady_clock::now();
  const auto built = core_.build_gas(jobs);
  for (size_t i = 0; i < jobs.size(); ++i) {
    gas_[jobs[i].key] = Entry{versions[i], built[i]};
    tlas_stale_ = true;  // a replaced GAS leaves the old TLAS on old geometry
  }
  gas_builds_ += built.size();
  last_gas_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return uint32_t(built.size());
}

void OptixAccel::build_tlas(const SceneUpload& upload, const InstanceTable& table,
                            const std::vector<ar::LightRecord>& lights) {
  tlas_ = core_.build_tlas(
      &table, lights,
      [&](uint32_t i, ar_key key) -> std::shared_ptr<const OptixGas> {
        const UploadedMesh* m = upload.find(key);
        if (!m)
          throw std::runtime_error("instance " + std::to_string(i) + " references geometry key " + std::to_string(key) +
                                   " which was never uploaded");
        if (m->view.triangle_count == 0) return nullptr;
        auto g = gas_.find(key);
        if (g == gas_.end() || g->second.version != m->version)
          throw std::runtime_error("geometry key " + std::to_string(key) + " (instance " + std::to_string(i) + ") has " +
                                   (g == gas_.end() ? "no GAS" : "a GAS older than its mesh data") +
                                   ": update_gas must run before build_tlas");
        return g->second.gas;
      },
      [&](uint8_t shape) -> std::shared_ptr<const OptixGas> {
        if (!lamp_gas_[shape]) lamp_gas_[shape] = core_.build_lamp_gas(shape);
        return lamp_gas_[shape];
      });
  tlas_stale_ = false;
  ++tlas_builds_;
}

namespace {

// The scene's OptiX context and builder together: the context is created on the scene's init
// thread with the device (0002 D15: a failure there is the device's init failure).
class SceneOptixBuilder final : public AccelBuilder {
 public:
  SceneOptixBuilder(Device& device, uint32_t index) : context_(device, index), core_(context_, Owner{}) {}
  std::shared_ptr<const SnapshotAccel> build(const AccelInput& in, const SnapshotAccel* prev) override {
    return core_.build(in, prev);
  }
  AccelCounters counters() const override { return core_.counters(); }

 private:
  OptixContext context_;
  OptixAccelBuilder core_;
};

}  // namespace

std::unique_ptr<AccelBuilder> make_optix_accel_builder(Device& device, uint32_t device_index) {
  return std::make_unique<SceneOptixBuilder>(device, device_index);
}

}  // namespace astroray::host

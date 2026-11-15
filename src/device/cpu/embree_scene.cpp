// SPDX-License-Identifier: Apache-2.0
#include "device/cpu/embree_scene.h"

#include <embree4/rtcore.h>

#include <algorithm>
#include <array>
#include <functional>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel/geom/lamp_intersect.h"

namespace astroray::host {

namespace {

std::string error_text(RTCError e) {
  switch (e) {
    case RTC_ERROR_UNKNOWN: return "unknown error";
    case RTC_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case RTC_ERROR_INVALID_OPERATION: return "invalid operation";
    case RTC_ERROR_OUT_OF_MEMORY: return "out of memory";
    case RTC_ERROR_UNSUPPORTED_CPU: return "unsupported CPU";
    case RTC_ERROR_CANCELLED: return "cancelled";
    default: return "error " + std::to_string(int(e));
  }
}

// Embree's per-thread error state, checked after each group of calls.
void check(RTCDevice dev, const char* what) {
  const RTCError e = rtcGetDeviceError(dev);
  if (e != RTC_ERROR_NONE) throw std::runtime_error(std::string("Embree: ") + what + ": " + error_text(e));
}

// Never refuses an allocation (a false return would make Embree fail it).
bool monitor(void* user, ssize_t bytes, bool /*post*/) {
  static_cast<std::atomic<int64_t>*>(user)->fetch_add(bytes);
  return true;
}

// One triangle scene for one mesh record, from the representative motion step.
RTCScene build_gas(RTCDevice dev, const MeshRecord& rec) {
  const MeshTopology& topo = *rec.topo;
  RTCScene scene = rtcNewScene(dev);
  rtcSetSceneFlags(scene, RTC_SCENE_FLAG_ROBUST);
  const uint32_t tris = topo.triangle_count(), verts = topo.vertex_count;
  if (tris != 0 && verts != 0) {
    RTCGeometry g = rtcNewGeometry(dev, RTC_GEOMETRY_TYPE_TRIANGLE);
    auto* vb = static_cast<float*>(
        rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), verts));
    auto* ib = static_cast<uint32_t*>(
        rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(uint32_t), tris));
    if (!vb || !ib) {
      rtcReleaseGeometry(g);
      rtcReleaseScene(scene);
      check(dev, "geometry buffers");
      throw std::runtime_error("Embree: geometry buffers could not be allocated");
    }
    const float* src = rec.positions.data() + size_t(representative_step(topo.motion_steps)) * verts * 3;
    std::copy(src, src + size_t(verts) * 3, vb);
    // tri_corners holds corner indices; Embree wants vertex indices.
    for (size_t k = 0; k < size_t(tris) * 3; ++k) ib[k] = topo.corner_vert[topo.tri_corners[k]];
    rtcSetGeometryMask(g, 0xFFFFFFFFu);  // the instance mask decides visibility, not the triangles
    rtcCommitGeometry(g);
    rtcAttachGeometry(scene, g);
    rtcReleaseGeometry(g);
  }
  rtcCommitScene(scene);
  return scene;
}

// The disk lamp's user geometry: the one lamp_disk_intersect of the kernel on the ray in the
// instance's object space (Embree hands user geometry the transformed ray; t is unchanged).
// Ref: Embree 4 API, rtcSetGeometryIntersectFunction / rtcSetGeometryOccludedFunction and the
// user_geometry tutorial (RenderKit/embree, Apache-2.0).
void disk_bounds(const RTCBoundsFunctionArguments* a) {
  *a->bounds_o = RTCBounds{-1.0f, -1.0f, -ar::kLampDiskBoundPad, 0.0f, 1.0f, 1.0f, ar::kLampDiskBoundPad, 0.0f};
}

bool disk_test(RTCRayN* ray, unsigned n, unsigned i, float* t) {
  const ar::Real3 o{RTCRayN_org_x(ray, n, i), RTCRayN_org_y(ray, n, i), RTCRayN_org_z(ray, n, i)};
  const ar::Real3 d{RTCRayN_dir_x(ray, n, i), RTCRayN_dir_y(ray, n, i), RTCRayN_dir_z(ray, n, i)};
  return ar::lamp_disk_intersect(o, d, RTCRayN_tnear(ray, n, i), RTCRayN_tfar(ray, n, i), t);
}

void disk_intersect(const RTCIntersectFunctionNArguments* a) {
  RTCRayN* ray = RTCRayHitN_RayN(a->rayhit, a->N);
  RTCHitN* hit = RTCRayHitN_HitN(a->rayhit, a->N);
  for (unsigned i = 0; i < a->N; ++i) {
    if (a->valid[i] == 0) continue;
    float t;
    if (!disk_test(ray, a->N, i, &t)) continue;
    RTCRayN_tfar(ray, a->N, i) = t;
    RTCHitN_u(hit, a->N, i) = 0.0f;
    RTCHitN_v(hit, a->N, i) = 0.0f;
    RTCHitN_Ng_x(hit, a->N, i) = 0.0f;
    RTCHitN_Ng_y(hit, a->N, i) = 0.0f;
    RTCHitN_Ng_z(hit, a->N, i) = 1.0f;
    RTCHitN_primID(hit, a->N, i) = a->primID;
    RTCHitN_geomID(hit, a->N, i) = a->geomID;
    for (unsigned l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l) RTCHitN_instID(hit, a->N, i, l) = a->context->instID[l];
  }
}

void disk_occluded(const RTCOccludedFunctionNArguments* a) {
  for (unsigned i = 0; i < a->N; ++i) {
    if (a->valid[i] == 0) continue;
    float t;
    if (disk_test(a->ray, a->N, i, &t)) RTCRayN_tfar(a->ray, a->N, i) = -std::numeric_limits<float>::infinity();
  }
}

// A sphere lamp's hit has no barycentrics: Embree reports its own sphere parameters in u and v,
// OptiX writes 0 (the hit record of every lamp hit but a triangle's has u = v = 0).
void zero_uv(const RTCFilterFunctionNArguments* a) {
  for (unsigned i = 0; i < a->N; ++i) {
    if (a->valid[i] == 0) continue;
    RTCHitN_u(a->hit, a->N, i) = 0.0f;
    RTCHitN_v(a->hit, a->N, i) = 0.0f;
  }
}

// The unit shape of one lamp type, the same for every lamp of that type (their instances place it).
RTCScene build_lamp_scene(RTCDevice dev, ar::LampShape shape) {
  RTCScene scene = rtcNewScene(dev);
  rtcSetSceneFlags(scene, RTC_SCENE_FLAG_ROBUST);
  RTCGeometry g = nullptr;
  if (shape == ar::LampShape::RECT) {
    g = rtcNewGeometry(dev, RTC_GEOMETRY_TYPE_TRIANGLE);
    auto* vb = static_cast<float*>(
        rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), 4));
    auto* ib = static_cast<uint32_t*>(
        rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(uint32_t), 2));
    if (vb && ib) {
      const float v[12] = {-0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 0.0f, -0.5f, 0.5f, 0.0f};
      const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
      std::copy(v, v + 12, vb);
      std::copy(idx, idx + 6, ib);
    }
  } else if (shape == ar::LampShape::SPHERE) {
    g = rtcNewGeometry(dev, RTC_GEOMETRY_TYPE_SPHERE_POINT);
    auto* vb = static_cast<float*>(
        rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, 4 * sizeof(float), 1));
    if (vb) vb[0] = 0.0f, vb[1] = 0.0f, vb[2] = 0.0f, vb[3] = 1.0f;
    rtcSetGeometryIntersectFilterFunction(g, zero_uv);
  } else {
    g = rtcNewGeometry(dev, RTC_GEOMETRY_TYPE_USER);
    rtcSetGeometryUserPrimitiveCount(g, 1);
    rtcSetGeometryBoundsFunction(g, disk_bounds, nullptr);
    rtcSetGeometryIntersectFunction(g, disk_intersect);
    rtcSetGeometryOccludedFunction(g, disk_occluded);
  }
  rtcSetGeometryMask(g, 0xFFFFFFFFu);  // the lamp instance's mask decides visibility
  rtcCommitGeometry(g);
  rtcAttachGeometry(scene, g);
  rtcReleaseGeometry(g);
  rtcCommitScene(scene);
  return scene;
}

}  // namespace

struct EmbreeDevice::Impl {
  RTCDevice dev = nullptr;
  std::atomic<int64_t> bytes{0};  // Embree's own allocations, from the memory monitor
};

namespace {

// Host memory of the structures one build made, recorded in the ledger until the last structure of
// the build (shared with later snapshots) is gone.
struct Charge {
  Charge(std::shared_ptr<Ledger> l, uint64_t bytes) : ledger(std::move(l)) {
    if (ledger && bytes) id = ledger->add(Owner{}, LedgerCategory::ACCELERATION, bytes);
  }
  ~Charge() {
    if (!ledger || !id) return;
    try {
      ledger->remove(id);
    } catch (...) {
    }
  }
  std::shared_ptr<Ledger> ledger;
  uint64_t id = 0;
};

// One Embree scene that snapshots share: a mesh's triangle scene (identified by its record) or a
// lamp's unit shape (no record). `keep` holds the device the memory monitor lives in.
struct Shared {
  ~Shared() {
    if (scene) rtcReleaseScene(scene);
  }
  RTCScene scene = nullptr;
  std::shared_ptr<const MeshRecord> record;
  std::shared_ptr<Charge> charge;
  std::shared_ptr<EmbreeDevice> keep;
};

}  // namespace

struct EmbreeScene::Impl {
  ~Impl() {
    if (top) rtcReleaseScene(top);  // before the shared scenes it instances
  }
  std::shared_ptr<EmbreeDevice> keep;  // first member: released last
  RTCScene top = nullptr;
  std::map<ar_key, std::shared_ptr<Shared>> gas;
  std::array<std::shared_ptr<Shared>, 4> lamp_scene;  // by ar::LampShape; NONE and INVALID stay null
  std::shared_ptr<Charge> charge;
  uint32_t instances = 0;
  uint64_t bytes = 0;
};

EmbreeDevice::EmbreeDevice() : impl_(new Impl) {
  // The static build has Embree's own tasking system; the default config uses all cores.
  impl_->dev = rtcNewDevice(nullptr);
  if (!impl_->dev) throw std::runtime_error("Embree: rtcNewDevice failed: " + error_text(rtcGetDeviceError(nullptr)));
  rtcSetDeviceMemoryMonitorFunction(impl_->dev, monitor, &impl_->bytes);
}

EmbreeDevice::~EmbreeDevice() {
  if (impl_ && impl_->dev) rtcReleaseDevice(impl_->dev);
}

EmbreeScene::EmbreeScene(SnapshotTag) : impl_(new Impl) {}

EmbreeScene::EmbreeScene(EmbreeDevice& device, const MeshStore& meshes, const InstanceTable& table,
                         const std::vector<ar::LightRecord>& lights, Ledger& ledger, ThreadPool& pool)
    : impl_(new Impl) {
  // The caller's ledger outlives the scene, as it always has: a non-owning handle.
  init(device, table, lights, [&meshes](ar_key k) { return meshes.find(k); },
       std::shared_ptr<Ledger>(&ledger, [](Ledger*) {}), &pool, nullptr);
}

std::shared_ptr<EmbreeScene> EmbreeScene::build(std::shared_ptr<EmbreeDevice> device, const AccelInput& in,
                                                const EmbreeScene* prev, ThreadPool* pool) {
  std::shared_ptr<EmbreeScene> s(new EmbreeScene(SnapshotTag{}));
  s->impl_->keep = device;
  const InstanceTable empty_table;
  const std::vector<ar::LightRecord> no_lights;
  s->init(
      *device, in.instances ? *in.instances : empty_table, in.lights ? *in.lights : no_lights,
      [&in](ar_key k) -> std::shared_ptr<const MeshRecord> {
        const auto it = in.meshes.find(k);
        return it == in.meshes.end() ? nullptr : it->second.record;
      },
      in.host_ledger, pool, prev);
  return s;
}

void EmbreeScene::init(EmbreeDevice& device, const InstanceTable& table, const std::vector<ar::LightRecord>& lights,
                       const std::function<std::shared_ptr<const MeshRecord>(ar_key)>& find,
                       const std::shared_ptr<Ledger>& ledger, ThreadPool* pool, const EmbreeScene* prev) {
  const auto t0 = std::chrono::steady_clock::now();
  Impl& s = *impl_;
  RTCDevice dev = device.impl_->dev;
  // Each structure is charged to the ledger on its own, by Embree's memory monitor over the call
  // that builds it, and released with its last holder. The monitor counts the whole device, which
  // belongs to this one scene and is only built on under the scene's table lock, so a serial
  // build sees nothing but its own allocations. Triangle scenes built on a pool run together and
  // are charged as one batch record that their last holder releases.
  const auto device_bytes = [&] { return device.impl_->bytes.load(); };
  uint64_t charged = 0;
  const auto charge_of = [&](int64_t used_bytes) -> std::shared_ptr<Charge> {
    if (used_bytes <= 0) return nullptr;
    charged += uint64_t(used_bytes);
    return std::make_shared<Charge>(ledger, uint64_t(used_bytes));
  };
  const auto adopt = [&](RTCScene scene, std::shared_ptr<const MeshRecord> rec, std::shared_ptr<Charge> charge) {
    auto sh = std::make_shared<Shared>();
    sh->scene = scene, sh->record = std::move(rec), sh->keep = s.keep, sh->charge = std::move(charge);
    ++gas_built;
    return sh;
  };

  // Lamp instances take their shape from the light table; everything else needs its mesh.
  std::vector<ar::LampShape> shape(table.instances.size(), ar::LampShape::NONE);
  std::vector<bool> is_lamp(table.instances.size(), false);
  for (uint32_t i = 0; i < table.instances.size(); ++i) {
    const uint32_t li = table.instances[i].light_index;
    if (li == ar::kNoLightIndex) continue;
    if (li >= lights.size())
      throw std::runtime_error("Embree: instance " + std::to_string(i) + " names light " + std::to_string(li) +
                               " but the light table has " + std::to_string(lights.size()));
    shape[i] = ar::lamp_shape(lights[li]);
    if (shape[i] == ar::LampShape::INVALID)
      throw std::runtime_error("Embree: instance " + std::to_string(i) + " names light " + std::to_string(li) +
                               " which is not a lamp (a point lamp needs radius 0, a sphere lamp radius > 0)");
    if (table.instances[i].mask & ar::kShadowMaskBit)
      throw std::runtime_error("Embree: instance " + std::to_string(i) + " is a lamp whose mask carries the shadow bit (0004 D8: a lamp never blocks a shadow ray)");
    is_lamp[i] = true;
  }

  // Resolve every instance's mesh first so a missing key is reported with its instance.
  std::map<ar_key, std::shared_ptr<const MeshRecord>> records;
  for (uint32_t i = 0; i < table.instances.size(); ++i) {
    if (is_lamp[i]) continue;
    const ar_key k = table.instances[i].geom_key;
    if (records.count(k)) continue;
    auto rec = find(k);
    if (!rec || !rec->topo)
      throw std::runtime_error("Embree: instance " + std::to_string(i) + " names geometry key " + std::to_string(k) +
                               " which has no mesh");
    records.emplace(k, std::move(rec));
  }

  // A key whose record is the previous snapshot's object keeps that scene; the rest are built, on
  // the pool when there is one. The first exception stops the rest and is rethrown.
  std::vector<std::pair<ar_key, std::shared_ptr<const MeshRecord>>> todo;
  for (auto& [k, r] : records) {
    std::shared_ptr<Shared> old;
    if (prev) {
      const auto it = prev->impl_->gas.find(k);
      if (it != prev->impl_->gas.end() && it->second->record == r) old = it->second;
    }
    if (old) {
      s.gas.emplace(k, std::move(old));
      ++gas_reused;
    } else {
      todo.emplace_back(k, r);
    }
  }
  std::vector<RTCScene> built(todo.size(), nullptr);
  const auto build_one = [&](uint32_t j) {
    built[j] = build_gas(dev, *todo[j].second);
    check(dev, "triangle scene");
  };
  std::vector<std::shared_ptr<Charge>> gas_charge(todo.size());
  try {
    if (pool && todo.size() > 1) {
      const int64_t m0 = device_bytes();
      pool->parallel_for(uint32_t(todo.size()), 1, [&](uint32_t first, uint32_t last) {
        for (uint32_t j = first; j < last; ++j) build_one(j);
      });
      const auto batch = charge_of(device_bytes() - m0);
      for (auto& c : gas_charge) c = batch;
    } else {
      for (uint32_t j = 0; j < todo.size(); ++j) {
        const int64_t m0 = device_bytes();
        build_one(j);
        gas_charge[j] = charge_of(device_bytes() - m0);
      }
    }
  } catch (...) {
    for (RTCScene b : built)
      if (b) rtcReleaseScene(b);
    throw;
  }
  for (size_t j = 0; j < todo.size(); ++j) s.gas.emplace(todo[j].first, adopt(built[j], todo[j].second, gas_charge[j]));

  // The instance scene's own memory: its geometries and tree, measured around its construction
  // with the lamp scenes (charged separately inside) taken out.
  const int64_t top_m0 = device_bytes();
  int64_t lamp_bytes = 0;

  // The instance scene: transform and mask per instance, instance id = table index.
  s.top = rtcNewScene(dev);
  rtcSetSceneFlags(s.top, RTC_SCENE_FLAG_ROBUST);
  for (uint32_t i = 0; i < table.instances.size(); ++i) {
    float lamp_xf[12];
    RTCScene inner = nullptr;
    const float* xf = table.centre(i).m;  // ar_xform is 3x4 row-major object to world
    if (is_lamp[i]) {
      if (shape[i] == ar::LampShape::NONE) continue;  // a delta point lamp has no surface to hit
      std::shared_ptr<Shared>& ls = s.lamp_scene[size_t(shape[i])];
      if (!ls && prev && prev->impl_->lamp_scene[size_t(shape[i])]) {
        ls = prev->impl_->lamp_scene[size_t(shape[i])];
        ++gas_reused;
      }
      if (!ls) {
        const int64_t m0 = device_bytes();
        RTCScene scene = build_lamp_scene(dev, shape[i]);
        const int64_t used = device_bytes() - m0;
        lamp_bytes += used;
        ls = adopt(scene, nullptr, charge_of(used));
        check(dev, "lamp scene");
      }
      inner = ls->scene;
      ar::lamp_geometry_xform(lights[table.instances[i].light_index], shape[i], lamp_xf);
      xf = lamp_xf;
    } else {
      inner = s.gas.at(table.instances[i].geom_key)->scene;
    }
    RTCGeometry g = rtcNewGeometry(dev, RTC_GEOMETRY_TYPE_INSTANCE);
    rtcSetGeometryInstancedScene(g, inner);
    rtcSetGeometryTimeStepCount(g, 1);
    rtcSetGeometryTransform(g, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, xf);
    rtcSetGeometryMask(g, table.instances[i].mask);
    rtcCommitGeometry(g);
    rtcAttachGeometryByID(s.top, g, i);
    rtcReleaseGeometry(g);
  }
  rtcCommitScene(s.top);
  check(dev, "instance scene");
  s.instances = uint32_t(table.instances.size());

  s.charge = charge_of(device_bytes() - top_m0 - lamp_bytes);
  s.bytes = charged;
  build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

EmbreeScene::~EmbreeScene() = default;

void* EmbreeScene::handle() const { return impl_->top; }
uint32_t EmbreeScene::instance_count() const { return impl_->instances; }
uint64_t EmbreeScene::acceleration_bytes() const { return impl_->bytes; }

namespace {

// The CPU scene's builder: one Embree device for all its snapshots.
class EmbreeAccelBuilder final : public AccelBuilder {
 public:
  EmbreeAccelBuilder() : device_(std::make_shared<EmbreeDevice>()) {}
  std::shared_ptr<const SnapshotAccel> build(const AccelInput& in, const SnapshotAccel* prev) override {
    // Triangle scenes are built serially: Embree parallelizes each commit with its own tasking.
    auto s = EmbreeScene::build(device_, in, static_cast<const EmbreeScene*>(prev), nullptr);
    gas_builds_ += s->gas_built, gas_reused_ += s->gas_reused, ++tlas_builds_;
    return s;
  }
  AccelCounters counters() const override { return {gas_builds_, gas_reused_, tlas_builds_}; }

 private:
  std::shared_ptr<EmbreeDevice> device_;
  std::atomic<uint64_t> gas_builds_{0}, gas_reused_{0}, tlas_builds_{0};
};

}  // namespace

std::unique_ptr<AccelBuilder> make_cpu_accel_builder() { return std::make_unique<EmbreeAccelBuilder>(); }

}  // namespace astroray::host

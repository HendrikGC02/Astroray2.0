// SPDX-License-Identifier: Apache-2.0
#include "device/cpu/embree_scene.h"

#include <embree4/rtcore.h>

#include <algorithm>
#include <array>
#include <atomic>
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

struct EmbreeScene::Impl {
  ~Impl() {
    if (top) rtcReleaseScene(top);
    for (auto& [key, scene] : gas) rtcReleaseScene(scene);
    for (RTCScene l : lamp_scene)
      if (l) rtcReleaseScene(l);
    if (ledger && ledger_id) {
      try {
        ledger->remove(ledger_id);
      } catch (...) {
      }
    }
  }
  RTCScene top = nullptr;
  std::map<ar_key, RTCScene> gas;
  std::array<RTCScene, 4> lamp_scene = {};  // by ar::LampShape; NONE and INVALID stay null
  uint32_t instances = 0;
  uint64_t bytes = 0;
  Ledger* ledger = nullptr;
  uint64_t ledger_id = 0;
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

EmbreeScene::EmbreeScene(EmbreeDevice& device, const MeshStore& meshes, const InstanceTable& table,
                         const std::vector<ar::LightRecord>& lights, Ledger& ledger, ThreadPool& pool)
    : impl_(new Impl) {
  Impl& s = *impl_;
  RTCDevice dev = device.impl_->dev;
  const int64_t before = device.impl_->bytes.load();

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
                               " which is not a lamp");
    is_lamp[i] = true;
  }

  // Resolve every instance's mesh first so a missing key is reported with its instance.
  std::map<ar_key, std::shared_ptr<const MeshRecord>> records;
  for (uint32_t i = 0; i < table.instances.size(); ++i) {
    if (is_lamp[i]) continue;
    const ar_key k = table.instances[i].geom_key;
    if (records.count(k)) continue;
    auto rec = meshes.find(k);
    if (!rec || !rec->topo)
      throw std::runtime_error("Embree: instance " + std::to_string(i) + " names geometry key " + std::to_string(k) +
                               " which has no mesh");
    records.emplace(k, std::move(rec));
  }

  // Build the key scenes on the pool; the first exception stops the rest and is rethrown.
  std::vector<std::pair<ar_key, const MeshRecord*>> todo;
  for (auto& [k, r] : records) todo.emplace_back(k, r.get());
  std::vector<RTCScene> built(todo.size(), nullptr);
  try {
    pool.parallel_for(uint32_t(todo.size()), 1, [&](uint32_t first, uint32_t last) {
      for (uint32_t j = first; j < last; ++j) {
        built[j] = build_gas(dev, *todo[j].second);
        check(dev, "triangle scene");
      }
    });
  } catch (...) {
    for (RTCScene b : built)
      if (b) rtcReleaseScene(b);
    throw;
  }
  for (size_t j = 0; j < todo.size(); ++j) s.gas.emplace(todo[j].first, built[j]);

  // The instance scene: transform and mask per instance, instance id = table index.
  s.top = rtcNewScene(dev);
  rtcSetSceneFlags(s.top, RTC_SCENE_FLAG_ROBUST);
  for (uint32_t i = 0; i < table.instances.size(); ++i) {
    float lamp_xf[12];
    RTCScene inner = nullptr;
    const float* xf = table.centre(i).m;  // ar_xform is 3x4 row-major object to world
    if (is_lamp[i]) {
      if (shape[i] == ar::LampShape::NONE) continue;  // a delta point lamp has no surface to hit
      RTCScene& ls = s.lamp_scene[size_t(shape[i])];
      if (!ls) {
        ls = build_lamp_scene(dev, shape[i]);
        check(dev, "lamp scene");
      }
      inner = ls;
      ar::lamp_geometry_xform(lights[table.instances[i].light_index], shape[i], lamp_xf);
      xf = lamp_xf;
    } else {
      inner = s.gas.at(table.instances[i].geom_key);
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

  const int64_t used = device.impl_->bytes.load() - before;
  if (used > 0) {
    s.bytes = uint64_t(used);
    s.ledger_id = ledger.add(Owner{}, LedgerCategory::ACCELERATION, s.bytes);
    s.ledger = &ledger;
  }
}

EmbreeScene::~EmbreeScene() = default;

void* EmbreeScene::handle() const { return impl_->top; }
uint32_t EmbreeScene::instance_count() const { return impl_->instances; }
uint64_t EmbreeScene::acceleration_bytes() const { return impl_->bytes; }

}  // namespace astroray::host

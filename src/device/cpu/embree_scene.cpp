// SPDX-License-Identifier: Apache-2.0
#include "device/cpu/embree_scene.h"

#include <embree4/rtcore.h>

#include <atomic>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

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

}  // namespace

struct EmbreeDevice::Impl {
  RTCDevice dev = nullptr;
  std::atomic<int64_t> bytes{0};  // Embree's own allocations, from the memory monitor
};

struct EmbreeScene::Impl {
  ~Impl() {
    if (top) rtcReleaseScene(top);
    for (auto& [key, scene] : gas) rtcReleaseScene(scene);
    if (ledger && ledger_id) {
      try {
        ledger->remove(ledger_id);
      } catch (...) {
      }
    }
  }
  RTCScene top = nullptr;
  std::map<ar_key, RTCScene> gas;
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

EmbreeScene::EmbreeScene(EmbreeDevice& device, const MeshStore& meshes, const InstanceTable& table, Ledger& ledger,
                         ThreadPool& pool)
    : impl_(new Impl) {
  Impl& s = *impl_;
  RTCDevice dev = device.impl_->dev;
  const int64_t before = device.impl_->bytes.load();

  // Resolve every instance's mesh first so a missing key is reported with its instance.
  std::map<ar_key, std::shared_ptr<const MeshRecord>> records;
  for (uint32_t i = 0; i < table.instances.size(); ++i) {
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
    RTCGeometry g = rtcNewGeometry(dev, RTC_GEOMETRY_TYPE_INSTANCE);
    rtcSetGeometryInstancedScene(g, s.gas.at(table.instances[i].geom_key));
    rtcSetGeometryTimeStepCount(g, 1);
    // ar_xform is 3x4 row-major object to world.
    rtcSetGeometryTransform(g, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, table.centre(i).m);
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

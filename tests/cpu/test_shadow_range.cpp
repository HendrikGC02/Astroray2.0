// SPDX-License-Identifier: Apache-2.0
// RULINGS 84 on real stages: a CPU session renders a quad under a white world (every first hit
// takes an NEE shadow ray, so the shadow pool is used every step) twice, once with the shadow pool
// partitioned up to its high-water mark and once over its whole capacity (AR_DEBUG_SHADOW_FULL_RANGE,
// the behaviour before the ruling). The free list is the same slots in the same order either way, so
// every film plane, the pass counters and the INVALID_SAMPLE entries are bit-identical. One CPU
// thread, so the film's fp32 adds run in one order (0002 D12). The invalid total is compared through
// the INVALID_SAMPLE entries; this scene has none, so it is 0 in both runs.
// Ref: docs/decisions/RULINGS.md 84; docs/decisions/0002-kernel-architecture.md D5, D6, D12.
#include <doctest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/film/film_view.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/session/readback.h"
#include "host/session/session.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;

// The scheduler reads AR_DEBUG_SHADOW_FULL_RANGE only in builds with the gate hooks.
#if defined(AR_GATE_HOOKS)
namespace {

constexpr uint32_t kW = 16, kH = 8, kSpp = 4;
constexpr ar_key kMesh = 100, kMaterial = 1, kGraph = 1001, kWorldGraph = 1002;

struct Rig {
  ContextState ctx;
  Scene scene{ctx, SceneConfig{2, {}, {}}};
  uint64_t generation = 0;
  Rig() {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
  }
  template <class F>
  void commit(F&& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    REQUIRE(e->commit(&generation) == AR_OK);
    scene.materials->end_edit();
    e.reset();
    scene.generations().wait_idle();
    REQUIRE(scene.generations().failed() == 0);
  }
  void settings(Edit& e) {
    ar_settings s{};
    s.samples = s.viewport_samples = kSpp;
    s.max_bounces = 3;
    s.pixel_filter = AR_FILTER_BOX;
    s.pixel_filter_width = 1.0f;
    s.film_exposure = 1.0f;
    s.seed = 7;
    REQUIRE(e.set_settings(&s) == AR_OK);
  }
  void world(Edit& e) {  // constant white world: the NEE target of every hit
    std::vector<ar_graph_node> nodes(3);
    std::vector<ar_graph_link> links{{0, 0, 2, 0}, {1, 0, 2, 1}};
    std::vector<float> f{1.0f, 1.0f, 1.0f, 1.0f};
    nodes[0].op = uint32_t(ar::Op::CONST_F32X3), nodes[0].f32_count = 3;
    nodes[1].op = uint32_t(ar::Op::CONST_F32), nodes[1].f32_first = 3, nodes[1].f32_count = 1;
    nodes[2].op = uint32_t(ar::Op::EMIT_LE);
    ar_graph_desc d = {};
    d.node_count = 3, d.link_count = 2, d.f32_count = 4, d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data();
    REQUIRE(scene.materials->set_graph(e, kWorldGraph, &d) == AR_OK);
    ar_world_desc w = {};
    w.surface_graph = kWorldGraph;
    REQUIRE(scene.materials->set_world(e, &w) == AR_OK);
  }
  void quad(Edit& e) {  // a tilted quad, so some bounces hit it again and some escape
    const float h = 20.0f;
    const std::vector<float> pos{-h, -h, -2.0f, h, -h, -1.0f, h, h, -1.0f, -h, h, -2.0f};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    ar_mesh_desc d = {};
    d.key = kMesh, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1, d.uv_layer_count = 1;
    ar_mesh_buffers b = {};
    REQUIRE(mesh_begin(scene, e, &d, &b) == AR_OK);
    std::memcpy(*b.positions, pos.data(), pos.size() * 4);
    std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
    std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
    for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
    std::memset(*b.uv, 0, b.uv_count * 4);
    REQUIRE(e.item_commit(kMesh) == AR_OK);
  }
  void material(Edit& e) {  // the default OpenPBR material (an empty graph)
    ar_graph_desc d = {};
    REQUIRE(scene.materials->set_graph(e, kGraph, &d) == AR_OK);
    ar_material_desc m = {};
    m.surface_graph = kGraph;
    m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
    REQUIRE(scene.materials->set_material(e, kMaterial, &m) == AR_OK);
  }
  void instance(Edit& e) {
    std::vector<ar_key> geom{kMesh}, object{500}, pool{kMaterial};
    std::vector<ar_xform> xf(1);
    xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
    std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA), one(1, 1);
    std::vector<ar_rgba> colors(1, ar_rgba{{1, 1, 1, 1}});
    std::vector<uint8_t> lg(1, 0);
    ar_instances_desc d = {};
    d.instance_count = 1, d.motion_steps = 1, d.transform_count = 1, d.material_key_count = 1;
    d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
    d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
    d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
    d.material_offsets = zeros.data(), d.material_counts = one.data(), d.material_keys = pool.data();
    REQUIRE(set_instances(e, &d) == AR_OK);
  }
};

ar_view_desc view_of(uint32_t w, uint32_t h) {
  ar_view_desc v{};
  v.region_width = w;
  v.region_height = h;
  v.render_scale = 1.0f;
  v.camera.cam_to_world.m[0] = v.camera.cam_to_world.m[5] = v.camera.cam_to_world.m[10] = 1.0f;
  const float p[16] = {0.5f, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 0, -1.0f, 0, 0, 0, 1.0f};
  for (int i = 0; i < 16; ++i) v.camera.projection_inverse[i] = p[i];
  v.camera.type = AR_CAMERA_PERSP;
  return v;
}

bool until(const std::function<bool()>& f, int timeout_ms = 60000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!f()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// Sets an environment variable for one scope; the scheduler reads it when the session builds it.
struct EnvGuard {
  const char* name;
  EnvGuard(const char* n, bool on) : name(n) { REQUIRE(_putenv_s(name, on ? "1" : "") == 0); }
  ~EnvGuard() { _putenv_s(name, ""); }
};

struct Render {
  std::vector<std::vector<float>> planes;  // every pass's mean, then Combined's variance
  PassCounters counters{};
  std::vector<uint64_t> invalid;  // the counts of the INVALID_SAMPLE entries
};

Render render(bool full_range) {
  Rig rig;
  rig.commit([&](Edit& e) {
    rig.quad(e);
    rig.world(e);
    rig.material(e);
    rig.settings(e);
  });
  rig.commit([&](Edit& e) { rig.instance(e); });
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 1);
  SessionOptions o;
  o.max_width = kW;
  o.max_height = kH;
  Render out;
  std::unique_ptr<Session> s;
  {
    EnvGuard env("AR_DEBUG_SHADOW_FULL_RANGE", full_range);
    s = std::make_unique<Session>(rig.scene, *dev, o);
  }
  s->set_view(view_of(kW, kH));
  s->start();
  REQUIRE(until([&] {
    const SessionProgress p = s->progress();
    return p.state == AR_STATE_CONVERGED && p.active_generation == rig.generation;
  }));
  REQUIRE(s->failure().empty());
  const std::vector<PassDesc> passes = list_passes(s->layout());
  REQUIRE(!passes.empty());
  s->read_film([&](const FilmReadContext& c) {
    FilmView view(c.device, c.film);
    for (const PassDesc& p : passes) {
      std::vector<float> m(pass_float_count(p, AR_QUANTITY_MEAN, view.width(), view.height()));
      read_pass(view, p, AR_QUANTITY_MEAN, uint32_t(m.size()), m.data());
      out.planes.push_back(std::move(m));
    }
    std::vector<float> v(pass_float_count(passes.front(), AR_QUANTITY_VARIANCE, view.width(), view.height()));
    read_pass(view, passes.front(), AR_QUANTITY_VARIANCE, uint32_t(v.size()), v.data());
    out.planes.push_back(std::move(v));
  });
  out.counters = s->pass_counters();
  const ar_report_info info = s->report_snapshot();
  for (uint32_t i = 0; i < info.entry_count; ++i) {
    ar_report_entry e{};
    s->report_entry(i, &e);
    if (e.code == AR_REPORT_INVALID_SAMPLE) out.invalid.push_back(e.count);
  }
  return out;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("shadow range/film and counters match the full shadow range") {
  const Render ranged = render(false);
  const Render full = render(true);
  REQUIRE(ranged.planes.size() == full.planes.size());
  for (size_t i = 0; i < ranged.planes.size(); ++i) {
    REQUIRE(ranged.planes[i].size() == full.planes[i].size());
    CHECK(std::memcmp(ranged.planes[i].data(), full.planes[i].data(), ranged.planes[i].size() * 4) == 0);
  }
  CHECK(std::memcmp(&ranged.counters, &full.counters, sizeof(PassCounters)) == 0);
  CHECK(ranged.invalid == full.invalid);
  // The scene really used the shadow pool and lit the film.
  CHECK(ranged.counters.rays_shadow > 0);
  double sum = 0.0;
  for (float x : ranged.planes.front()) sum += x;
  CHECK(sum > 0.0);
}

}  // TEST_SUITE
#endif  // AR_GATE_HOOKS

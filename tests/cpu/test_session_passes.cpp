// SPDX-License-Identifier: Apache-2.0
// The session's film follows the latched settings' pass list: no request allocates no primary-hit
// AOV, Position adds the four of them (Position, Normal, UV, IndexMA) and Position reads the hit
// point, a commit that drops the request rebuilds the film without them, and a name the engine
// cannot produce gives one report entry per settings change and never fails the render.
// The pass list is read as ar_session_pass_count and ar_session_read_pass read it: list_passes of
// Session::layout(), values through a FilmView of the film under the render lock.
// Ref: docs/decisions/0008-film-outputs-denoise.md d10, d13a; 0007 d25; RULINGS 74 (PASS_UNSUPPORTED).
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "host/film/film_view.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/session/readback.h"
#include "host/session/session.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kW = 6, kH = 4;
constexpr ar_key kMesh = 100;

struct PassRig {
  ContextState ctx;
  Scene scene{ctx, SceneConfig{2, {}, {}}};
  uint64_t generation = 0;
  PassRig() {
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
  void settings(Edit& e, const std::vector<std::string>& passes) {
    std::vector<ar_name> names(passes.size());
    for (size_t i = 0; i < passes.size(); ++i) std::memcpy(names[i].text, passes[i].data(), passes[i].size());
    ar_settings s{};
    s.passes = names.data();
    s.pass_count = uint32_t(names.size());
    s.samples = s.viewport_samples = 2;
    s.max_bounces = 0;
    s.pixel_filter = AR_FILTER_BOX;
    s.pixel_filter_width = 1.0f;
    s.film_exposure = 1.0f;
    REQUIRE(e.set_settings(&s) == AR_OK);
  }
  // A material-less quad at z = -1 that fills the camera's view, with one UV layer.
  void quad(Edit& e) {
    const float h = 20.0f, z = -1.0f;
    const std::vector<float> pos{-h, -h, z, h, -h, z, h, h, z, -h, h, z}, uv{0, 0, 1, 0, 1, 1, 0, 1};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    ar_mesh_desc d = {};
    d.key = kMesh, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1, d.uv_layer_count = 1;
    ar_mesh_buffers b = {};
    REQUIRE(mesh_begin(scene, e, &d, &b) == AR_OK);
    std::memcpy(*b.positions, pos.data(), pos.size() * 4);
    std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
    std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
    for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
    REQUIRE(b.uv_count == uv.size());
    std::memcpy(*b.uv, uv.data(), uv.size() * 4);
    REQUIRE(e.item_commit(kMesh) == AR_OK);
  }
  void instance(Edit& e) {
    std::vector<ar_key> geom{kMesh}, object{500};
    std::vector<ar_xform> xf(1);
    xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
    std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA);
    std::vector<ar_rgba> colors(1, ar_rgba{{1, 1, 1, 1}});
    std::vector<uint8_t> lg(1, 0);
    ar_instances_desc d = {};
    d.instance_count = 1, d.motion_steps = 1, d.transform_count = 1;
    d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
    d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
    d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
    d.material_offsets = zeros.data(), d.material_counts = zeros.data();
    REQUIRE(set_instances(e, &d) == AR_OK);
  }
  void build(const std::vector<std::string>& passes) {
    commit([&](Edit& e) {
      quad(e);
      settings(e, passes);
    });
    commit([&](Edit& e) { instance(e); });
  }
};

ar_view_desc view_of(uint32_t w, uint32_t h) {
  ar_view_desc v{};
  v.region_width = w;
  v.region_height = h;
  v.render_scale = 1.0f;
  v.camera.cam_to_world.m[0] = v.camera.cam_to_world.m[5] = v.camera.cam_to_world.m[10] = 1.0f;
  const float p[16] = {0.5f, 0, 0, 0, 0, 0.375f, 0, 0, 0, 0, 0, -1.0f, 0, 0, 0, 1.0f};
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

// Converged on the rig's last commit.
void settle(Session& s, const PassRig& rig) {
  REQUIRE(until([&] {
    const SessionProgress p = s.progress();
    return p.state == AR_STATE_CONVERGED && p.active_generation == rig.generation;
  }));
  REQUIRE(s.failure().empty());
}

std::vector<std::string> pass_names(const Session& s) {
  std::vector<std::string> out;
  for (const PassDesc& p : list_passes(s.layout())) out.push_back(p.name);
  return out;
}

bool has(const std::vector<std::string>& v, const char* name) { return std::find(v.begin(), v.end(), name) != v.end(); }

void check_no_aov(Session& s) {
  const std::vector<std::string> names = pass_names(s);
  for (const char* aov : {"Position", "Normal", "UV", "IndexMA"}) CHECK_FALSE(has(names, aov));
  CHECK(has(names, "Combined"));
  CHECK(has(names, "AR Bounces"));  // statistics stay on
  s.read_film([](const FilmReadContext& c) { CHECK(c.film.layout().find("aov_position") == nullptr); });
}

// The Position pass of the render rectangle, read as ar_session_read_pass reads it.
std::vector<float> read_position(Session& s) {
  const std::vector<PassDesc> passes = list_passes(s.layout());
  const auto it = std::find_if(passes.begin(), passes.end(), [](const PassDesc& p) { return p.name == "Position"; });
  REQUIRE(it != passes.end());
  std::vector<float> out;
  s.read_film([&](const FilmReadContext& c) {
    FilmView view(c.device, c.film);
    out.resize(pass_float_count(*it, AR_QUANTITY_MEAN, view.width(), view.height()));
    read_pass(view, *it, AR_QUANTITY_MEAN, uint32_t(out.size()), out.data());
  });
  return out;
}

struct Unsupported {
  uint32_t entries = 0;
  uint64_t count = 0;
};

Unsupported unsupported_pass(Session& s, const std::string& name) {
  Unsupported r;
  const ar_report_info info = s.report_snapshot();
  for (uint32_t i = 0; i < info.entry_count; ++i) {
    ar_report_entry e{};
    s.report_entry(i, &e);
    const char* d = reinterpret_cast<const char*>(e.detail);
    const std::string detail(d, std::find(d, d + sizeof e.detail, '\0'));
    if (e.code == AR_REPORT_PASS_UNSUPPORTED && detail == name) {
      ++r.entries;
      r.count += e.count;
    }
  }
  return r;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("session passes/the film follows the latched pass request") {
  PassRig rig;
  rig.build({});
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 1);
  SessionOptions o;
  o.max_width = kW;
  o.max_height = kH;
  Session s(rig.scene, *dev, o);
  s.set_view(view_of(kW, kH));
  s.start();
  settle(s, rig);
  check_no_aov(s);  // no passes requested

  rig.commit([&](Edit& e) { rig.settings(e, {"Combined", "Position"}); });
  settle(s, rig);
  const std::vector<std::string> names = pass_names(s);
  for (const char* aov : {"Position", "Normal", "UV", "IndexMA"}) CHECK(has(names, aov));
  const std::vector<float> pos = read_position(s);
  REQUIRE(pos.size() == size_t(kW) * kH * 3);
  // Every pixel hits the quad at z = -1, so every Position is finite and on that plane.
  for (uint32_t j = 0; j < kW * kH; ++j) {
    for (uint32_t k = 0; k < 3; ++k) CHECK(std::isfinite(pos[j * 3 + k]));
    CHECK(std::fabs(pos[j * 3 + 2] + 1.0f) < 1e-4f);
  }

  rig.commit([&](Edit& e) { rig.settings(e, {"Combined"}); });
  settle(s, rig);
  check_no_aov(s);  // the request was dropped: the film was rebuilt without the AOVs
}

TEST_CASE("session passes/an unknown pass gives one report entry per settings change") {
  PassRig rig;
  rig.build({"Position", "AR No Such Pass"});
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 1);
  SessionOptions o;
  o.max_width = kW;
  o.max_height = kH;
  Session s(rig.scene, *dev, o);
  s.set_view(view_of(kW, kH));
  s.start();
  settle(s, rig);
  Unsupported r = unsupported_pass(s, "AR No Such Pass");
  CHECK(r.entries == 1);
  CHECK(r.count == 1);
  CHECK(unsupported_pass(s, "Position").entries == 0);
  CHECK(has(pass_names(s), "Position"));  // the known name is still served

  rig.commit([](Edit&) {});  // a commit without settings: the settings did not change
  settle(s, rig);
  r = unsupported_pass(s, "AR No Such Pass");
  CHECK(r.entries == 1);
  CHECK(r.count == 1);

  rig.commit([&](Edit& e) { rig.settings(e, {"AR No Such Pass"}); });  // a settings change
  settle(s, rig);
  r = unsupported_pass(s, "AR No Such Pass");
  CHECK(r.entries == 1);
  CHECK(r.count == 2);
  CHECK(s.progress().state == AR_STATE_CONVERGED);
}

}  // TEST_SUITE

// SPDX-License-Identifier: Apache-2.0
// 0001 V15, the parts that need a sample the engine cannot produce from valid input: a material
// whose program makes the VM fail at a shading point gives an INVALID_SAMPLE entry whose count is
// the number of failed samples, and a film reset (a new epoch) removes the FILM-scope entries.
// The program is the gate-hook op TEST_INJECT_NAN aimed at a register past the file (BAD_REGISTER):
// a NaN that really is written never gets that far, because the lobe stack and the colour lift
// clamp or gate it (a NaN in any record slot only darkens the sample: probed on base_weight,
// base_diffuse_roughness, specular_ior and closure_weight), while a VM error ends the sample INVALID
// in the same branch as a non-finite contribution (shade_surface.h: `!ok || !is_finite`).
// The host graph compiler refuses test ops (graph.cpp: they are the compiler's, never the addon's),
// so the test compiles an ordinary material whose surface entry is not folded away and overwrites its
// program words in place, with the same length, before the session builds its tables. The tables
// reference those arrays, so the session runs the patched program.
// Ref: docs/decisions/0001-*.md V15; 0008 d6 (invalid samples counted, never averaged); 0004 D17.
#include <doctest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/film/film_view.h"
#include "host/session/readback.h"
#include "host/session/session.h"
#include "vm/ops.h"
#include "vm/ops/store_slot.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kW = 6, kH = 4, kSpp = 4;
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
  void settings(Edit& e, uint32_t seed) {
    ar_settings s{};
    s.samples = s.viewport_samples = kSpp;
    s.max_bounces = 2;
    s.pixel_filter = AR_FILTER_BOX;
    s.pixel_filter_width = 1.0f;
    s.film_exposure = 1.0f;
    s.seed = seed;
    REQUIRE(e.set_settings(&s) == AR_OK);
  }
  void world(Edit& e) {  // constant white world: the NEE sample every first hit takes
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
  void quad(Edit& e) {
    const float h = 20.0f, z = -1.0f;
    const std::vector<float> pos{-h, -h, z, h, -h, z, h, h, z, -h, h, z};
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
  // base_color from the UV (a per-point value, so the surface entry is a real program).
  void material(Edit& e) {
    std::vector<ar_graph_node> nodes(2);
    std::vector<ar_graph_link> links{{0, 0, 1, 0}};
    std::vector<int32_t> i{0, 0};  // uv set 0; record slot base_color is set below
    nodes[0].op = uint32_t(ar::Op::LOAD_CTX_UV), nodes[0].i32_first = 0, nodes[0].i32_count = 1;
    nodes[1].op = uint32_t(ar::Op::STORE_SLOT), nodes[1].i32_first = 1, nodes[1].i32_count = 1;
    i[1] = int32_t(ar::RecordSlot::base_color);
    ar_graph_desc d = {};
    d.node_count = 2, d.link_count = 1, d.i32_count = 2, d.nodes = nodes.data(), d.links = links.data(), d.params_i32 = i.data();
    REQUIRE(scene.materials->set_graph(e, kGraph, &d) == AR_OK);
    ar_material_desc m = {};
    m.surface_graph = kGraph;
    m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
    REQUIRE(scene.materials->set_material(e, kMaterial, &m) == AR_OK);
  }
  // A plain constant material replacing the NaN one (a different key: the edit replaces the slice).
  void clean_material(Edit& e) {
    ar_graph_desc d = {};
    REQUIRE(scene.materials->set_graph(e, kGraph, &d) == AR_OK);
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
  // Overwrites the surface program of the UV material, word for word, with
  //   TEST_INJECT_NAN r40 (past the register file: VmStatus::BAD_REGISTER); STORE_SLOT; END
  // which is as long as the program the compiler emitted for LOAD_CTX_UV; STORE_SLOT -> base_color.
  // `reg`: the register TEST_INJECT_NAN writes; 40 is past the file (BAD_REGISTER), a register inside
  // it writes a real NaN that STORE_SLOT (source register 0, so `reg` 0 reaches it) must refuse with
  // NON_FINITE_INPUT.
  void inject_nan(uint32_t reg = 40u) {
    const auto arrays = scene.materials->arrays();
    auto& a = const_cast<MaterialArrays&>(*arrays);  // the store made it non-const; see the header comment
    REQUIRE(a.keys.size() == 1);
    uint32_t* p = a.programs.data() + a.headers[0].program_offset;
    REQUIRE(a.programs.size() - a.headers[0].program_offset == 8);
    REQUIRE(p[4] != 0);  // LOAD_CTX_UV, not END
    p[4] = uint32_t(ar::Op::TEST_INJECT_NAN) | (reg << 8);
    p[5] = uint32_t(ar::Op::STORE_SLOT);
    p[6] = uint32_t(ar::RecordSlot::base_weight);
    p[7] = uint32_t(ar::Op::END);
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

// Converged on the rig's last commit, in an epoch after `after_epoch`.
void settle(Session& s, const Rig& rig, uint64_t after_epoch) {
  REQUIRE(until([&] {
    const SessionProgress p = s.progress();
    return p.state == AR_STATE_CONVERGED && p.active_generation == rig.generation && p.epoch > after_epoch;
  }));
  REQUIRE(s.failure().empty());
}

// The counts of the report's entries with `code`.
std::vector<uint64_t> counts(Session& s, uint32_t code) {
  std::vector<uint64_t> out;
  const ar_report_info info = s.report_snapshot();
  for (uint32_t i = 0; i < info.entry_count; ++i) {
    ar_report_entry e{};
    s.report_entry(i, &e);
    if (e.code == code) out.push_back(e.count);
  }
  return out;
}

struct Fixture {
  Rig rig;
  std::shared_ptr<Device> dev;
  std::unique_ptr<Session> s;
  explicit Fixture(uint32_t inject_reg = 40u) {
    rig.commit([&](Edit& e) {
      rig.quad(e);
      rig.world(e);
      rig.material(e);
      rig.settings(e, 3);
    });
    rig.commit([&](Edit& e) { rig.instance(e); });
    rig.inject_nan(inject_reg);
    dev = make_cpu_session_device(rig.ctx, 1);
    SessionOptions o;
    o.max_width = kW;
    o.max_height = kH;
    s = std::make_unique<Session>(rig.scene, *dev, o);
    s->set_view(view_of(kW, kH));
    s->start();
    settle(*s, rig, 0);
  }
};

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("v15/invalid sample count equals the injected samples") {
  Fixture f;
  // Every camera sample first hits the quad, whose program fails, and is marked invalid once.
  const std::vector<uint64_t> c = counts(*f.s, AR_REPORT_INVALID_SAMPLE);
  REQUIRE(c.size() == 1);
  CHECK(c[0] == uint64_t(kW) * kH * kSpp);
}

TEST_CASE("v15/nan input counts invalid") {
  // The same program, but the NaN is really written into register 0 and STORE_SLOT reads it: the
  // store refuses it (NON_FINITE_INPUT) and the sample ends INVALID, once per camera sample, with no
  // clamp or lift hiding it (ruling 91). An invalid sample adds nothing to the film (0008 d6), so no
  // pixel is black without a count.
  Fixture f(0u);
  const std::vector<uint64_t> c = counts(*f.s, AR_REPORT_INVALID_SAMPLE);
  REQUIRE(c.size() == 1);
  CHECK(c[0] == uint64_t(kW) * kH * kSpp);
  const std::vector<PassDesc> passes = list_passes(f.s->layout());
  REQUIRE(passes.front().name == "Combined");
  std::vector<float> m;
  f.s->read_film([&](const FilmReadContext& ctx) {
    FilmView view(ctx.device, ctx.film);
    m.resize(pass_float_count(passes.front(), AR_QUANTITY_MEAN, view.width(), view.height()));
    read_pass(view, passes.front(), AR_QUANTITY_MEAN, uint32_t(m.size()), m.data());
  });
  REQUIRE(!m.empty());
  // Every pixel is the quad and every sample of it was refused: a pixel with no valid sample reads NaN
  // (no data), never a black 0 that looks like a result.
  for (float v : m) CHECK(std::isnan(v));
}

TEST_CASE("v15/a film reset removes the FILM entries") {
  Fixture f;
  REQUIRE(counts(*f.s, AR_REPORT_INVALID_SAMPLE).size() == 1);  // the count is the other case's
  // Replacing the material starts a new epoch, which clears the film and its FILM entries; the
  // replacement cannot inject, so nothing raises them again. (A settings-only reset would
  // rebuild the material tables and drop the patched program.)
  const uint64_t epoch2 = f.s->progress().epoch;
  REQUIRE(epoch2 >= 1);
  f.rig.commit([&](Edit& e) { f.rig.clean_material(e); });
  settle(*f.s, f.rig, epoch2);
  CHECK(counts(*f.s, AR_REPORT_INVALID_SAMPLE).empty());
}

}  // TEST_SUITE

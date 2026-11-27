// SPDX-License-Identifier: Apache-2.0
// The session's film follows the latched settings' pass list: no request allocates no primary-hit
// AOV, Position adds the four of them (Position, Normal, UV, IndexMA) and Position reads the hit
// point, a commit that drops the request rebuilds the film without them, and a name the engine
// cannot produce gives one report entry per settings change and never fails the render. The quad
// emits, so the rebuilt film's Combined shows that the rebuild re-uploaded the session tables.
// The pass list is read as ar_session_pass_count reads it (list_passes of Session::layout()); values
// through read_pass on a FilmView of the film under the render lock. The C API path, with its
// revision check (ruling 75), is tests/abi/test_session_passes.py.
// Depth and Alpha (d10): a plane's Depth is the view-space z of the hit at sample 0 and reads 1e10
// where nothing was hit; with film_transparent a ray that scattered nowhere adds no world radiance and
// its transparency, so Alpha is the covered fraction of the pixel (exactly 1 and 0 with the
// pixel-centre hook), an opacity card gives its opacity, and with the setting off Alpha is 1 and
// Combined is what it was before the planes existed (the golden hashes).
// Ref: docs/decisions/0008-film-outputs-denoise.md d10, d13a; 0007 d25; RULINGS 74 (PASS_UNSUPPORTED);
// Cycles kernel/film/read.h (alpha and depth conversions), kernel/integrator/shade_background.h.
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "host/film/film_view.h"
#include "host/light/lamps.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/session/readback.h"
#include "host/session/session.h"
#include "vm/ops.h"
#include "vm/ops/store_slot.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kW = 6, kH = 4;
constexpr ar_key kLamp = 700, kMesh = 100, kMaterial = 1, kGraph = 1001, kWorldGraph = 1002;
constexpr uint32_t kSpp = 16;
// A white emitter: its display value is kStrength in each channel (0008 d7; the analytic A5 scene
// checks the same mapping).
constexpr float kStrength = 0.5f;
// FNV-1a hashes of Combined (float bits) of the emitter quad at exposure 1 and 2, as the build before the
// Depth and Alpha planes rendered it.
constexpr uint64_t kGolden1 = 7879973154604098651ull, kGolden2 = 10155126523775399412ull;

struct PassRig {
  ContextState ctx;
  Scene scene{ctx, SceneConfig{2, {}, {}}};
  uint64_t generation = 0;
  float exposure = 1.0f;  // ar_settings.film_exposure of the next settings commit
  uint32_t spp = kSpp;
  bool film_transparent = false;  // ar_settings.film_transparent of the next settings commit
  // The quad's four corners (x, y, z each); the default fills the camera's view at z = -1.
  std::vector<float> quad_pos{-20, -20, -1, 20, -20, -1, 20, 20, -1, -20, 20, -1};
  // The quad's material is a black card of this opacity (no emission) instead of the white emitter.
  bool card = false;
  float opacity = 0.5f;
  float world = 0.0f;  // > 0: a constant white world of that strength
  // A camera-visible sphere lamp (radius lamp_radius, centred at lamp_at) between the camera and the world.
  bool lamp = false;
  float lamp_at[3] = {0, 0, -2};
  float lamp_radius = 0.3f;
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
    s.samples = s.viewport_samples = spp;
    s.max_bounces = 0;
    s.transparent_bounces = 8;
    s.film_transparent = film_transparent ? 1u : 0u;
    s.pixel_filter = AR_FILTER_BOX;
    s.pixel_filter_width = 1.0f;
    s.film_exposure = exposure;
    REQUIRE(e.set_settings(&s) == AR_OK);
  }
  // A material-less quad at z = -1 that fills the camera's view, with one UV layer.
  void quad(Edit& e) {
    const std::vector<float>& pos = quad_pos;
    const std::vector<float> uv{0, 0, 1, 0, 1, 1, 0, 1};
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
  void material(Edit& e) {
    if (card) {
      // A constant opacity into the record's geometry_opacity slot: a null event with probability
      // 1 - opacity at each hit, otherwise the path ends (max_bounces 0) with nothing to add.
      std::vector<ar_graph_node> nodes(2);
      std::vector<ar_graph_link> links{{0, 0, 1, 0}};
      std::vector<float> f{opacity};
      std::vector<int32_t> slot{int32_t(ar::RecordSlot::geometry_opacity)};
      nodes[0].op = uint32_t(ar::Op::CONST_F32), nodes[0].f32_count = 1;
      nodes[1].op = uint32_t(ar::Op::STORE_SLOT), nodes[1].i32_count = 1;
      ar_graph_desc d = {};
      d.node_count = 2, d.link_count = 1, d.f32_count = 1, d.i32_count = 1;
      d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data(), d.params_i32 = slot.data();
      REQUIRE(scene.materials->set_graph(e, kGraph, &d) == AR_OK);
      ar_material_desc m = {};
      m.surface_graph = kGraph;
      m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
      REQUIRE(scene.materials->set_material(e, kMaterial, &m) == AR_OK);
      return;
    }
    std::vector<ar_graph_node> nodes(3);
    std::vector<ar_graph_link> links{{0, 0, 2, 0}, {1, 0, 2, 1}};
    std::vector<float> f{1.0f, 1.0f, 1.0f, kStrength};
    nodes[0].op = uint32_t(ar::Op::CONST_F32X3), nodes[0].f32_count = 3;
    nodes[1].op = uint32_t(ar::Op::CONST_F32), nodes[1].f32_first = 3, nodes[1].f32_count = 1;
    nodes[2].op = uint32_t(ar::Op::EMIT_LE);
    ar_graph_desc d = {};
    d.node_count = 3, d.link_count = 2, d.f32_count = 4, d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data();
    REQUIRE(scene.materials->set_graph(e, kGraph, &d) == AR_OK);
    ar_material_desc m = {};
    m.surface_graph = kGraph;
    m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
    REQUIRE(scene.materials->set_material(e, kMaterial, &m) == AR_OK);
  }
  void world_light(Edit& e) {
    std::vector<ar_graph_node> nodes(3);
    std::vector<ar_graph_link> links{{0, 0, 2, 0}, {1, 0, 2, 1}};
    std::vector<float> f{1.0f, 1.0f, 1.0f, world};
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
  void sphere_lamp(Edit& e) {
    ar_light_desc d = {};
    d.type = AR_LIGHT_POINT;
    d.color.kind = AR_SPECTRUM_RGB;
    d.color.rgb[0] = d.color.rgb[1] = d.color.rgb[2] = 1.0f;
    d.xf.m[0] = d.xf.m[5] = d.xf.m[10] = 1.0f;
    d.xf.m[3] = lamp_at[0], d.xf.m[7] = lamp_at[1], d.xf.m[11] = lamp_at[2];
    d.visibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE;
    d.size_x = lamp_radius;
    d.power = 10.0f, d.normalize = 1, d.cast_shadow = 1;
    REQUIRE(set_light(e, kLamp, &d, scene.materials->table()) == AR_OK);
  }
  void instance(Edit& e) {
    std::vector<ar_key> geom{kMesh}, object{500};
    std::vector<ar_xform> xf(1);
    xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
    std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA), one(1, 1);
    std::vector<ar_key> pool{kMaterial};
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
  void build(const std::vector<std::string>& passes) {
    commit([&](Edit& e) {
      quad(e);
      material(e);
      if (world > 0.0f) world_light(e);
      if (lamp) sphere_lamp(e);
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
  for (const char* aov : {"Depth", "Position", "Normal", "UV", "IndexMA", "Alpha"}) CHECK_FALSE(has(names, aov));
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

// Any pass of the render rectangle (the mean), as ar_session_read_pass reads it.
std::vector<float> read_named(Session& s, const std::string& name) {
  const std::vector<PassDesc> passes = list_passes(s.layout());
  const auto it = std::find_if(passes.begin(), passes.end(), [&](const PassDesc& p) { return p.name == name; });
  REQUIRE(it != passes.end());
  std::vector<float> out;
  s.read_film([&](const FilmReadContext& c) {
    FilmView view(c.device, c.film);
    out.resize(pass_float_count(*it, AR_QUANTITY_MEAN, view.width(), view.height()));
    read_pass(view, *it, AR_QUANTITY_MEAN, uint32_t(out.size()), out.data());
  });
  return out;
}

// ASTRORAY_TEST_PIXEL_CENTRE for the scope of a session's construction (the session reads it at create):
// every camera ray then crosses its pixel's centre, so coverage is exact.
struct CentreHook {
  explicit CentreHook(bool on) { _putenv_s("ASTRORAY_TEST_PIXEL_CENTRE", on ? "1" : ""); }
  ~CentreHook() { _putenv_s("ASTRORAY_TEST_PIXEL_CENTRE", ""); }
};

using Planes = std::map<std::string, std::vector<float>>;

// Builds the rig, renders it to convergence on the CPU with one worker and returns the named passes. The
// aov_depth film plane as stored (not as read) comes back under "stored depth".
Planes render_planes(PassRig& rig, const std::vector<std::string>& passes, bool centre,
                     const ar_view_desc& view = view_of(kW, kH)) {
  rig.build(passes);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 1);
  SessionOptions o;
  o.max_width = kW;
  o.max_height = kH;
  std::unique_ptr<Session> s;
  {
    CentreHook hook(centre);
    s = std::make_unique<Session>(rig.scene, *dev, o);
  }
  s->set_view(view);
  s->start();
  settle(*s, rig);
  Planes out;
  for (const std::string& name : passes) out[name] = read_named(*s, name);
  s->read_film([&](const FilmReadContext& c) {
    FilmView fv(c.device, c.film);
    const std::vector<PassDesc> listed = list_passes(c.film.layout());
    if (const auto it = std::find_if(listed.begin(), listed.end(), [](const PassDesc& p) { return p.name == "Combined"; });
        it != listed.end()) {
      std::vector<float>& var = out["Combined variance"];
      var.resize(pass_float_count(*it, AR_QUANTITY_VARIANCE, fv.width(), fv.height()));
      read_pass(fv, *it, AR_QUANTITY_VARIANCE, uint32_t(var.size()), var.data());
    }
    if (c.film.layout().find("aov_depth") != nullptr) {
      const std::vector<double> d = fv.mean("aov_depth", 0);
      for (double z : d) out["stored depth"].push_back(float(z));
    }
  });
  return out;
}

// The camera ray of pixel (column i, row j from the bottom) through its centre at z = -1 in view_of's
// camera: film plane half-extents 0.5 and 0.375.
double col_u(uint32_t i) { return ((double(i) + 0.5) / kW * 2.0 - 1.0) * 0.5; }
double row_v(uint32_t j) { return ((double(j) + 0.5) / kH * 2.0 - 1.0) * 0.375; }

// FNV-1a over the float bits: a golden of a whole plane.
uint64_t hash_bits(const std::vector<float>& v) {
  uint64_t h = 1469598103934665603ull;
  for (float f : v) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    for (int k = 0; k < 4; ++k) h = (h ^ ((b >> (8 * k)) & 0xffu)) * 1099511628211ull;
  }
  return h;
}

// Combined averaged over the render rectangle, per channel, and that average's standard error
// from the per-pixel variances of the mean.
void combined_average(Session& s, double mean[3], double se[3]) {
  const std::vector<PassDesc> passes = list_passes(s.layout());
  REQUIRE(passes.front().name == "Combined");
  std::vector<float> m, v;
  s.read_film([&](const FilmReadContext& c) {
    FilmView view(c.device, c.film);
    m.resize(pass_float_count(passes.front(), AR_QUANTITY_MEAN, view.width(), view.height()));
    v.resize(m.size());
    read_pass(view, passes.front(), AR_QUANTITY_MEAN, uint32_t(m.size()), m.data());
    read_pass(view, passes.front(), AR_QUANTITY_VARIANCE, uint32_t(v.size()), v.data());
  });
  const size_t pixels = m.size() / 3;
  for (uint32_t k = 0; k < 3; ++k) {
    double sm = 0.0, sv = 0.0;
    for (size_t j = 0; j < pixels; ++j) sm += m[j * 3 + k], sv += v[j * 3 + k];
    mean[k] = sm / double(pixels);
    se[k] = std::sqrt(sv) / double(pixels);
  }
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
  for (const char* aov : {"Depth", "Position", "Normal", "UV", "IndexMA"}) CHECK(has(names, aov));
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
  // The rebuilt film integrates the emitter: its response and wavelength tables were re-uploaded.
  double mean[3], se[3];
  combined_average(s, mean, se);
  for (uint32_t k = 0; k < 3; ++k) {
    MESSAGE("Combined after the rebuild, channel " << k << ": " << mean[k] << " (expected " << kStrength
                                                   << "), se " << se[k]);
    CHECK(std::isfinite(se[k]));
    CHECK(std::fabs(mean[k] - kStrength) <= 5.0 * se[k] + 1e-6);
  }
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

TEST_CASE("session passes/film exposure scales the display channel") {
  // ar_settings.film_exposure reaches the film: the display channel doubles at exposure 2 while a
  // science channel does not move (0008 d7). Every commit starts an epoch, so no sample is mixed.
  // A counter-based sampler draws the same wavelengths in each epoch, hence the tight tolerance.
  PassRig rig;
  rig.build({"Combined", "Position"});
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 1);
  SessionOptions o;
  o.max_width = kW;
  o.max_height = kH;
  Session s(rig.scene, *dev, o);
  s.set_view(view_of(kW, kH));
  s.start();
  settle(s, rig);
  const auto read = [&](const char* name) {
    const std::vector<PassDesc> passes = list_passes(s.layout());
    const auto it = std::find_if(passes.begin(), passes.end(), [&](const PassDesc& p) { return p.name == name; });
    REQUIRE(it != passes.end());
    std::vector<float> out;
    s.read_film([&](const FilmReadContext& c) {
      FilmView view(c.device, c.film);
      out.resize(pass_float_count(*it, AR_QUANTITY_MEAN, view.width(), view.height()));
      read_pass(view, *it, AR_QUANTITY_MEAN, uint32_t(out.size()), out.data());
    });
    return out;
  };
  const std::vector<float> c1 = read("Combined"), p1 = read("Position");
  rig.exposure = 2.0f;
  rig.commit([&](Edit& e) { rig.settings(e, {"Combined", "Position"}); });
  settle(s, rig);
  const std::vector<float> c2 = read("Combined"), p2 = read("Position");
  REQUIRE(c1.size() == c2.size());
  REQUIRE(p1.size() == p2.size());
  for (size_t i = 0; i < c1.size(); ++i) {
    REQUIRE(c1[i] > 0.0f);
    CHECK(std::fabs(c2[i] - 2.0f * c1[i]) <= 1e-6 * std::fabs(c2[i]));
  }
  for (size_t i = 0; i < p1.size(); ++i) CHECK(p2[i] == p1[i]);
}

TEST_CASE("session passes/film exposure that is not finite and >= 0 is refused") {
  // A NaN, Inf or negative exposure would put garbage into every display moment at resolve.
  for (const float bad : {std::nanf(""), INFINITY, -1.0f}) {
    PassRig rig;
    auto e = rig.scene.begin_edit();
    REQUIRE(e);
    rig.exposure = bad;
    rig.settings(*e, {"Combined"});
    uint64_t g = 0;
    CHECK(e->commit(&g) == AR_ERR_INVALID_ARGUMENT);
    CHECK(e->last_error().find("film_exposure") != std::string::npos);
  }
}

TEST_CASE("session passes/depth is the view space z of the hit") {
  // A plane at z = -1: every pixel's ray has length 1 / cos(theta) but the view-space z is 1.
  {
    PassRig rig;
    const Planes p = render_planes(rig, {"Combined", "Depth"}, true);
    REQUIRE(p.at("Depth").size() == size_t(kW) * kH);
    for (uint32_t j = 0; j < kH; ++j)
      for (uint32_t i = 0; i < kW; ++i) CHECK(std::fabs(p.at("Depth")[j * kW + i] - 1.0f) <= 1e-5f);
  }
  // A tilted plane z = -2 + x / 2 (corners inside the view's front): the ray of film point (u, v) is
  // t (u, v, -1), so it meets the plane at view-space z t = 2 / (1 + u / 2), while its length is t sqrt(u^2 + v^2 + 1).
  PassRig rig;
  rig.quad_pos = {-3, -3, -3.5f, 3, -3, -0.5f, 3, 3, -0.5f, -3, 3, -3.5f};
  const Planes p = render_planes(rig, {"Combined", "Depth"}, true);
  double worst = 0.0, length_gap = 0.0;
  for (uint32_t j = 0; j < kH; ++j)
    for (uint32_t i = 0; i < kW; ++i) {
      const double u = col_u(i), v = row_v(j), t = 2.0 / (1.0 + 0.5 * u);
      worst = std::max(worst, std::fabs(double(p.at("Depth")[j * kW + i]) - t) / t);
      length_gap = std::max(length_gap, t * std::sqrt(u * u + v * v + 1.0) - t);
    }
  MESSAGE("tilted plane: worst relative error of Depth " << worst << ", the ray length would differ by up to " << length_gap);
  CHECK(worst <= 1e-5);
  CHECK(length_gap > 0.05);
}

TEST_CASE("session passes/depth is the same at one and sixteen samples") {
  // Sample 0's z is kept (Cycles film_overwrite_pass_float at sample 0), so the plane does not depend
  // on how many samples follow, and with jittered film positions it is not the pixel-centre z that the
  // mean of the samples would approach.
  Planes one, many;
  {
    PassRig rig;
    rig.quad_pos = {-3, -3, -3.5f, 3, -3, -0.5f, 3, 3, -0.5f, -3, 3, -3.5f};
    rig.spp = 1;
    one = render_planes(rig, {"Combined", "Depth"}, false);
  }
  {
    PassRig rig;
    rig.quad_pos = {-3, -3, -3.5f, 3, -3, -0.5f, 3, 3, -0.5f, -3, 3, -3.5f};
    rig.spp = 16;
    many = render_planes(rig, {"Combined", "Depth"}, false);
  }
  REQUIRE(one.at("Depth").size() == many.at("Depth").size());
  double off_centre = 0.0;
  for (uint32_t j = 0; j < kH; ++j)
    for (uint32_t i = 0; i < kW; ++i) {
      const size_t k = size_t(j) * kW + i;
      CHECK(one.at("Depth")[k] == many.at("Depth")[k]);
      off_centre = std::max(off_centre, std::fabs(double(many.at("Depth")[k]) - 2.0 / (1.0 + 0.5 * col_u(i))));
    }
  MESSAGE("largest distance of the sample-0 depth from the pixel-centre depth: " << off_centre);
  CHECK(off_centre > 1e-3);
}

TEST_CASE("session passes/alpha is the covered fraction with film transparent") {
  // Half the view is a card (x in [-3, 0] at z = -1: columns 0-2 of 6), a world of strength 0.5 behind it.
  const std::vector<float> half{-3, -3, -1, 0, -3, -1, 0, 3, -1, -3, 3, -1};
  for (const bool on : {true, false}) {
    PassRig rig;
    rig.quad_pos = half;
    rig.world = 0.5f;
    rig.film_transparent = on;
    const Planes p = render_planes(rig, {"Combined", "Depth", "Alpha"}, true);
    for (uint32_t j = 0; j < kH; ++j)
      for (uint32_t i = 0; i < kW; ++i) {
        const size_t k = size_t(j) * kW + i;
        const bool covered = i < 3;
        INFO("film transparent " << on << ", pixel (" << i << ", " << j << ")");
        // The pixel-centre hook makes coverage exact: 1 and 0, no partial value.
        CHECK(p.at("Alpha")[k] == (on && !covered ? 0.0f : 1.0f));
        CHECK(p.at("Depth")[k] == (covered ? p.at("Depth")[k] : 1e10f));
        if (covered) CHECK(std::fabs(p.at("Depth")[k] - 1.0f) <= 1e-5f);
        // The stored plane holds 0 where nothing was hit, whatever the world shows there.
        CHECK(p.at("stored depth")[k] == (covered ? p.at("stored depth")[k] : 0.0f));
        for (uint32_t c = 0; c < 3; ++c) {
          const float v = p.at("Combined")[k * 3 + c];
          if (covered) CHECK(v > 0.9f * kStrength);  // the emitter, never the world
          else if (on) CHECK(v == 0.0f);             // the world is invisible to a ray that scattered nowhere
          else CHECK(v > 0.0f);                      // the world, as before
        }
      }
  }
  // Without the hook an edge pixel's alpha is its covered fraction: the card ends at x = 0.05, 0.3 of the
  // way across column 3 (u from 0 to 1/6), so alpha = 0.3 with a binomial standard error at 256 samples.
  PassRig rig;
  rig.quad_pos = {-3, -3, -1, 0.05f, -3, -1, 0.05f, 3, -1, -3, 3, -1};
  rig.world = 0.5f;
  rig.film_transparent = true;
  rig.spp = 256;
  const Planes p = render_planes(rig, {"Combined", "Alpha"}, false);
  const double se = std::sqrt(0.3 * 0.7 / 256.0);
  for (uint32_t j = 0; j < kH; ++j) {
    for (uint32_t i = 0; i < kW; ++i) {
      const float a = p.at("Alpha")[size_t(j) * kW + i];
      if (i < 3) CHECK(a == 1.0f);
      else if (i > 3) CHECK(a == 0.0f);
      else {
        MESSAGE("edge pixel row " << j << ": alpha " << a << " (covered fraction 0.3, se " << se << ")");
        CHECK(std::fabs(double(a) - 0.3) <= 3.0 * se);
      }
    }
  }
}

TEST_CASE("session passes/an opacity card gives its opacity as alpha and drops the world") {
  // A black card of opacity 0.5 filling the view over a world of strength 0.5, 256 samples: each
  // sample hits the card or passes through to the world with probability 1/2, so with film
  // transparent Alpha = 0.5 and Combined (the card adds nothing) is 0; with it off Combined is half the
  // world and Alpha is 1.
  const double se_pixel = std::sqrt(0.25 / 256.0), se_mean = se_pixel / std::sqrt(double(kW * kH));
  Planes on, off;
  {
    PassRig rig;
    rig.card = true;
    rig.world = 0.5f;
    rig.film_transparent = true;
    rig.spp = 256;
    on = render_planes(rig, {"Combined", "Alpha"}, false);
  }
  {
    PassRig rig;
    rig.card = true;
    rig.world = 0.5f;
    rig.spp = 256;
    off = render_planes(rig, {"Combined", "Alpha"}, false);
  }
  double mean_alpha = 0.0;
  for (size_t k = 0; k < on.at("Alpha").size(); ++k) {
    mean_alpha += on.at("Alpha")[k];
    CHECK(std::fabs(double(on.at("Alpha")[k]) - 0.5) <= 5.0 * se_pixel);
    CHECK(off.at("Alpha")[k] == 1.0f);
  }
  mean_alpha /= double(on.at("Alpha").size());
  MESSAGE("opacity card: mean Alpha " << mean_alpha << " (expected 0.5, se of the mean " << se_mean << ")");
  CHECK(std::fabs(mean_alpha - 0.5) <= 3.0 * se_mean);
  for (float v : on.at("Combined")) CHECK(v == 0.0f);
  // The world alone, from a card that is always passed through: the half of it that the off render shows.
  PassRig clear;
  clear.card = true;
  clear.opacity = 0.0f;
  clear.world = 0.5f;
  clear.spp = 256;
  const Planes world = render_planes(clear, {"Combined"}, false);
  // Per channel: the average of the off render against half the world's, with the standard errors of
  // both averages from the per-pixel variances of the mean.
  const size_t pixels = size_t(kW) * kH;
  for (uint32_t k = 0; k < 3; ++k) {
    double w = 0.0, c = 0.0, vw = 0.0, vc = 0.0;
    for (size_t j = 0; j < pixels; ++j) {
      w += world.at("Combined")[j * 3 + k], c += off.at("Combined")[j * 3 + k];
      vw += world.at("Combined variance")[j * 3 + k], vc += off.at("Combined variance")[j * 3 + k];
    }
    w /= double(pixels), c /= double(pixels);
    const double se = std::sqrt(vc + 0.25 * vw) / double(pixels);
    MESSAGE("channel " << k << ": world alone " << w << ", behind the 0.5 card with the setting off " << c << " (se " << se << ")");
    REQUIRE(w > 0.0);
    CHECK(std::fabs(c - 0.5 * w) <= 4.0 * se);
  }
}

TEST_CASE("session passes/a camera ray through a visible lamp is still a transparent-background ray") {
  // A sphere lamp between the camera and the world, Film Transparent on. Cycles counts a lamp crossing as
  // a transparent bounce (integrator/shade_light.h, "count this as a transparent bounce"): the lamp's
  // emission is Combined's, the ray carries on with PATH_RAY_TRANSPARENT_BACKGROUND set, and reaching the
  // world it adds transparency 1. So Alpha is 0 behind the lamp too, and Combined shows the lamp only
  // where a ray crossed it. The quad sits behind the camera so it is never hit.
  PassRig rig;
  rig.quad_pos = {-20, -20, 5, 20, -20, 5, 20, 20, 5, -20, 20, 5};
  rig.world = 0.5f;
  rig.lamp = true;
  rig.lamp_at[0] = -0.2f;
  rig.film_transparent = true;
  const Planes p = render_planes(rig, {"Combined", "Alpha"}, true);
  uint32_t covered = 0, clear = 0;
  for (uint32_t j = 0; j < kH; ++j)
    for (uint32_t i = 0; i < kW; ++i) {
      // Distance of the pixel-centre ray (through (u, v, -1) from the origin) to the lamp's centre.
      const double d[3] = {col_u(i), row_v(j), -1.0}, c[3] = {rig.lamp_at[0], rig.lamp_at[1], rig.lamp_at[2]};
      const double dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], cd = c[0] * d[0] + c[1] * d[1] + c[2] * d[2];
      const double dist = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] - cd * cd / dd);
      INFO("pixel (" << i << ", " << j << "), ray to lamp-centre distance " << dist);
      REQUIRE(std::fabs(dist - rig.lamp_radius) > 0.02);  // no pixel on the silhouette: coverage is unambiguous
      const bool crossed = dist < rig.lamp_radius;
      const size_t k = size_t(j) * kW + i;
      CHECK(p.at("Alpha")[k] == 0.0f);
      for (uint32_t ch = 0; ch < 3; ++ch) {
        if (crossed) CHECK(p.at("Combined")[k * 3 + ch] > 0.0f);  // the lamp, never the world
        else CHECK(p.at("Combined")[k * 3 + ch] == 0.0f);         // the world is invisible
      }
      (crossed ? covered : clear)++;
    }
  CHECK(covered > 0u);
  CHECK(clear > 0u);
  MESSAGE("lamp crossing: " << covered << " pixels behind the lamp, " << clear << " clear, Alpha 0 on all");
}

TEST_CASE("session passes/an opacity zero card leaves the world transparent exactly") {
  // The null event of a card that is always passed through leaves the throughput exactly 1, so every
  // pixel-centre sample adds transparency exactly 1 and Alpha is exactly 0 (not 1 - 0.98^n or the like);
  // Combined shows no world.
  PassRig rig;
  rig.card = true;
  rig.opacity = 0.0f;
  rig.world = 0.5f;
  rig.film_transparent = true;
  const Planes p = render_planes(rig, {"Combined", "Alpha"}, true);
  for (size_t k = 0; k < p.at("Alpha").size(); ++k) CHECK(p.at("Alpha")[k] == 0.0f);
  for (float v : p.at("Combined")) CHECK(v == 0.0f);
}

TEST_CASE("session passes/film transparent off leaves Combined bit identical") {
  // Golden hashes of Combined for two renders that exist without the new planes (the emitter quad at
  // exposure 1 and 2), taken on the build before Depth and Alpha: with the setting off, requesting the
  // planes changes no bit of Combined and Alpha reads exactly 1.
  const uint64_t golden[2] = {kGolden1, kGolden2};
  for (int k = 0; k < 2; ++k) {
    PassRig plain;
    plain.exposure = float(k + 1);
    const Planes a = render_planes(plain, {"Combined"}, false);
    PassRig with;
    with.exposure = float(k + 1);
    const Planes b = render_planes(with, {"Combined", "Depth", "Alpha"}, false);
    MESSAGE("exposure " << k + 1 << ": Combined hash " << hash_bits(a.at("Combined")) << " with the planes " << hash_bits(b.at("Combined")));
    CHECK(a.at("Combined") == b.at("Combined"));
    CHECK(hash_bits(a.at("Combined")) == golden[k]);
    for (float v : b.at("Alpha")) CHECK(v == 1.0f);
  }
}

TEST_CASE("session passes/depth is refused for a camera that is not a pinhole") {
  PassRig rig;
  rig.build({"Combined", "Depth", "Position"});
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 1);
  SessionOptions o;
  o.max_width = kW;
  o.max_height = kH;
  Session s(rig.scene, *dev, o);
  ar_view_desc v = view_of(kW, kH);
  v.camera.type = AR_CAMERA_PANO;
  s.set_view(v);
  s.start();
  settle(s, rig);
  // One entry naming the pass, no Depth plane, the other AOVs untouched (Position is still served).
  CHECK(unsupported_pass(s, "Depth").entries == 1);
  const std::vector<std::string> names = pass_names(s);
  CHECK_FALSE(has(names, "Depth"));
  CHECK(has(names, "Position"));
  s.read_film([](const FilmReadContext& c) { CHECK(c.film.layout().find("aov_depth") == nullptr); });
}

}  // TEST_SUITE

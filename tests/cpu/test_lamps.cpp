// SPDX-License-Identifier: Apache-2.0
// Lamp ingestion (task 068): records from descriptors, approximation entries, lamp instances,
// the radiance scale and the scene's lamp input to the light table.
// Ref: 0004 D6 (conventions), D8 (lamps never block shadow rays); 0003 d10 (radiance scale);
// Cycles intern/cycles/scene/light.cpp (normalize), cited in host/light/lamps.h.
#include <doctest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "film/resolve.h"
#include "host/data_resource.h"
#include "host/film/film_layout.h"
#include "host/film/response_table.h"
#include "host/light/lamps.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/session/session.h"
#include "host/spectral/wavelength_table.h"
#include "host/thread_pool.h"
#include "stages/film_build_active.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;
using ar::Real;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct GB {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  int add(ar::Op op, std::vector<float> fp) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size()), n.f32_count = uint32_t(fp.size());
    f.insert(f.end(), fp.begin(), fp.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  explicit GB(float strength) {  // a constant Emission of colour (1, 0.5, 0.25)
    const int c = add(ar::Op::CONST_F32X3, {1.0f, 0.5f, 0.25f}), s = add(ar::Op::CONST_F32, {strength});
    const int e = add(ar::Op::EMIT_LE, {});
    links.push_back({uint32_t(c), 0u, uint32_t(e), 0u});
    links.push_back({uint32_t(s), 0u, uint32_t(e), 1u});
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size()), d.f32_count = uint32_t(f.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data();
    return d;
  }
};

struct Fixture {
  astroray::ContextState ctx;
  Scene scene{ctx, SceneConfig{2, {}}};
  Fixture() {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
  }
  const ar::Rgb2SpecTable& table() { return scene.materials->table(); }
  template <class F>
  void commit(F&& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    scene.materials->end_edit();
    e.reset();
    scene.generations().wait_idle();
    REQUIRE(scene.generations().failed() == 0);
  }
  void set_light(ar_key key, const ar_light_desc& d) {
    commit([&](Edit& e) { REQUIRE(astroray::host::set_light(e, key, &d, table()) == AR_OK); });
  }
  uint32_t approximations(ar_key subject) {
    scene.report().snapshot();
    ar_report_info info = {};
    REQUIRE(scene.report().info(&info) == AR_OK);
    uint32_t n = 0;
    for (uint32_t k = 0; k < info.entry_count; ++k) {
      ar_report_entry e = {};
      REQUIRE(scene.report().entry(k, &e) == AR_OK);
      if (e.subject == subject && e.code == AR_REPORT_NODE_APPROXIMATED) n += uint32_t(e.count);
    }
    return n;
  }
  // A lit constant world (strength `k`) and a one-quad emissive mesh (two triangles, strength 4 k)
  // instanced once.
  void world_and_quad(float k) {
    const GB gw(k), ge(4.0f * k);
    commit([&](Edit& e) {
      const ar_graph_desc dw = gw.desc(), de = ge.desc();
      ar_world_desc w = {};
      w.surface_graph = 40;
      REQUIRE(scene.materials->set_graph(e, 40, &dw) == AR_OK);
      REQUIRE(scene.materials->set_world(e, &w) == AR_OK);
      REQUIRE(scene.materials->set_graph(e, 41, &de) == AR_OK);
      ar_material_desc m = {};
      m.surface_graph = 41;
      m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
      REQUIRE(scene.materials->set_material(e, 42, &m) == AR_OK);
    });
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    commit([&](Edit& e) {
      ar_mesh_desc d = {};
      d.key = 100, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(scene, e, &d, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(100) == AR_OK);
    });
    std::vector<ar_key> geom{100}, object{1}, pool{42};
    std::vector<ar_xform> xf(1);
    xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
    std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA), moff{0}, mcount{1};
    std::vector<ar_rgba> colors(1, ar_rgba{{1, 1, 1, 1}});
    std::vector<uint8_t> lg(1, 0);
    ar_instances_desc d = {};
    d.instance_count = 1, d.motion_steps = 1, d.transform_count = 1, d.material_key_count = 1;
    d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
    d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
    d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
    d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = pool.data();
    commit([&](Edit& e) { REQUIRE(set_instances(e, &d) == AR_OK); });
  }
};

ar_light_desc desc(uint32_t type, float sx, float sy, float power) {
  ar_light_desc d = {};
  d.color.kind = AR_SPECTRUM_RGB;
  d.color.rgb[0] = d.color.rgb[1] = d.color.rgb[2] = 1.0f;
  d.xf.m[0] = d.xf.m[5] = d.xf.m[10] = 1.0f;
  d.type = type;
  d.visibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE | AR_VISIBILITY_GLOSSY | AR_VISIBILITY_TRANSMISSION;
  d.size_x = sx, d.size_y = sy;
  d.spread = float(kPi);
  d.power = power;
  d.normalize = 1;
  d.cast_shadow = 1;
  return d;
}

const ar::ObserverView& observer() {
  static astroray::ObserverTables t;
  static ar::ObserverView v = [] {
    astroray::DataBlob b;
    REQUIRE(astroray::find_data("spectral/observer.bin", &b));
    REQUIRE(astroray::parse_observer(b.data, b.size, &t).empty());
    return ar::ObserverView{&t.cmf[0][0], t.d65};
  }();
  return v;
}

double emission_at(const ar::SpectrumRef& r, float nm) {
  ar::Lambda lam;
  for (int k = 0; k < ar::kLanes; ++k) lam.nm[k] = Real(nm);
  return double(ar::resolve(r, lam, observer(), nullptr).v[0]);
}

// Flat tables as in test_film_resolve: a slot of v / 256 resolves to exactly v.
struct FilmRig {
  astroray::ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  ResponseTable response;
  WavelengthTable wavelengths;
  std::unique_ptr<Film> film;
  ar::KernelGlobals kg{};
  std::vector<uint32_t> list;
  FilmRig() : pool(ctx, 1), dev(ledger, pool) {
    response.display_first = 0;
    response.photon_scale = 1e-9 / (6.62607015e-34 * 299792458.0);
    response.y_w = 1.0;
    const double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) response.m_ws[i] = m[i];
    for (uint32_t c = 0; c < 4; ++c) {
      response.channels.push_back({2 * c, 2, ar::RESPONSE_ENERGY, 0});
      response.knot_nm.push_back(300.0f);
      response.knot_nm.push_back(900.0f);
      response.knot_value.push_back(1.0f);
      response.knot_value.push_back(1.0f);
    }
    wavelengths.edges = {360.0f, 616.0f};
    wavelengths.cdf = {0.0f, 1.0f};
    FilmRequest req;
    req.max_width = req.max_height = 1;
    req.slot_capacity = 1;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    ar::FilmData& d = film->data();
    d.response = response_view(response);
    d.wavelengths = {wavelengths.edges.data(), wavelengths.cdf.data(), wavelengths.bins()};
    kg.film = &d;
    kg.session.width = kg.session.height = 1;
    kg.session.seed = 7u;
    kg.session.sampler_mode = ar::SAMPLER_SOBOL;
    kg.session.samples_per_pass = 1;
  }
  // Resolves one slot holding `v` (what the shade stages would add) and returns S1 of channel 0.
  double resolve_one(float v) {
    list.assign(1, 0u);
    kg.active_pixels = list.data();
    cpu_film_build_active(kg, 0, ar::film_build_active_count(film->data()));
    ar::Spec L;
    for (int i = 0; i < 4; ++i) L.v[i] = v / 256.0f;
    ar::sample_add(kg, 0, L);
    ar::sample_set_flags(kg, 0, 0);
    cpu_film_resolve(kg, 0, ar::film_resolve_count(kg));
    return film->data().s1[0];
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("lamps/records from descriptors") {
  Fixture fx;
  auto build = [&](const ar_light_desc& d) { return build_lamp(1, d, fx.table()); };
  auto p = build(desc(AR_LIGHT_POINT, 0, 0, 100));
  CHECK(p->record.type == uint8_t(ar::LightType::POINT));
  CHECK(p->record.size[0] == 0.0f);
  CHECK(p->record.strength == 100.0f);
  CHECK(p->record.emission.kind == uint8_t(ar::SpectrumKind::RGB));
  CHECK(p->record.emission.semantic == uint8_t(ar::SpectrumSemantic::ILLUMINANT));
  CHECK(p->approximations.empty());
  // The colour reaches the record: a red lamp is brighter at 650 nm than at 450 nm, a blue one the reverse.
  ar_light_desc red = desc(AR_LIGHT_POINT, 0, 0, 1);
  red.color.rgb[0] = 1.0f, red.color.rgb[1] = red.color.rgb[2] = 0.05f;
  ar_light_desc blue = desc(AR_LIGHT_POINT, 0, 0, 1);
  blue.color.rgb[2] = 1.0f, blue.color.rgb[0] = blue.color.rgb[1] = 0.05f;
  CHECK(emission_at(build(red)->record.emission, 650.0f) > 2.0 * emission_at(build(red)->record.emission, 450.0f));
  CHECK(emission_at(build(blue)->record.emission, 450.0f) > 2.0 * emission_at(build(blue)->record.emission, 650.0f));
  auto s = build(desc(AR_LIGHT_POINT, 0.5f, 0, 100));
  CHECK(s->record.type == uint8_t(ar::LightType::SPHERE));
  CHECK(s->record.size[0] == 0.5f);
  // The object's scale does not resize a sphere (Cycles PointLight uses the radius as given).
  ar_light_desc scaled_sphere = desc(AR_LIGHT_POINT, 0.5f, 0, 100);
  scaled_sphere.xf.m[0] = scaled_sphere.xf.m[5] = scaled_sphere.xf.m[10] = 3.0f;
  CHECK(build(scaled_sphere)->record.size[0] == 0.5f);
  auto r = build(desc(AR_LIGHT_AREA_RECT, 2, 3, 10));
  CHECK(r->record.type == uint8_t(ar::LightType::AREA_RECT));
  CHECK(r->record.size[0] == 2.0f);
  CHECK(r->record.size[1] == 3.0f);
  // Ellipse sizes are diameters; equal ones are a disk of half the size, with no entry.
  auto e = build(desc(AR_LIGHT_AREA_ELLIPSE, 2, 2, 10));
  CHECK(e->record.type == uint8_t(ar::LightType::AREA_DISK));
  CHECK(e->record.size[0] == 1.0f);
  CHECK(e->record.size[1] == 0.0f);
  CHECK(e->approximations.empty());
  // An object scale in the transform scales an area lamp's extents.
  ar_light_desc scaled = desc(AR_LIGHT_AREA_RECT, 2, 3, 10);
  scaled.xf.m[0] = 2.0f;
  CHECK(build(scaled)->record.size[0] == 4.0f);
  // normalize: Cycles divides by its light area only when set, so the unnormalised strength
  // is P times that area (4 for a delta point, 4 pi r^2, w h, pi/4 w h).
  auto un = [&](ar_light_desc d) {
    d.normalize = 0;
    return build(d)->record.strength;
  };
  CHECK(un(desc(AR_LIGHT_POINT, 0, 0, 100)) == doctest::Approx(400.0));
  CHECK(un(desc(AR_LIGHT_POINT, 0.5f, 0, 100)) == doctest::Approx(100.0 * 4.0 * kPi * 0.25));
  CHECK(un(desc(AR_LIGHT_AREA_RECT, 2, 3, 10)) == doctest::Approx(60.0));
  CHECK(un(desc(AR_LIGHT_AREA_ELLIPSE, 2, 2, 10)) == doctest::Approx(10.0 * kPi));
  // A refused lamp is a loud error, never another light.
  CHECK_THROWS_AS(build(desc(AR_LIGHT_SPOT, 0, 0, 1)), std::invalid_argument);
  CHECK_THROWS_AS(build(desc(AR_LIGHT_POINT, 0, 0, -1)), std::invalid_argument);
  CHECK_THROWS_AS(build(desc(AR_LIGHT_AREA_RECT, 0, 1, 1)), std::invalid_argument);
  ar_light_desc bad = desc(AR_LIGHT_POINT, 0, 0, 1);
  bad.color.rgb[1] = HUGE_VALF;
  CHECK_THROWS_AS(build(bad), std::invalid_argument);
  bad = desc(AR_LIGHT_POINT, 0, 0, 1);
  bad.color.table_count = 3;  // only a TABLE colour has one
  CHECK_THROWS_AS(build(bad), std::invalid_argument);
  bad = desc(AR_LIGHT_POINT, 0, 0, 1);
  bad.color.kind = 9;
  CHECK_THROWS_AS(build(bad), std::invalid_argument);
}

TEST_CASE("lamps/unequal ellipse raises one entry") {
  Fixture fx;
  ar_light_desc ell = desc(AR_LIGHT_AREA_ELLIPSE, 4, 1, 10);
  ar_light_desc spread = desc(AR_LIGHT_AREA_RECT, 1, 1, 10);
  spread.spread = 1.0f;  // radians
  ar_light_desc bb = desc(AR_LIGHT_POINT, 0, 0, 10);
  bb.color.kind = AR_SPECTRUM_BLACKBODY;
  bb.color.temperature = 3000.0f;
  bb.color.scale = 1.0f;  // a BLACKBODY lamp is exact, so it raises nothing
  ar_light_desc noshadow = desc(AR_LIGHT_POINT, 0, 0, 10);
  noshadow.cast_shadow = 0;
  ar_light_desc neg = desc(AR_LIGHT_POINT, 0, 0, 10);
  neg.color.rgb[0] = -0.5f;
  ar_light_desc mirrored = desc(AR_LIGHT_AREA_RECT, 1, 1, 10);
  mirrored.xf.m[10] = -1.0f;
  fx.commit([&](Edit& e) {
    REQUIRE(astroray::host::set_light(e, 10, &ell, fx.table()) == AR_OK);
    REQUIRE(astroray::host::set_light(e, 11, &spread, fx.table()) == AR_OK);
    REQUIRE(astroray::host::set_light(e, 12, &bb, fx.table()) == AR_OK);
    REQUIRE(astroray::host::set_light(e, 14, &noshadow, fx.table()) == AR_OK);
    REQUIRE(astroray::host::set_light(e, 15, &neg, fx.table()) == AR_OK);
    REQUIRE(astroray::host::set_light(e, 16, &mirrored, fx.table()) == AR_OK);
  });
  for (ar_key k : {10, 11, 14, 15, 16}) CHECK(fx.approximations(k) == 1);
  CHECK(fx.approximations(12) == 0);
  // The disk keeps the ellipse's area: diameter sqrt(4 * 1) = 2.
  auto l = fx.scene.lights();
  REQUIRE(l->lights.size() == 6);
  CHECK(l->lights[0].type == uint8_t(ar::LightType::AREA_DISK));
  CHECK(l->lights[0].size[0] == doctest::Approx(1.0));
  // An unrelated edit keeps the entry once; the corrected lamp clears it.
  fx.set_light(13, desc(AR_LIGHT_POINT, 0, 0, 1));
  CHECK(fx.approximations(10) == 1);
  fx.set_light(10, desc(AR_LIGHT_AREA_ELLIPSE, 4, 4, 10));
  CHECK(fx.approximations(10) == 0);
}

TEST_CASE("lamps/no shadow bit") {
  Fixture fx;
  fx.world_and_quad(1.0f);
  ar_light_desc a = desc(AR_LIGHT_AREA_RECT, 1, 1, 5);
  a.visibility |= AR_VISIBILITY_SHADOW | AR_VISIBILITY_VOLUME;  // a lamp asking for it still gets none
  a.xf.m[3] = 3.0f;
  ar_light_desc b = desc(AR_LIGHT_POINT, 0, 0, 5);
  b.visibility = AR_VISIBILITY_CAMERA;
  b.xf.m[3] = 7.0f;
  fx.commit([&](Edit& e) {
    REQUIRE(astroray::host::set_light(e, 20, &a, fx.table()) == AR_OK);
    REQUIRE(astroray::host::set_light(e, 21, &b, fx.table()) == AR_OK);
  });
  // Lights: the quad's two triangles, the world, then the lamps in key order.
  auto check = [&](const std::vector<float>& x) {
    auto lights = fx.scene.lights();
    auto t = fx.scene.instances_with_lamps(*lights);
    const size_t first = 1;  // the quad's instance
    REQUIRE(lights->lights.size() == 3 + x.size());
    REQUIRE(t->instances.size() == first + x.size());
    CHECK(lights->lamp_first == 3);
    for (size_t i = 0; i < x.size(); ++i) {
      const Instance& in = t->instances[first + i];
      CHECK((in.mask & ar::kShadowMaskBit) == 0);
      CHECK((in.visibility & AR_VISIBILITY_SHADOW) == 0);
      CHECK(in.geom_key == 0);
      REQUIRE(in.light_index == 2 + 1 + i);  // triangles + world + i
      CHECK(lights->lights[in.light_index].type != uint8_t(ar::LightType::TRIANGLE));
      CHECK(lights->lights[in.light_index].xf[3] == x[i]);
      CHECK(t->step(uint32_t(first + i), 0).m[3] == x[i]);
    }
    return t;
  };
  auto t = check({3.0f, 7.0f});
  CHECK((t->instances[1].mask & 0x0F) == 0x0F);
  CHECK(t->instances[2].mask == 1);
  // Removing the first lamp moves the second to the first lamp slot.
  fx.commit([&](Edit& e) { REQUIRE(e.remove(AR_KIND_LIGHT, 20) == AR_OK); });
  t = check({7.0f});
  CHECK(t->instances[1].mask == 1);
}

TEST_CASE("lamps/radiance scale power of two") {
  std::vector<double> peaks{0.3, 7.5, 100.0};
  double s = radiance_scale(peaks);
  int e = 0;
  CHECK(std::frexp(s, &e) == 0.5);  // an exact power of two
  CHECK(100.0 * s >= 1.0);
  CHECK(100.0 * s < 2.0);
  CHECK(radiance_scale({4.0}) == 0.25);   // exactly 2^k lands on 1
  CHECK(radiance_scale({1.0}) == 1.0);
  CHECK(radiance_scale({0.3}) == 4.0);    // 1.2
  CHECK(radiance_scale({0.0, 0.0}) == 1.0);
  CHECK_THROWS_AS(radiance_scale({-1.0}), std::invalid_argument);
  CHECK_THROWS_AS(radiance_scale({HUGE_VAL}), std::invalid_argument);
  // From real emitters: the largest peak times s is in [1, 2) however the powers are scaled, and a
  // lamp's peak is its radiance (the point's intensity P / 4 pi), not its power.
  Fixture fx;
  for (double power : {1e-3, 1.0, 5e4}) {
    std::vector<double> p;
    for (double f : {1.0, 0.25, 8.0}) {
      LightInput li;
      li.record = build_lamp(1, desc(AR_LIGHT_POINT, 0, 0, float(power * f)), fx.table())->record;
      p.push_back(emitter_peak(li, observer()));
    }
    const double scale = radiance_scale(p);
    int ex = 0;
    CHECK(std::frexp(scale, &ex) == 0.5);
    double largest = 0.0;
    for (double x : p) largest = std::max(largest, x * scale);
    CHECK(largest >= 1.0);
    CHECK(largest < 2.0);
  }
  LightInput rect, pt;
  rect.record = build_lamp(1, desc(AR_LIGHT_AREA_RECT, 2, 2, 40), fx.table())->record;
  pt.record = build_lamp(1, desc(AR_LIGHT_POINT, 0, 0, 40), fx.table())->record;
  CHECK(emitter_peak(pt, observer()) == doctest::Approx(emitter_peak(rect, observer())));  // P / 4 pi against P / (pi 4)
}

TEST_CASE("lamps/resolved value is independent of the radiance scale") {
  // One synthetic slot, as the shade stages would add it: the emitter's value times s. The film
  // multiplies by 1 / s, so every s resolves to the same number.
  const float value = 2.0f;
  double out[2] = {};
  const double scales[2] = {1.0, 8.0};
  for (int k = 0; k < 2; ++k) {
    FilmRig rig;
    rig.film->data().inv_radiance_scale = 1.0 / scales[k];
    out[k] = rig.resolve_one(float(value * scales[k]));
  }
  CHECK(out[0] == 2.0);
  CHECK(out[1] == out[0]);
  // The importances and the pmf do not move with s: the same lights at s = 1 and at 4.
  auto inputs = [&](float s) {
    std::vector<LightInput> v;
    Fixture fx;
    for (auto d : {desc(AR_LIGHT_POINT, 0, 0, 3), desc(AR_LIGHT_AREA_RECT, 1, 2, 10)}) {
      LightInput li;
      li.record = build_lamp(1, d, fx.table())->record;
      li.record.strength *= s;
      v.push_back(li);
    }
    LightInput tri;
    const ar::SpectrumRef white = ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1));
    tri.record = ar::light_make_triangle(ar::Real3{0, 0, 0}, ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0}, 0, 0, 0, Real(2.0f * s), white);
    tri.terms = {{white, 2.0f * s}};
    v.push_back(tri);
    LightInput w;
    w.record = ar::light_make_world(Real(0.5f * s), white);
    v.push_back(w);
    return v;
  };
  ar::SceneSphere sphere{ar::Real3{0, 0, 0}, Real(5)};
  const auto a = build_light_table(inputs(1.0f), sphere, observer(), nullptr);
  const auto b = build_light_table(inputs(4.0f), sphere, observer(), nullptr);
  REQUIRE(a->pmf.size() == b->pmf.size());
  for (size_t i = 0; i < a->pmf.size(); ++i) {
    CHECK(a->pmf[i] == b->pmf[i]);
    CHECK(b->importance[i] == 4.0 * a->importance[i]);
  }
}

TEST_CASE("lamps/scene lights and the scene sphere") {
  Fixture fx;
  // A lit constant world and two lamps, no meshes.
  const GB g(2.0f);
  fx.commit([&](Edit& e) {
    const ar_graph_desc d = g.desc();
    ar_world_desc w = {};
    w.surface_graph = 40;
    REQUIRE(fx.scene.materials->set_graph(e, 40, &d) == AR_OK);
    REQUIRE(fx.scene.materials->set_world(e, &w) == AR_OK);
  });
  ar_light_desc rect = desc(AR_LIGHT_AREA_RECT, 2, 2, 10);
  ar_light_desc far = desc(AR_LIGHT_POINT, 0.5f, 0, 10);
  far.xf.m[3] = 10.0f;
  fx.commit([&](Edit& e) {
    REQUIRE(astroray::host::set_light(e, 30, &rect, fx.table()) == AR_OK);
    REQUIRE(astroray::host::set_light(e, 31, &far, fx.table()) == AR_OK);
  });
  auto t = fx.scene.lights();
  REQUIRE(t->lights.size() == 3);  // the world, then the two lamps
  CHECK(t->lights[0].type == uint8_t(ar::LightType::WORLD_CONST));
  // The sphere holds both lamps (x from -sqrt2/2.. to 10.5), so the world has positive power.
  CHECK(double(t->scene.radius) >= 5.0);
  CHECK(t->pmf[0] > 0.0f);
  CHECK(fx.scene.light_rebuilds() == 1);
  // An unrelated edit does not rebuild; adding, changing and removing a lamp each do.
  fx.commit([&](Edit& e) {
    ar_material_desc m = {};
    m.surface_graph = 40;
    REQUIRE(fx.scene.materials->set_material(e, 50, &m) == AR_OK);
  });
  CHECK(fx.scene.lights() == t);
  CHECK(fx.scene.light_rebuilds() == 1);
  fx.set_light(32, desc(AR_LIGHT_POINT, 0, 0, 1));
  t = fx.scene.lights();
  CHECK(fx.scene.light_rebuilds() == 2);
  CHECK(t->lights.size() == 4);
  fx.set_light(32, desc(AR_LIGHT_POINT, 0, 0, 2));
  t = fx.scene.lights();
  CHECK(fx.scene.light_rebuilds() == 3);
  CHECK(t->lights.size() == 4);
  fx.commit([&](Edit& e) { REQUIRE(e.remove(AR_KIND_LIGHT, 32) == AR_OK); });
  t = fx.scene.lights();
  CHECK(fx.scene.light_rebuilds() == 4);
  CHECK(t->lights.size() == 3);
  // A reset drops every lamp.
  fx.commit([&](Edit& e) { REQUIRE(e.reset() == AR_OK); });
  t = fx.scene.lights();
  CHECK(fx.scene.light_rebuilds() == 5);
  CHECK(t->lights.size() == 1);  // the world stays: only note_reset (the C entry point) clears it
}

TEST_CASE("lamps/scene pmf does not depend on the emission scale") {
  // The same scene with every emitter 4 times as bright: s falls by 4 and the pmf is unchanged,
  // so s reached the estimate terms as well as the records.
  std::vector<float> pmf[2];
  double s[2] = {};
  for (int i = 0; i < 2; ++i) {
    const float k = i == 0 ? 1.0f : 4.0f;
    Fixture fx;
    fx.world_and_quad(k);
    fx.set_light(20, desc(AR_LIGHT_AREA_RECT, 1, 1, 10.0f * k));
    fx.set_light(21, desc(AR_LIGHT_POINT, 0, 0, 5.0f * k));
    auto t = fx.scene.lights();
    REQUIRE(t->lights.size() == 5);
    pmf[i] = t->pmf;
    s[i] = t->radiance_scale;
  }
  CHECK(s[0] == 4.0 * s[1]);
  REQUIRE(pmf[0].size() == pmf[1].size());
  for (size_t i = 0; i < pmf[0].size(); ++i) CHECK(pmf[0][i] == pmf[1][i]);
}

TEST_CASE("lamps/a commit that changes s advances the epoch and the film scale") {
  Fixture fx;
  fx.set_light(1, desc(AR_LIGHT_POINT, 0, 0, 1.0f));
  Ledger ledger;
  ThreadPool pool(fx.ctx, 2);
  CpuDevice dev(ledger, pool);
  dev.register_production_stages();
  SessionOptions o;
  o.max_width = o.max_height = 4;
  Session s(fx.scene, dev, o);
  auto inv_scale = [&] {
    double v = 0.0;
    s.read_film([&](const FilmReadContext& c) { v = c.film.data().inv_radiance_scale; });
    return v;
  };
  auto settled = [&](uint64_t above) {
    for (int i = 0; i < 20000; ++i) {
      if (s.latch().epoch() > above && s.progress().state == AR_STATE_CONVERGED) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  s.start();
  REQUIRE(s.wait(20000));
  REQUIRE(settled(0));
  const uint64_t epoch = s.latch().epoch();
  const double s0 = fx.scene.lights()->radiance_scale;
  CHECK(inv_scale() == 1.0 / s0);
  fx.set_light(2, desc(AR_LIGHT_POINT, 0, 0, 4000.0f));  // a far brighter emitter moves s
  const double s1 = fx.scene.lights()->radiance_scale;
  REQUIRE(s1 != s0);
  REQUIRE(settled(epoch));
  CHECK(s.latch().epoch() > epoch);
  CHECK(inv_scale() == 1.0 / s1);
  s.close();
  for (int i = 0; i < 20000 && s.progress().state != AR_STATE_CLOSED; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

}  // TEST_SUITE("fast")

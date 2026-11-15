// SPDX-License-Identifier: Apache-2.0
// Lamp ingestion (task 068): records from descriptors, approximation entries, lamp instances,
// the radiance scale and the scene's lamp input to the light table.
// Ref: 0004 D6 (conventions), D8 (lamps never block shadow rays); 0003 d10 (radiance scale);
// Cycles intern/cycles/scene/light.cpp (normalize), cited in host/light/lamps.h.
#include <doctest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "host/data_resource.h"
#include "host/light/lamps.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "vm/ops.h"

using namespace astroray::host;
using ar::Real;

namespace {

constexpr double kPi = 3.14159265358979323846;

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
};

ar_light_desc desc(uint32_t type, float sx, float sy, float power) {
  ar_light_desc d = {};
  d.color.kind = AR_SPECTRUM_RGB;
  d.color.rgb[0] = d.color.rgb[1] = d.color.rgb[2] = 1.0f;
  d.xf.m[0] = d.xf.m[5] = d.xf.m[10] = 1.0f;
  d.type = type;
  d.visibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE | AR_VISIBILITY_GLOSSY | AR_VISIBILITY_TRANSMISSION;
  d.size_x = sx, d.size_y = sy;
  d.spread = 180.0f;
  d.power = power;
  d.normalize = 1;
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
  auto s = build(desc(AR_LIGHT_POINT, 0.5f, 0, 100));
  CHECK(s->record.type == uint8_t(ar::LightType::SPHERE));
  CHECK(s->record.size[0] == 0.5f);
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
  // An object scale in the transform scales an area lamp's extents but not a point's radius.
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
}

TEST_CASE("lamps/unequal ellipse raises one entry") {
  Fixture fx;
  ar_light_desc ell = desc(AR_LIGHT_AREA_ELLIPSE, 4, 1, 10);
  ar_light_desc spread = desc(AR_LIGHT_AREA_RECT, 1, 1, 10);
  spread.spread = 90.0f;
  ar_light_desc bb = desc(AR_LIGHT_POINT, 0, 0, 10);
  bb.color.kind = AR_SPECTRUM_BLACKBODY;
  bb.color.temperature = 3000.0f;
  fx.commit([&](Edit& e) {
    REQUIRE(astroray::host::set_light(e, 10, &ell, fx.table()) == AR_OK);
    REQUIRE(astroray::host::set_light(e, 11, &spread, fx.table()) == AR_OK);
    REQUIRE(astroray::host::set_light(e, 12, &bb, fx.table()) == AR_OK);
  });
  CHECK(fx.approximations(10) == 1);
  CHECK(fx.approximations(11) == 1);
  CHECK(fx.approximations(12) == 1);
  // The disk keeps the ellipse's area: diameter sqrt(4 * 1) = 2.
  auto l = fx.scene.lights();
  REQUIRE(l->lights.size() == 3);
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
  ar_light_desc a = desc(AR_LIGHT_AREA_RECT, 1, 1, 5);
  a.visibility |= AR_VISIBILITY_SHADOW | AR_VISIBILITY_VOLUME;  // a lamp asking for it still gets none
  ar_light_desc b = desc(AR_LIGHT_POINT, 0, 0, 5);
  b.visibility = AR_VISIBILITY_CAMERA;
  b.xf.m[3] = 7.0f;
  fx.commit([&](Edit& e) {
    REQUIRE(astroray::host::set_light(e, 20, &a, fx.table()) == AR_OK);
    REQUIRE(astroray::host::set_light(e, 21, &b, fx.table()) == AR_OK);
  });
  auto t = fx.scene.instances_with_lamps();
  REQUIRE(t);
  REQUIRE(t->instances.size() == 2);
  auto lights = fx.scene.lights();
  for (size_t i = 0; i < 2; ++i) {
    const Instance& in = t->instances[i];
    CHECK((in.mask & ar::kShadowMaskBit) == 0);
    CHECK((in.visibility & AR_VISIBILITY_SHADOW) == 0);
    REQUIRE(in.light_index < lights->lights.size());
    CHECK(lights->lights[in.light_index].xf[3] == t->step(uint32_t(i), 0).m[3]);
  }
  CHECK(t->instances[0].light_index != t->instances[1].light_index);
  CHECK((t->instances[0].mask & 0x0F) == 0x0F);
  CHECK(t->instances[1].mask == 1);
  CHECK(t->instances[0].geom_key == 0);
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
  // From real emitters: the largest lamp peak times s is in [1, 2) however the powers are scaled.
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
}

TEST_CASE("lamps/scene lights and the scene sphere") {
  Fixture fx;
  // A lit constant world and two lamps, no meshes.
  ar_graph_node nodes[3] = {};
  const float f[4] = {1.0f, 1.0f, 1.0f, 2.0f};
  nodes[0].op = uint32_t(ar::Op::CONST_F32X3), nodes[0].f32_first = 0, nodes[0].f32_count = 3;
  nodes[1].op = uint32_t(ar::Op::CONST_F32), nodes[1].f32_first = 3, nodes[1].f32_count = 1;
  nodes[2].op = uint32_t(ar::Op::EMIT_LE);
  const ar_graph_link links[2] = {{0, 0, 2, 0}, {1, 0, 2, 1}};
  ar_graph_desc g = {};
  g.node_count = 3, g.link_count = 2, g.f32_count = 4;
  g.nodes = nodes, g.links = links, g.params_f32 = f;
  fx.commit([&](Edit& e) {
    ar_world_desc w = {};
    w.surface_graph = 40;
    REQUIRE(fx.scene.materials->set_graph(e, 40, &g) == AR_OK);
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

}  // TEST_SUITE("fast")

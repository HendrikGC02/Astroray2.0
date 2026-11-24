// SPDX-License-Identifier: Apache-2.0
// A BLACKBODY lamp colour: the record carries {T, scale x 1 / Y(Planck(T))}, its display Y is the
// scale, bad temperatures and scales raise, a temperature whose Planck radiance underflows keeps T
// and raises one entry, and a 1e9 K lamp beside a 3 K lamp renders with no invalid sample and the
// same film at the automatic radiance scale as at s = 1 (0003 check 15, with Planck emitters).
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d10, d11, d25; 0004 D6, D10;
// 0007-appendix-abi.md ar_spectrum_ref.
#include <doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/film/film_view.h"
#include "host/light/lamps.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/session/readback.h"
#include "host/session/session.h"
#include "host/spectral/blackbody.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr ar_key kMesh = 100, kMaterial = 1, kGraph = 1001, kHot = 20, kCold = 21;
constexpr uint32_t kW = 16, kH = 16, kSpp = 4;

ar_light_desc blackbody(float kelvin, float scale, float power = 10.0f) {
  ar_light_desc d = {};
  d.color.kind = AR_SPECTRUM_BLACKBODY;
  d.color.temperature = kelvin;
  d.color.scale = scale;
  d.color.rgb[0] = 0.2f, d.color.rgb[1] = 0.4f, d.color.rgb[2] = 0.9f;  // ignored for this kind
  d.xf.m[0] = d.xf.m[5] = d.xf.m[10] = 1.0f;
  d.type = AR_LIGHT_POINT;
  d.visibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE | AR_VISIBILITY_GLOSSY | AR_VISIBILITY_TRANSMISSION;
  d.power = power;
  d.normalize = 1;
  d.cast_shadow = 1;
  return d;
}

const ObserverTables& observer_tables() {
  static ObserverTables t = [] {
    DataBlob b;
    REQUIRE(find_data("spectral/observer.bin", &b));
    ObserverTables out;
    REQUIRE(parse_observer(b.data, b.size, &out).empty());
    return out;
  }();
  return t;
}

// sum_i S(lambda_i) ybar_i / Y_n over the observer's 1 nm grid: the display Y of a spectrum (0003 d11),
// evaluated through the kernel's resolve exactly as the film's emitters are.
double display_y(const ar::SpectrumRef& r) {
  const ObserverTables& t = observer_tables();
  const ar::ObserverView obs{&t.cmf[0][0], t.d65};
  double y = 0.0;
  for (uint32_t i = 0; i < kObserverSamples; ++i) {
    ar::Lambda lam;
    for (int k = 0; k < ar::kLanes; ++k) lam.nm[k] = ar::Real(double(kObserverLambdaMin) + double(i));
    y += double(ar::resolve(r, lam, obs, nullptr).v[0]) * double(t.cmf[1][i]);
  }
  return y / t.y_n;
}

struct Fixture {
  ContextState ctx;
  Scene scene{ctx, SceneConfig{2, {}, {}}};
  uint64_t generation = 0;
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
    REQUIRE(e->commit(&generation) == AR_OK);
    scene.materials->end_edit();
    e.reset();
    scene.generations().wait_idle();
    REQUIRE(scene.generations().failed() == 0);
  }
  void set_light(ar_key key, const ar_light_desc& d) {
    commit([&](Edit& e) { REQUIRE(astroray::host::set_light(e, key, &d, table()) == AR_OK); });
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
  void plane(Edit& e) {  // a wide quad below the lamps facing them, the default OpenPBR material
    const float h = 20.0f;
    const std::vector<float> pos{-h, -h, -2.0f, h, -h, -2.0f, h, h, -2.0f, -h, h, -2.0f};
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
    ar_graph_desc g = {};
    REQUIRE(scene.materials->set_graph(e, kGraph, &g) == AR_OK);
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
  const float p[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0, -1.0f, 0, 0, 0, 1.0f};
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

// The radiance scale reads ASTRORAY_TEST_RADIANCE_SCALE when the light table is built, so the guard
// outlives the scene's commits.
struct EnvGuard {
  const char* name;
  EnvGuard(const char* n, bool on) : name(n) { REQUIRE(_putenv_s(name, on ? "1" : "") == 0); }
  ~EnvGuard() { _putenv_s(name, ""); }
};

struct Render {
  std::vector<std::vector<float>> planes;  // every pass's mean
  std::vector<uint64_t> invalid;           // the counts of the INVALID_SAMPLE entries
  std::vector<std::string> approximated;   // the details of the NODE_APPROXIMATED entries
  std::vector<uint64_t> approximated_subject;
  double scale = 0.0;                      // the light table's radiance scale s
};

// A hot and a cold lamp over a plane, one CPU thread so the film's fp32 adds run in one order (0002 D12).
Render render(bool unit_scale) {
  EnvGuard env("ASTRORAY_TEST_RADIANCE_SCALE", unit_scale);
  Fixture fx;
  fx.commit([&](Edit& e) {
    fx.plane(e);
    fx.settings(e);
  });
  fx.commit([&](Edit& e) { fx.instance(e); });
  ar_light_desc hot = blackbody(1.0e9f, 1.0f, 40.0f), cold = blackbody(3.0f, 1.0f, 40.0f);
  hot.xf.m[3] = 0.3f, hot.xf.m[7] = 0.2f, hot.xf.m[11] = 0.5f;
  cold.xf.m[3] = -0.4f, cold.xf.m[7] = -0.1f, cold.xf.m[11] = 0.6f;
  fx.set_light(kHot, hot);
  fx.set_light(kCold, cold);
  Render out;
  out.scale = fx.scene.lights()->radiance_scale;
  std::shared_ptr<Device> dev = make_cpu_session_device(fx.ctx, 1);
  SessionOptions o;
  o.max_width = kW;
  o.max_height = kH;
  Session s(fx.scene, *dev, o);
  s.set_view(view_of(kW, kH));
  s.start();
  REQUIRE(until([&] {
    const SessionProgress p = s.progress();
    return p.state == AR_STATE_CONVERGED && p.active_generation == fx.generation;
  }));
  REQUIRE(s.failure().empty());
  const std::vector<PassDesc> passes = list_passes(s.layout());
  REQUIRE(!passes.empty());
  s.read_film([&](const FilmReadContext& c) {
    FilmView view(c.device, c.film);
    for (const PassDesc& p : passes) {
      std::vector<float> m(pass_float_count(p, AR_QUANTITY_MEAN, view.width(), view.height()));
      read_pass(view, p, AR_QUANTITY_MEAN, uint32_t(m.size()), m.data());
      out.planes.push_back(std::move(m));
    }
  });
  const ar_report_info info = s.report_snapshot();
  for (uint32_t i = 0; i < info.entry_count; ++i) {
    ar_report_entry e{};
    s.report_entry(i, &e);
    if (e.code == AR_REPORT_INVALID_SAMPLE) out.invalid.push_back(e.count);
    if (e.code == AR_REPORT_NODE_APPROXIMATED) {
      out.approximated.push_back(reinterpret_cast<const char*>(e.detail));
      out.approximated_subject.push_back(e.subject);
    }
  }
  s.close();
  until([&] { return s.progress().state == AR_STATE_CLOSED; });
  return out;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("lamp_blackbody/record carries the ref") {
  Fixture fx;
  for (double kelvin : {1000.0, 3000.0, 6500.0, 20000.0}) {
    INFO("T = " << kelvin);
    const ar_light_desc d = blackbody(float(kelvin), 2.5f);
    const auto lamp = build_lamp(1, d, fx.table());
    // {BLACKBODY, ILLUMINANT, p[0] = T, p[1] = scale x 1 / Y(Planck(T))} (0003 d11, 0005 item 9), built
    // here by hand so the lamp and the compiler cannot agree on a wrong ref.
    ar::SpectrumRef want = {};
    want.kind = uint8_t(ar::SpectrumKind::BLACKBODY);
    want.semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
    want.p[0] = float(kelvin);
    want.p[1] = float(2.5 * blackbody_y_normalised(kelvin));
    CHECK(std::memcmp(&lamp->record.emission, &want, sizeof want) == 0);
    CHECK(lamp->record.type == uint8_t(ar::LightType::POINT));
    CHECK(lamp->record.strength == 10.0f);
    CHECK(lamp->approximations.empty());
    // The rgb field is ignored, not blended in.
    ar_light_desc other = d;
    other.color.rgb[0] = 1.0f, other.color.rgb[1] = other.color.rgb[2] = 0.0f;
    CHECK(std::memcmp(&build_lamp(1, other, fx.table())->record.emission, &want, sizeof want) == 0);
  }
  // A sphere lamp takes a BLACKBODY colour too, and a record of another kind is still refused.
  ar_light_desc sphere = blackbody(6500.0f, 1.0f);
  sphere.size_x = 0.5f;
  CHECK(build_lamp(1, sphere, fx.table())->record.type == uint8_t(ar::LightType::SPHERE));
  ar::SpectrumRef table_ref = {};
  table_ref.kind = uint8_t(ar::SpectrumKind::TABLE);
  table_ref.semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
  CHECK(ar::light_problem(ar::light_make(ar::LightType::POINT, ar::Real3{0, 0, 0}, ar::Real(0), ar::Real(1), table_ref)) != nullptr);
}

TEST_CASE("lamp_blackbody/display y equals scale") {
  Fixture fx;
  for (float kelvin : {1000.0f, 6500.0f, 20000.0f}) {
    INFO("T = " << kelvin);
    const auto lamp = build_lamp(1, blackbody(kelvin, 3.0f), fx.table());
    // Y(scale x Planck(T) / Y(Planck(T))) = scale, 0003 check 11's figure for the Blackbody node.
    CHECK(display_y(lamp->record.emission) == doctest::Approx(3.0).epsilon(1e-3));
  }
}

TEST_CASE("lamp_blackbody/validation errors") {
  Fixture fx;
  auto build = [&](float kelvin, float scale) { return build_lamp(1, blackbody(kelvin, scale), fx.table()); };
  CHECK_NOTHROW(build(3.0f, 1.0f));
  CHECK_NOTHROW(build(6500.0f, 0.0f));  // a dark lamp is a lamp
  CHECK_NOTHROW(build(1.0e9f, 1.0f));
  for (float bad : {0.0f, -300.0f, std::nanf(""), HUGE_VALF, -HUGE_VALF}) {
    INFO("T = " << bad);
    CHECK_THROWS_AS(build(bad, 1.0f), std::invalid_argument);  // never the no-visible-radiance path
  }
  CHECK_THROWS_AS(build(1.5e9f, 1.0f), std::invalid_argument);  // above planck.h's float domain
  for (float bad : {-1.0f, std::nanf(""), HUGE_VALF}) {
    INFO("scale = " << bad);
    CHECK_THROWS_AS(build(6500.0f, bad), std::invalid_argument);
  }
  // 1 / Y(Planck(T)) is a double at 100 K but not a float: refused, naming the colour.
  try {
    build(100.0f, 1.0f);
    FAIL("a 100 K lamp stores 1 / Y that overflows a float");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()).find("light color") != std::string::npos);
  }
  CHECK_THROWS_AS(build(6500.0f, 3.4e38f), std::invalid_argument);
}

TEST_CASE("lamp_blackbody/no visible radiance raises one entry") {
  Fixture fx;
  ar_light_desc d = blackbody(3.0f, 2.0f, 7.0f);
  d.xf.m[3] = 1.5f;
  d.size_x = 0.25f;
  d.visibility = AR_VISIBILITY_CAMERA;
  // Planck(3 K) underflows double over 360-830 nm, so there is no Y to normalise by.
  CHECK(blackbody_visible_y(3.0) == 0.0);
  CHECK_THROWS_AS(blackbody_y_normalised(3.0), std::invalid_argument);
  const auto lamp = build_lamp(kCold, d, fx.table());
  REQUIRE(lamp->approximations.size() == 1);
  const std::string& a = lamp->approximations[0];
  CHECK(a.find("light 21") != std::string::npos);
  CHECK(a.find("3 K") != std::string::npos);
  CHECK(a.find("no visible radiance, not normalised") != std::string::npos);
  // T and the scale are kept unnormalised and every other field is the lamp's own.
  CHECK(lamp->record.emission.kind == uint8_t(ar::SpectrumKind::BLACKBODY));
  CHECK(lamp->record.emission.p[0] == 3.0f);
  CHECK(lamp->record.emission.p[1] == 2.0f);
  CHECK(lamp->record.type == uint8_t(ar::LightType::SPHERE));
  CHECK(lamp->record.size[0] == 0.25f);
  CHECK(lamp->record.xf[3] == 1.5f);
  CHECK(lamp->record.strength == 7.0f);  // normalised: the power as given
  CHECK(lamp->visibility == AR_VISIBILITY_CAMERA);
  // A hot lamp raises nothing.
  CHECK(build_lamp(kHot, blackbody(1.0e9f, 1.0f), fx.table())->approximations.empty());
}

TEST_CASE("lamp_blackbody/extreme temperatures one render") {
  const Render a = render(false);
  // Zero INVALID samples and a finite, lit film with a 1e9 K and a 3 K lamp in one scene.
  CHECK(a.invalid.empty());
  double sum = 0.0;
  for (const std::vector<float>& plane : a.planes)
    for (float x : plane) CHECK(std::isfinite(x));
  REQUIRE(!a.planes.empty());
  for (float x : a.planes.front()) sum += x;
  CHECK(sum > 0.0);
  // The 3 K lamp is the one lamp with an entry.
  REQUIRE(a.approximated.size() == 1);
  CHECK(a.approximated_subject[0] == kCold);
  CHECK(a.approximated[0].find("no visible radiance, not normalised") != std::string::npos);
  // s = 2^k from the hot lamp's Planck peak, a power of two, not 1 (else the comparison below is trivial).
  int e = 0;
  CHECK(std::frexp(a.scale, &e) == 0.5);
  CHECK(a.scale != 1.0);
#if defined(AR_GATE_HOOKS)
  const Render one = render(true);
  CHECK(one.scale == 1.0);
  REQUIRE(a.planes.size() == one.planes.size());
  for (size_t i = 0; i < a.planes.size(); ++i) {
    REQUIRE(a.planes[i].size() == one.planes[i].size());
    CHECK(std::memcmp(a.planes[i].data(), one.planes[i].data(), a.planes[i].size() * 4) == 0);
  }
#endif
}

}  // TEST_SUITE("fast")

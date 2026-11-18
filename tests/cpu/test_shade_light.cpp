// SPDX-License-Identifier: Apache-2.0
// shade_light (0004 D1, D3, D8): hand-built lamp crossings. A camera path that meets a lamp's
// emitting face adds Le beta times the emitter-hit MIS weight to its sample's slot, once; the
// non-emitting side (a sphere's inside on the way out, a rectangle's back) and a light path add
// nothing. Every crossing re-queues INTERSECT_CLOSEST from just past the lamp in the same
// direction with bounces, event_index, flags and throughput unchanged, and prev_dist grows so the
// previous vertex rebuilt from the new origin is the same point (0003 d19 row 17).
// Expected values are computed here in double from the formulas of 0004 D6 (L = P / (4 pi^2 r^2),
// L = P / (pi A)) and pbrt-v4's cone pdf 1 / (2 pi (1 - cos theta_max)), not from the kernel.
// Ref: docs/decisions/0004-integrator.md D1, D3, D6, D8; 0003 d19; integrator/mis.h.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/film/film_layout.h"
#include "host/thread_pool.h"
#include "kernel/kernel_globals.h"
#include "kernel/light/light_sampler.h"
#include "kernel/material/slot_resolve.h"
#include "kernel/stages/shade_light.h"
#include "kernel_cpu/stage_cpu.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kSlots = 6;
constexpr double kPi = 3.14159265358979323846;
constexpr float kSphereP = 30.0f, kSphereR = 0.5f, kRectP = 12.0f;
constexpr uint16_t kEvent = 9;

// Lights [decoy point, sphere at the origin (radius 0.5), rectangle 1 x 1 at z = 5 facing -z].
// Instance 0 is the sphere's primitive, instance 1 the rectangle's. Colours are a constant
// REFLECTANCE-semantic 1, so Le is the geometric factor times P whatever the observer tables.
// Paths (ray_d unit unless stated):
//   0 camera, first vertex: (0, 0, -3) toward +z meets the sphere's outside at t = 2.5
//   1 camera, after a diffuse bounce at (0, 0, -3): origin (0, 0, -2.5), prev_dist 0.5, t = 2
//   2 camera, inside the sphere: from the centre toward +z, t = 0.5 (leaving: no Le)
//   3 light path, the same crossing as 0 (passes through, no contribution)
//   4 camera, after a delta bounce: (0.2, 0.1, 3) toward +z, ray_d of length 2, meets the
//     rectangle's front at t = 1
//   5 camera, first vertex: (0.1, -0.2, 8) toward -z meets the rectangle's back at t = 3
struct LampRig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::unique_ptr<Film> film;
  ar::KernelGlobals kg{};
  ar::PathStateSoA path{};
  ar::SceneTables tables{};
  ar::SceneData scene{};
  std::vector<ar::DeviceInstanceRecord> instances = std::vector<ar::DeviceInstanceRecord>(2);
  std::vector<ar::LightRecord> lights;
  std::vector<float> pmf{0.2f, 0.5f, 0.3f};
  ar::LightSamplerView sampler{};
  std::vector<uint32_t> queue{0, 1, 2, 3, 4, 5}, active{0, 1, 2, 3, 4, 5};
  std::vector<uint8_t> next_stage = std::vector<uint8_t>(kSlots, uint8_t(ar::Stage::SHADE_LIGHT));
  std::vector<uint8_t> term = std::vector<uint8_t>(kSlots, uint8_t(ar::Term::ACTIVE));
  std::vector<uint32_t> flags = std::vector<uint32_t>(kSlots, 0u);
  std::vector<uint32_t> sample_id{0, 1, 2, 3, 4, 5};
  std::vector<ar::U16x6> bounces = std::vector<ar::U16x6>(kSlots, ar::U16x6{{0, 0, 0, 0, 0, 0}});
  std::vector<uint16_t> event_index = std::vector<uint16_t>(kSlots, kEvent);
  std::vector<ar::Real3> ray_o = std::vector<ar::Real3>(kSlots), ray_d = std::vector<ar::Real3>(kSlots);
  std::vector<ar::Spec> lambda_t = std::vector<ar::Spec>(kSlots), beta = std::vector<ar::Spec>(kSlots);
  std::vector<ar::Spec> r_u = std::vector<ar::Spec>(kSlots), r_l = std::vector<ar::Spec>(kSlots);
  std::vector<uint32_t> prev_ng = std::vector<uint32_t>(kSlots), prev_ns = std::vector<uint32_t>(kSlots);
  std::vector<ar::Real> prev_dist = std::vector<ar::Real>(kSlots, 0.0f), hit_t = std::vector<ar::Real>(kSlots);
  std::vector<uint32_t> hit_inst{0, 0, 0, 0, 1, 1};
  ar::StepEntry step{0, kSlots, 0};

  LampRig() : pool(ctx, 1), dev(ledger, pool) {
    FilmRequest req;
    req.max_width = kSlots;
    req.max_height = 1;
    req.slot_capacity = kSlots;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    const ar::SpectrumRef c = ar::make_const_ref(ar::SpectrumSemantic::REFLECTANCE, ar::Real(1));
    lights = {ar::light_make(ar::LightType::POINT, ar::Real3{0, 0, 0}, ar::Real(0), ar::Real(100), c),
              ar::light_make(ar::LightType::SPHERE, ar::Real3{0, 0, 0}, ar::Real(kSphereR), ar::Real(kSphereP), c),
              ar::light_make_area(ar::LightType::AREA_RECT, ar::Real3{0, 0, 5}, ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0},
                                  ar::Real(1), ar::Real(1), ar::Real(kRectP), c)};
    sampler.light_count = uint32_t(lights.size());
    sampler.pmf = pmf.data();
    for (auto& r : instances) r = {};
    instances[0].light_index = 1;
    instances[1].light_index = 2;
    scene.instances = instances.data();
    scene.instance_count = uint32_t(instances.size());

    ray_o = {{0, 0, -3}, {0, 0, -2.5f}, {0, 0, 0}, {0, 0, -3}, {0.2f, 0.1f, 3}, {0.1f, -0.2f, 8}};
    ray_d = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 2}, {0, 0, -1}};
    hit_t = {2.5f, 2.0f, 0.5f, 2.5f, 1.0f, 3.0f};
    for (uint32_t i = 0; i < kSlots; ++i) {
      for (int l = 0; l < ar::kLanes; ++l) {
        lambda_t[i].v[l] = 450.0f + 30.0f * float(l);
        beta[i].v[l] = (0.25f + 0.5f * float(i)) * (1.0f + 0.25f * float(l));  // a different beta per lane
        r_u[i].v[l] = 1.0f + 0.125f * float(l);
        r_l[i].v[l] = 0.5f + 0.25f * float(l);
      }
      flags[i] = ar::set_hero_lane(0u, i & 3u);
      prev_ng[i] = prev_ns[i] = ar::oct_encode(ar::Real3{0, 0, 1});
    }
    bounces[1] = ar::U16x6{{2, 1, 1, 0, 0, 0}};
    prev_dist[1] = 0.5f;  // the previous vertex is (0, 0, -3)
    bounces[3] = ar::U16x6{{1, 1, 0, 0, 0, 0}};
    flags[3] |= ar::PATH_KIND;
    bounces[4] = ar::U16x6{{1, 0, 1, 0, 0, 0}};
    flags[4] |= ar::LAST_BOUNCE_SPECULAR;

    path.sample_id = sample_id.data();
    path.next_stage = next_stage.data();
    path.term = term.data();
    path.flags = flags.data();
    path.bounces = bounces.data();
    path.event_index = event_index.data();
    path.ray_o = ray_o.data();
    path.ray_d = ray_d.data();
    path.lambda_t = lambda_t.data();
    path.beta = beta.data();
    path.r_u = r_u.data();
    path.r_l = r_l.data();
    path.prev_ng = prev_ng.data();
    path.prev_ns = prev_ns.data();
    path.prev_dist = prev_dist.data();
    path.hit_t = hit_t.data();
    path.hit_inst = hit_inst.data();
    kg.path = &path;
    kg.main_queue = queue.data();
    kg.active_pixels = active.data();
    kg.film = &film->data();
    kg.tables = &tables;
    kg.scene = &scene;
    kg.lights = lights.data();
    kg.light_sampler = &sampler;
    kg.session.width = kSlots;
    kg.session.height = 1;
    kg.session.samples_per_pass = 1;
  }

  void run() { cpu_stage_shade_light(kg, step, 0, kSlots); }
  ar::Spec slot_value(uint32_t slot) const {
    ar::Spec s;
    for (int l = 0; l < ar::kLanes; ++l) s.v[l] = film->data().slot_L[4u * slot + uint32_t(l)];
    return s;
  }
};

double avg4(const double v[4]) { return (v[0] + v[1] + v[2] + v[3]) / 4.0; }

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("shade_light/camera crossing adds the weighted Le once") {
  LampRig rig;
  const std::vector<ar::Spec> beta = rig.beta, r_u = rig.r_u, r_l = rig.r_l;
  rig.run();
  const double l_sphere = double(kSphereP) / (4.0 * kPi * kPi * double(kSphereR) * double(kSphereR));
  const double l_rect = double(kRectP) / (kPi * 1.0);
  // MIS denominators (integrator/mis.h): avg(r_u) unweighted; avg(r_u + r_l p_light) after a
  // non-delta bounce, p_light = pmf(sphere) / (2 pi (1 - cos theta_max)) seen from (0, 0, -3).
  double ru[4], w1[4];
  for (int l = 0; l < 4; ++l) ru[l] = double(r_u[0].v[l]);
  const double sin2 = double(kSphereR) * double(kSphereR) / 9.0;
  const double p_light = double(rig.pmf[1]) / (2.0 * kPi * (1.0 - std::sqrt(1.0 - sin2)));
  for (int l = 0; l < 4; ++l) w1[l] = double(r_u[1].v[l]) + double(r_l[1].v[l]) * p_light;
  const double expect_scale[kSlots] = {l_sphere / avg4(ru), l_sphere / avg4(w1), 0.0, 0.0, l_rect / avg4(ru), 0.0};
  for (uint32_t s = 0; s < kSlots; ++s) {
    CAPTURE(s);
    const ar::Spec got = rig.slot_value(s);
    for (int l = 0; l < ar::kLanes; ++l) {
      const double want = double(beta[s].v[l]) * expect_scale[s];
      if (want == 0.0) CHECK(got.v[l] == 0.0f);
      else CHECK(double(got.v[l]) == doctest::Approx(want).epsilon(2e-6));
    }
  }
}

TEST_CASE("shade_light/requeue keeps event index") {
  LampRig rig;
  const LampRig before;  // an untouched copy of every input
  rig.run();
  for (uint32_t s = 0; s < kSlots; ++s) {
    CAPTURE(s);
    CHECK(rig.next_stage[s] == uint8_t(ar::Stage::INTERSECT_CLOSEST));
    CHECK(rig.term[s] == uint8_t(ar::Term::ACTIVE));
    CHECK(rig.event_index[s] == kEvent);
    for (int i = 0; i < 6; ++i) CHECK(rig.bounces[s].v[i] == before.bounces[s].v[i]);
    CHECK(rig.flags[s] == before.flags[s]);
    CHECK(rig.prev_ng[s] == before.prev_ng[s]);
    CHECK(rig.prev_ns[s] == before.prev_ns[s]);
    for (int l = 0; l < ar::kLanes; ++l) {
      CHECK(rig.beta[s].v[l] == before.beta[s].v[l]);
      CHECK(rig.r_u[s].v[l] == before.r_u[s].v[l]);
      CHECK(rig.r_l[s].v[l] == before.r_l[s].v[l]);
    }
    const ar::Real3 d = rig.ray_d[s], o = before.ray_o[s], o2 = rig.ray_o[s];
    CHECK((d.x == before.ray_d[s].x && d.y == before.ray_d[s].y && d.z == before.ray_d[s].z));
    // Just past the lamp: beyond the hit point along the ray, by less than a millimetre (the
    // offset is 256 float ulps per component, 1.2e-4 at coordinates of 5, geom/ray_offset.h).
    const ar::Real3 p = o + d * before.hit_t[s];
    const double along = double(ar::dot(o2 - p, d)) / double(ar::length(d));
    CHECK(along > 0.0);
    CHECK(along < 1e-3);
    // The previous vertex rebuilt from the new origin is the old one (0003 d19 row 17).
    const ar::Real3 prev_old = o - d * before.prev_dist[s], prev_new = o2 - d * rig.prev_dist[s];
    CHECK(double(ar::length(prev_new - prev_old)) < 1e-5);
    CHECK(rig.prev_dist[s] > before.prev_dist[s]);
  }
  // From the sphere's outside the new origin is inside it, from inside it is outside.
  CHECK(double(ar::length(rig.ray_o[0])) < double(kSphereR));
  CHECK(double(ar::length(rig.ray_o[2])) > double(kSphereR));
}

}  // TEST_SUITE

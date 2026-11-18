// SPDX-License-Identifier: Apache-2.0
// shade_light (0004 D1, D3, D8): hand-built lamp crossings. A camera path that meets a lamp's
// emitting face adds Le beta times the emitter-hit MIS weight to its sample's slot, once; the
// non-emitting side (a sphere's inside on the way out, a rectangle's back) and a light path add
// nothing. Every crossing re-queues INTERSECT_CLOSEST from just past the lamp in the same
// direction with bounces, event_index, flags and throughput unchanged, and prev_dist grows so the
// previous vertex rebuilt from the new origin is the same point (0003 d19 row 17). A lamp instance
// whose record has no lamp shape fails its sample loudly (INVALID).
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
#include "film/resolve.h"
#include "kernel/kernel_globals.h"
#include "kernel/light/light_sampler.h"
#include "kernel/material/slot_resolve.h"
#include "kernel/stages/shade_light.h"
#include "kernel_cpu/stage_cpu.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kSlots = 9;
constexpr double kPi = 3.14159265358979323846;
constexpr float kSphereP = 30.0f, kSphereR = 0.5f, kRectP = 12.0f, kDiskP = 7.0f, kDiskR = 0.5f;
// The disk crossing of path 7: from the previous vertex kDiskPrev to the point kDiskHit on the disk.
constexpr double kDiskPrev[3] = {1.0, 0.0, 0.3}, kDiskHit[3] = {4.0, 0.1, 0.2};
constexpr uint16_t kEvent = 9;

// Lights [decoy point, sphere at the origin (radius 0.5), rectangle 1 x 1 at z = 5 facing -z,
// disk of radius 0.5 at (4, 0, 0) facing -x]. Instances 0, 1, 2 are the sphere's, the rectangle's and
// the disk's primitives; instance 3 names the point light, which has no lamp shape (a broken table). Colours are a constant
// REFLECTANCE-semantic 1, so Le is the geometric factor times P whatever the observer tables.
// Paths (ray_d unit unless stated):
//   0 camera, first vertex: (0, 0, -3) toward +z meets the sphere's outside at t = 2.5
//   1 camera, after a diffuse bounce at (0, 0, -3): origin (0, 0, -2.5), prev_dist 0.5, t = 2
//   2 camera, inside the sphere: from the centre toward +z, t = 0.5 (leaving: no Le)
//   3 light path, the same crossing as 0 (passes through, no contribution)
//   4 camera, after a delta bounce: (0.2, 0.1, 3) toward +z, ray_d of length 2, meets the
//     rectangle's front at t = 1
//   5 camera, first vertex: (0.1, -0.2, 8) toward -z meets the rectangle's back at t = 3
//   6 camera, after a diffuse bounce at (0, 0, 3): origin (0, 0, 3.5), prev_dist 0.5, meets the
//     rectangle's front at t = 1.5
//   7 camera, after a diffuse bounce at kDiskPrev: origin 0.5 along the ray, meets the disk's
//     front at kDiskHit
//   8 camera, first vertex, on instance 3: the fail-loudly branch
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
  std::vector<ar::DeviceInstanceRecord> instances = std::vector<ar::DeviceInstanceRecord>(4);
  std::vector<ar::LightRecord> lights;
  std::vector<float> pmf{0.1f, 0.4f, 0.3f, 0.2f};
  ar::LightSamplerView sampler{};
  std::vector<uint32_t> queue{0, 1, 2, 3, 4, 5, 6, 7, 8}, active{0, 1, 2, 3, 4, 5, 6, 7, 8};
  std::vector<uint8_t> next_stage = std::vector<uint8_t>(kSlots, uint8_t(ar::Stage::SHADE_LIGHT));
  std::vector<uint8_t> term = std::vector<uint8_t>(kSlots, uint8_t(ar::Term::ACTIVE));
  std::vector<uint32_t> flags = std::vector<uint32_t>(kSlots, 0u);
  std::vector<uint32_t> sample_id{0, 1, 2, 3, 4, 5, 6, 7, 8};
  std::vector<ar::U16x6> bounces = std::vector<ar::U16x6>(kSlots, ar::U16x6{{0, 0, 0, 0, 0, 0}});
  std::vector<uint16_t> event_index = std::vector<uint16_t>(kSlots, kEvent);
  std::vector<ar::Real3> ray_o = std::vector<ar::Real3>(kSlots), ray_d = std::vector<ar::Real3>(kSlots);
  std::vector<ar::Spec> lambda_t = std::vector<ar::Spec>(kSlots), beta = std::vector<ar::Spec>(kSlots);
  std::vector<ar::Spec> r_u = std::vector<ar::Spec>(kSlots), r_l = std::vector<ar::Spec>(kSlots);
  std::vector<uint32_t> prev_ng = std::vector<uint32_t>(kSlots), prev_ns = std::vector<uint32_t>(kSlots);
  std::vector<ar::Real> prev_dist = std::vector<ar::Real>(kSlots, 0.0f), hit_t = std::vector<ar::Real>(kSlots);
  std::vector<uint32_t> hit_inst{0, 0, 0, 0, 1, 1, 1, 2, 3};
  ar::StepEntry step{0, kSlots, 0};

  LampRig() : pool(ctx, 1), dev(ledger, pool) {
    FilmRequest req;
    req.max_width = kSlots;
    req.max_height = 1;
    req.slot_capacity = kSlots;
    req.statistics = true;
    req.termination_stats = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    const ar::SpectrumRef c = ar::make_const_ref(ar::SpectrumSemantic::REFLECTANCE, ar::Real(1));
    lights = {ar::light_make(ar::LightType::POINT, ar::Real3{0, 0, 0}, ar::Real(0), ar::Real(100), c),
              ar::light_make(ar::LightType::SPHERE, ar::Real3{0, 0, 0}, ar::Real(kSphereR), ar::Real(kSphereP), c),
              ar::light_make_area(ar::LightType::AREA_RECT, ar::Real3{0, 0, 5}, ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0},
                                  ar::Real(1), ar::Real(1), ar::Real(kRectP), c),
              ar::light_make_area(ar::LightType::AREA_DISK, ar::Real3{4, 0, 0}, ar::Real3{0, 1, 0}, ar::Real3{0, 0, 1},
                                  ar::Real(kDiskR), ar::Real(0), ar::Real(kDiskP), c)};
    sampler.light_count = uint32_t(lights.size());
    sampler.pmf = pmf.data();
    for (auto& r : instances) r = {};
    instances[0].light_index = 1;
    instances[1].light_index = 2;
    instances[2].light_index = 3;
    instances[3].light_index = 0;
    scene.instances = instances.data();
    scene.instance_count = uint32_t(instances.size());

    const double dv[3] = {kDiskHit[0] - kDiskPrev[0], kDiskHit[1] - kDiskPrev[1], kDiskHit[2] - kDiskPrev[2]};
    const double dl = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
    const ar::Real3 d7{float(dv[0] / dl), float(dv[1] / dl), float(dv[2] / dl)};
    const ar::Real3 o7{float(kDiskPrev[0] + 0.5 * dv[0] / dl), float(kDiskPrev[1] + 0.5 * dv[1] / dl),
                       float(kDiskPrev[2] + 0.5 * dv[2] / dl)};
    ray_o = {{0, 0, -3}, {0, 0, -2.5f}, {0, 0, 0}, {0, 0, -3}, {0.2f, 0.1f, 3}, {0.1f, -0.2f, 8}, {0, 0, 3.5f}, o7, {0, 0, -3}};
    ray_d = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 2}, {0, 0, -1}, {0, 0, 1}, d7, {0, 0, 1}};
    hit_t = {2.5f, 2.0f, 0.5f, 2.5f, 1.0f, 3.0f, 1.5f, float(dl - 0.5), 1.0f};
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
    bounces[6] = ar::U16x6{{1, 1, 0, 0, 0, 0}};
    prev_dist[6] = 0.5f;  // the previous vertex is (0, 0, 3)
    bounces[7] = ar::U16x6{{3, 3, 0, 0, 0, 0}};
    prev_dist[7] = 0.5f;  // the previous vertex is kDiskPrev

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
  const double disk_area = kPi * double(kDiskR) * double(kDiskR);
  const double l_disk = double(kDiskP) / (kPi * disk_area);
  // MIS denominators (integrator/mis.h): avg(r_u) unweighted; avg(r_u + r_l p_light) after a
  // non-delta bounce, p_light = pmf x the lamp's solid-angle pdf from the previous vertex:
  //  - sphere (path 1, from (0, 0, -3)): the cone, 1 / (2 pi (1 - cos theta_max));
  //  - rectangle (path 6, from (0, 0, 3), on its axis 2 below it): 1 / Omega, Omega =
  //    4 asin(a b / sqrt((a^2 + h^2)(b^2 + h^2))) for half sides a, b at distance h, the closed
  //    form of the integral of cos theta / r^2 over the rectangle (spherical-rectangle sampling's
  //    pdf, Urena et al. 2013);
  //  - disk (path 7): area sampling, |q - p|^2 / (A cos theta_l) (0004 D6).
  const double sin2 = double(kSphereR) * double(kSphereR) / 9.0;
  const double p_sphere = double(rig.pmf[1]) / (2.0 * kPi * (1.0 - std::sqrt(1.0 - sin2)));
  const double a = 0.5, h = 2.0;
  const double omega = 4.0 * std::asin(a * a / std::sqrt((a * a + h * h) * (a * a + h * h)));
  const double p_rect = double(rig.pmf[2]) / omega;
  const double dv[3] = {kDiskHit[0] - kDiskPrev[0], kDiskHit[1] - kDiskPrev[1], kDiskHit[2] - kDiskPrev[2]};
  const double d2 = dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2];
  const double p_disk = double(rig.pmf[3]) * d2 / (disk_area * (dv[0] / std::sqrt(d2)));  // front is -x
  const auto weighted = [&](uint32_t s, double p_light) {
    double w[4];
    for (int l = 0; l < 4; ++l) w[l] = double(r_u[s].v[l]) + double(r_l[s].v[l]) * p_light;
    return avg4(w);
  };
  double ru[4];
  for (int l = 0; l < 4; ++l) ru[l] = double(r_u[0].v[l]);
  const double expect_scale[kSlots - 1] = {l_sphere / avg4(ru),          l_sphere / weighted(1, p_sphere), 0.0, 0.0,
                                           l_rect / avg4(ru),            0.0,
                                           l_rect / weighted(6, p_rect), l_disk / weighted(7, p_disk)};
  for (uint32_t s = 0; s + 1 < kSlots; ++s) {  // path 8 is the broken table's (its own case)
    CAPTURE(s);
    const ar::Spec got = rig.slot_value(s);
    for (int l = 0; l < ar::kLanes; ++l) {
      const double want = double(beta[s].v[l]) * expect_scale[s];
      if (want == 0.0) CHECK(got.v[l] == 0.0f);
      else CHECK(double(got.v[l]) == doctest::Approx(want).epsilon(2e-6));
    }
  }
}

TEST_CASE("shade_light/a lamp instance without a lamp shape fails the sample") {
  LampRig rig;
  rig.run();
  const uint32_t s = kSlots - 1;
  CHECK(!ar::film_sample_valid(rig.slot_value(s)));
  CHECK(rig.term[s] == uint8_t(ar::Term::INVALID));
  CHECK(rig.next_stage[s] == uint8_t(ar::Stage::TERMINATED));
  const ar::FilmData& f = rig.film->data();
  CHECK(f.count[uint64_t(uint32_t(f.term_hist) + uint32_t(ar::Term::INVALID)) * f.plane_stride + s] == 1u);
}
TEST_CASE("shade_light/requeue keeps event index") {
  LampRig rig;
  const LampRig before;  // an untouched copy of every input
  rig.run();
  for (uint32_t s = 0; s + 1 < kSlots; ++s) {  // path 8 ends (the broken table's case)
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

// SPDX-License-Identifier: Apache-2.0
// shade_light on the GPU against the CPU float stage (0009 d17 step iii, the per-stage fixture of
// 0002 D4): 10^5 synthetic lamp crossings over a table of [decoy point, sphere, rectangle, disk],
// from either side of each lamp, with random wavelengths, throughputs (1% non-finite), bounce
// counts, light paths (10%), delta last bounces (30%) and the emitter-MIS inputs (r_u, r_l, the
// previous vertex). Integer fields (term, next_stage, event_index, the slot flags) must be
// bit-equal; the slot radiance agrees to 0002 D12's throughput threshold, the re-queued origin and
// prev_dist to its evaluation threshold (FMA-off GPU build vs CPU float).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D12; 0009 d17; 0004 D1, D8.
// The production module is launched, so what runs is the shipped code.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "agree.h"
#include "device/cpu/cpu_device.h"
#include "host/device.h"
#include "host/film/film_layout.h"
#include "host/thread_pool.h"
#include "stages/shade_light.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kFilmW = 250, kFilmH = 200, kK = 2;
constexpr uint32_t kN = kFilmW * kFilmH * kK;  // 10^5 paths

struct Snapshot {
  std::vector<float> slot_L;
  std::vector<uint32_t> slot_flags;
  std::vector<uint8_t> next_stage, term;
  std::vector<uint16_t> event_index;
  std::vector<ar::Real3> ray_o;
  std::vector<ar::Real> prev_dist;
};

std::vector<float> observer_illuminant() {
  std::vector<float> t(ar::kObsSamples);
  for (int i = 0; i < ar::kObsSamples; ++i) t[size_t(i)] = 1.0f + 0.5f * std::sin(float(i) / 37.0f);
  return t;
}

ar::Real3 unit_dir(uint32_t i, uint32_t dim) {
  const ar::Real3 v{float(agree::unit(i, dim)) - 0.5f, float(agree::unit(i, dim + 1)) - 0.5f,
                    float(agree::unit(i, dim + 2)) - 0.5f + 1e-3f};
  return ar::normalize(v);
}

Snapshot run(Device& dev, ModuleDevice* gpu) {
  const Owner owner{1, 1};
  const auto put = [&](const void* p, uint64_t bytes) {
    DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, bytes, owner);
    dev.upload(b, 0, p, bytes);
    return b;
  };
  FilmRequest req;
  req.max_width = kFilmW;
  req.max_height = kFilmH;
  req.slot_capacity = kN;
  Film film(dev, FilmLayout::build(req), owner);
  DeviceBuffer film_buf = put(&film.data(), sizeof(ar::FilmData));

  std::vector<uint32_t> identity(kN), list(kFilmW * kFilmH);
  for (uint32_t i = 0; i < kN; ++i) identity[i] = i;
  for (uint32_t i = 0; i < list.size(); ++i) list[i] = i;
  DeviceBuffer queue_buf = put(identity.data(), kN * sizeof(uint32_t));
  DeviceBuffer list_buf = put(list.data(), list.size() * sizeof(uint32_t));
  const ar::StepEntry step{0, kN, 0};
  ar::StepTable table{};
  table.stage[uint32_t(ar::Stage::SHADE_LIGHT)] = step;
  DeviceBuffer table_buf = put(&table, sizeof table);

  // The light table and the lamp instances (instance k is the primitive of light k + 1).
  ar::SpectrumRef colour{};
  colour.kind = uint8_t(ar::SpectrumKind::RGB);
  colour.semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
  colour.p[0] = 0.3f, colour.p[1] = -0.2f, colour.p[2] = 0.1f, colour.p[3] = 0.9f;
  const ar::Real3 c_sphere{0.5f, -0.25f, 1.0f}, c_rect{-1.0f, 0.5f, 2.0f}, c_disk{1.5f, 1.0f, -0.5f};
  const std::vector<ar::LightRecord> lights{
      ar::light_make(ar::LightType::POINT, ar::Real3{1, 2, 3}, ar::Real(0), ar::Real(50), colour),
      ar::light_make(ar::LightType::SPHERE, c_sphere, ar::Real(0.4), ar::Real(20), colour),
      ar::light_make_area(ar::LightType::AREA_RECT, c_rect, ar::normalize(ar::Real3{1, 1, 0}),
                          ar::normalize(ar::Real3{-1, 1, 0.5f}), ar::Real(0.8), ar::Real(0.5),
                          ar::Real(15), colour),
      ar::light_make_area(ar::LightType::AREA_DISK, c_disk, ar::Real3{0, 1, 0}, ar::Real3{0, 0, 1}, ar::Real(0.3),
                          ar::Real(0), ar::Real(9), colour)};
  DeviceBuffer lights_buf = put(lights.data(), lights.size() * sizeof(ar::LightRecord));
  const std::vector<float> pmf{0.1f, 0.4f, 0.3f, 0.2f};
  DeviceBuffer pmf_buf = put(pmf.data(), pmf.size() * sizeof(float));
  ar::LightSamplerView sampler{};
  sampler.light_count = uint32_t(lights.size());
  sampler.pmf = static_cast<const float*>(pmf_buf.ptr);
  DeviceBuffer sampler_buf = put(&sampler, sizeof sampler);
  std::vector<ar::DeviceInstanceRecord> instances(3);
  for (uint32_t k = 0; k < 3; ++k) instances[k] = {}, instances[k].light_index = k + 1u;
  DeviceBuffer inst_buf = put(instances.data(), instances.size() * sizeof(ar::DeviceInstanceRecord));
  ar::SceneData scene{};
  scene.instances = static_cast<const ar::DeviceInstanceRecord*>(inst_buf.ptr);
  scene.instance_count = 3;
  DeviceBuffer scene_buf = put(&scene, sizeof scene);

  // Path state, inputs from the sampler's independent stream (agree::unit), never a rand().
  std::vector<uint8_t> next(kN, uint8_t(ar::Stage::SHADE_LIGHT)), term(kN, uint8_t(ar::Term::ACTIVE));
  std::vector<uint32_t> flags(kN), sample_id(kN), hit_inst(kN), prev_ng(kN), prev_ns(kN);
  std::vector<uint16_t> event_index(kN);
  std::vector<ar::U16x6> bounces(kN);
  std::vector<ar::Spec> lambda_t(kN), beta(kN), r_u(kN), r_l(kN);
  std::vector<ar::Real3> ray_o(kN), ray_d(kN);
  std::vector<ar::Real> prev_dist(kN), hit_t(kN);
  const float inf = std::numeric_limits<float>::infinity();
  for (uint32_t i = 0; i < kN; ++i) {
    for (int l = 0; l < ar::kLanes; ++l) {
      lambda_t[i].v[l] = 380.0f + 400.0f * float(agree::unit(i, uint32_t(l)));
      beta[i].v[l] = 2.0f * float(agree::unit(i, 4u + uint32_t(l)));
      r_u[i].v[l] = 0.5f + float(agree::unit(i, 14u + uint32_t(l)));
      r_l[i].v[l] = 4.0f * float(agree::unit(i, 18u + uint32_t(l)));
    }
    const double bad = agree::unit(i, 8);  // 1% of paths carry a non-finite throughput lane
    if (bad < 0.005) beta[i].v[int(agree::unit(i, 9) * 4.0)] = std::numeric_limits<float>::quiet_NaN();
    else if (bad < 0.01) beta[i].v[int(agree::unit(i, 9) * 4.0)] = inf;
    sample_id[i] = i;
    flags[i] = ar::set_hero_lane(0u, uint32_t(agree::unit(i, 11) * 4.0) & 3u);
    if (agree::unit(i, 10) < 0.1) flags[i] |= ar::PATH_KIND;
    if (agree::unit(i, 13) < 0.3) flags[i] |= ar::LAST_BOUNCE_SPECULAR;
    bounces[i] = ar::U16x6{{uint16_t(agree::unit(i, 12) * 13.0), 0, 0, 0, 0, 0}};
    event_index[i] = uint16_t(agree::unit(i, 22) * 4000.0);
    // A point on lamp k (instance k - 1), a direction from either side, an origin t before it.
    const uint32_t k = 1u + (uint32_t(agree::unit(i, 23) * 3.0) % 3u);
    hit_inst[i] = k - 1u;
    const float a = float(agree::unit(i, 24)) - 0.5f, b = float(agree::unit(i, 25)) - 0.5f;
    ar::Real3 p;
    if (k == 1u) p = c_sphere + unit_dir(i, 26) * ar::Real(0.4);
    else if (k == 2u) p = c_rect + ar::area_axis(lights[2], 0) * (0.8f * a) + ar::area_axis(lights[2], 1) * (0.5f * b);
    else p = c_disk + ar::area_axis(lights[3], 0) * (0.4f * a) + ar::area_axis(lights[3], 1) * (0.4f * b);
    ray_d[i] = unit_dir(i, 29);
    hit_t[i] = 0.25f + 3.0f * float(agree::unit(i, 32));
    ray_o[i] = p - ray_d[i] * hit_t[i];
    const ar::Real3 n{float(agree::unit(i, 33)) - 0.5f, float(agree::unit(i, 34)) - 0.5f, 1.0f};
    prev_ng[i] = ar::oct_encode(ar::normalize(n));
    prev_ns[i] = ar::oct_encode(ar::normalize(n + ar::Real3{0.1f, 0.0f, 0.0f}));
    prev_dist[i] = float(agree::unit(i, 35));
  }
  DeviceBuffer next_buf = put(next.data(), kN), term_buf = put(term.data(), kN);
  DeviceBuffer flags_buf = put(flags.data(), kN * sizeof(uint32_t));
  DeviceBuffer sample_buf = put(sample_id.data(), kN * sizeof(uint32_t));
  DeviceBuffer inst_idx_buf = put(hit_inst.data(), kN * sizeof(uint32_t));
  DeviceBuffer ev_buf = put(event_index.data(), kN * sizeof(uint16_t));
  DeviceBuffer bounces_buf = put(bounces.data(), kN * sizeof(ar::U16x6));
  DeviceBuffer lambda_buf = put(lambda_t.data(), kN * sizeof(ar::Spec));
  DeviceBuffer beta_buf = put(beta.data(), kN * sizeof(ar::Spec));
  DeviceBuffer ru_buf = put(r_u.data(), kN * sizeof(ar::Spec)), rl_buf = put(r_l.data(), kN * sizeof(ar::Spec));
  DeviceBuffer ro_buf = put(ray_o.data(), kN * sizeof(ar::Real3)), rd_buf = put(ray_d.data(), kN * sizeof(ar::Real3));
  DeviceBuffer png_buf = put(prev_ng.data(), kN * 4), pns_buf = put(prev_ns.data(), kN * 4);
  DeviceBuffer pd_buf = put(prev_dist.data(), kN * sizeof(ar::Real)), ht_buf = put(hit_t.data(), kN * sizeof(ar::Real));
  ar::PathStateSoA soa{};
  soa.sample_id = static_cast<uint32_t*>(sample_buf.ptr);
  soa.next_stage = static_cast<uint8_t*>(next_buf.ptr);
  soa.term = static_cast<uint8_t*>(term_buf.ptr);
  soa.flags = static_cast<uint32_t*>(flags_buf.ptr);
  soa.bounces = static_cast<ar::U16x6*>(bounces_buf.ptr);
  soa.event_index = static_cast<uint16_t*>(ev_buf.ptr);
  soa.lambda_t = static_cast<ar::Spec*>(lambda_buf.ptr);
  soa.beta = static_cast<ar::Spec*>(beta_buf.ptr);
  soa.r_u = static_cast<ar::Spec*>(ru_buf.ptr), soa.r_l = static_cast<ar::Spec*>(rl_buf.ptr);
  soa.ray_o = static_cast<ar::Real3*>(ro_buf.ptr), soa.ray_d = static_cast<ar::Real3*>(rd_buf.ptr);
  soa.prev_ng = static_cast<uint32_t*>(png_buf.ptr), soa.prev_ns = static_cast<uint32_t*>(pns_buf.ptr);
  soa.prev_dist = static_cast<ar::Real*>(pd_buf.ptr);
  soa.hit_t = static_cast<ar::Real*>(ht_buf.ptr);
  soa.hit_inst = static_cast<uint32_t*>(inst_idx_buf.ptr);
  DeviceBuffer soa_buf = put(&soa, sizeof soa);

  const std::vector<float> illum = observer_illuminant();
  const std::vector<float> cmf(3 * ar::kObsSamples, 0.0f);
  DeviceBuffer illum_buf = put(illum.data(), illum.size() * sizeof(float));
  DeviceBuffer cmf_buf = put(cmf.data(), cmf.size() * sizeof(float));
  ar::SceneTables tables{};
  tables.observer = ar::ObserverView{static_cast<const float*>(cmf_buf.ptr), static_cast<const float*>(illum_buf.ptr)};
  DeviceBuffer tables_buf = put(&tables, sizeof tables);

  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(soa_buf.ptr);
  kg.step_table = static_cast<const ar::StepTable*>(table_buf.ptr);
  kg.main_queue = static_cast<const uint32_t*>(queue_buf.ptr);
  kg.active_pixels = static_cast<uint32_t*>(list_buf.ptr);
  kg.film = static_cast<ar::FilmData*>(film_buf.ptr);
  kg.tables = static_cast<const ar::SceneTables*>(tables_buf.ptr);
  kg.scene = static_cast<const ar::SceneData*>(scene_buf.ptr);
  kg.lights = static_cast<const ar::LightRecord*>(lights_buf.ptr);
  kg.light_sampler = static_cast<const ar::LightSamplerView*>(sampler_buf.ptr);
  kg.session.width = kFilmW;
  kg.session.height = kFilmH;
  kg.session.samples_per_pass = kK;
  DeviceBuffer kg_buf = put(&kg, sizeof kg);

  if (gpu != nullptr) {
    const uint32_t module = gpu->load_module_file(std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/shade_light.cubin");
    const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_buf.ptr);
    void* args[] = {&kg_dev};
    gpu->launch_module_kernel(module, "k_shade_light", (kN + 255) / 256, 256, args);
  } else {
    // On the CPU device the uploaded copies are host memory the stage reads directly.
    const ar::KernelGlobals host_kg = kg;
    cpu_stage_shade_light(host_kg, step, 0, kN);
  }
  dev.synchronize();

  Snapshot s;
  s.slot_L.resize(size_t(kN) * 4);
  s.slot_flags.resize(kN);
  s.next_stage.resize(kN);
  s.term.resize(kN);
  s.event_index.resize(kN);
  s.ray_o.resize(kN);
  s.prev_dist.resize(kN);
  const ar::FilmData& fd = film.data();
  const auto offset = [&](const void* p) { return uint64_t(static_cast<const char*>(p) - static_cast<const char*>(film.buffer().ptr)); };
  dev.download(s.slot_L.data(), film.buffer(), offset(fd.slot_L), s.slot_L.size() * sizeof(float));
  dev.download(s.slot_flags.data(), film.buffer(), offset(fd.slot_flags), kN * sizeof(uint32_t));
  dev.download(s.next_stage.data(), next_buf, 0, kN);
  dev.download(s.term.data(), term_buf, 0, kN);
  dev.download(s.event_index.data(), ev_buf, 0, kN * sizeof(uint16_t));
  dev.download(s.ray_o.data(), ro_buf, 0, kN * sizeof(ar::Real3));
  dev.download(s.prev_dist.data(), pd_buf, 0, kN * sizeof(ar::Real));

  for (DeviceBuffer* b : {&film_buf, &queue_buf, &list_buf, &table_buf, &lights_buf, &pmf_buf, &sampler_buf, &inst_buf,
                          &scene_buf, &next_buf, &term_buf, &flags_buf, &sample_buf, &inst_idx_buf, &ev_buf, &bounces_buf,
                          &lambda_buf, &beta_buf, &ru_buf, &rl_buf, &ro_buf, &rd_buf, &png_buf, &pns_buf, &pd_buf, &ht_buf,
                          &soa_buf, &illum_buf, &cmf_buf, &tables_buf, &kg_buf})
    dev.free(*b);
  return s;
}

agree::Results results(const std::vector<float>& v, uint32_t stride) {
  agree::Results r;
  r.n = uint32_t(v.size() / stride);
  r.stride = stride;
  r.v.assign(v.begin(), v.end());
  return r;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("shade_light/agreement") {
  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const Snapshot c = run(cpu, nullptr);
  const Snapshot g = run(*gpu, gpu.get());
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);

  // Integer fields: bitwise.
  CHECK(c.term == g.term);
  CHECK(c.next_stage == g.next_stage);
  CHECK(c.slot_flags == g.slot_flags);
  CHECK(c.event_index == g.event_index);
  // The fixture exercises both outcomes: re-queued crossings and invalid samples.
  uint32_t requeued = 0, invalid = 0;
  for (uint32_t i = 0; i < kN; ++i) {
    requeued += c.next_stage[i] == uint8_t(ar::Stage::INTERSECT_CLOSEST);
    invalid += c.term[i] == uint8_t(ar::Term::INVALID);
  }
  CHECK(invalid > 100u);
  CHECK(requeued + invalid == kN);
  // NaN positions (sample_mark_invalid) must match exactly; they are zeroed before the comparison.
  std::vector<float> lc = c.slot_L, lg = g.slot_L;
  uint32_t nan_mismatch = 0, lit = 0;
  for (size_t i = 0; i < lc.size(); ++i) {
    const bool a = !(lc[i] == lc[i]), b = !(lg[i] == lg[i]);
    nan_mismatch += a != b;
    if (a) lc[i] = 0.0f;
    if (b) lg[i] = 0.0f;
    lit += lc[i] > 0.0f;
  }
  CHECK(nan_mismatch == 0u);
  CHECK(lit > 4u * 20000u);  // radiance really was added (front-side camera crossings)
  CHECK(lit < 4u * 70000u);  // and back sides and light paths added none

  // Radiance (4 lanes per path): Le beta over the balance denominator, whose light pdf (the sphere's
  // cone, the spherical rectangle's solid angle) goes through transcendentals, so it is a weight of
  // the throughput class, as NEE's contribution (test_nee_agree). The re-queue state is plain
  // arithmetic: the evaluation class. Invalid paths keep their origin, so every origin is compared.
  const agree::Stats e = agree::compare(results(lg, 4), results(lc, 4), agree::Metric::REL_L2);
  CHECK(agree::report("shade_light", "slot radiance nofma vs cpu float", e, agree::Cls::THROUGHPUT,
                      agree::NOFMA_VS_CPU_FLOAT).pass);
  std::vector<float> oc, og, pc(c.prev_dist.begin(), c.prev_dist.end()), pg(g.prev_dist.begin(), g.prev_dist.end());
  for (uint32_t i = 0; i < kN; ++i) {
    oc.insert(oc.end(), {c.ray_o[i].x, c.ray_o[i].y, c.ray_o[i].z});
    og.insert(og.end(), {g.ray_o[i].x, g.ray_o[i].y, g.ray_o[i].z});
  }
  const agree::Stats eo = agree::compare(results(og, 3), results(oc, 3), agree::Metric::REL_L2);
  CHECK(agree::report("shade_light", "re-queued origin nofma vs cpu float", eo, agree::Cls::EVAL,
                      agree::NOFMA_VS_CPU_FLOAT).pass);
  const agree::Stats ep = agree::compare(results(pg, 1), results(pc, 1), agree::Metric::REL_L2);
  CHECK(agree::report("shade_light", "prev_dist nofma vs cpu float", ep, agree::Cls::EVAL,
                      agree::NOFMA_VS_CPU_FLOAT).pass);
}

}  // TEST_SUITE

// SPDX-License-Identifier: Apache-2.0
// shade_background on the GPU against the CPU float stage (0009 d17 step iii, the per-stage
// fixture of 0002 D4): 10^5 synthetic camera-path misses over a table of [decoy lamp, world, decoy
// lamp], two samples per pixel so the path-end statistics are shared between paths, with random
// wavelengths, throughputs (1% non-finite), bounce counts, hero lanes and the world-MIS inputs (r_u,
// r_l, previous vertex, delta last bounce; 074). Integer fields (term,
// next_stage, the slot flags and every COUNT plane) must be bit-equal; the slot radiance agrees to
// 0002 D12's evaluation threshold (FMA-off GPU build vs CPU float).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D12; 0009 d17; 0004 D5, D28.
// The production module is launched, so what runs is the shipped code.
#include <doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "agree.h"
#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/device.h"
#include "host/film/film_layout.h"
#include "host/material/material_store.h"
#include "host/scene/world.h"
#include "host/thread_pool.h"
#include "stages/shade_background.h"


using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kFilmW = 250, kFilmH = 200, kK = 2;
constexpr uint32_t kN = kFilmW * kFilmH * kK;  // 10^5 paths

// One device's copy of the fixture.
struct Snapshot {
  std::vector<float> slot_L;
  std::vector<uint32_t> slot_flags;
  std::vector<uint8_t> next_stage, term;
  std::vector<uint32_t> counts;  // every COUNT plane
  std::vector<float> adds;       // every ADD plane (the bounce sum: exact integers)
};

constexpr uint32_t kEnvW = 64, kEnvH = 32;

// A world program of an Environment Texture over a smooth float image, compiled by the host as the
// addon's graph would be (LOAD_CTX_P -> TEX_ENVIRONMENT -> EMIT_LE), with everything the stage reads
// besides the path state: the image, the sRGB decode table and the RGB-to-spectrum table.
struct ProgramWorld {
  MaterialStore store{std::make_shared<Ledger>()};
  std::vector<float> texels;
  std::array<float, ar::kSrgbTableSize> srgb{};
  StoredGraph graph;
  WorldOutput world;

  // `grey`: every texel grey, whose lift is analytic; otherwise saturated colours, whose lift goes through the
  // RGB-to-spectrum table.
  explicit ProgramWorld(uint32_t mode, bool grey) {
    texels.resize(size_t(kEnvW) * kEnvH * 4, 1.0f);
    const float pi = 3.14159265358979f;
    for (uint32_t y = 0; y < kEnvH; ++y)
      for (uint32_t x = 0; x < kEnvW; ++x) {
        float* t = &texels[(size_t(y) * kEnvW + x) * 4];
        const float fx = (float(x) + 0.5f) / float(kEnvW), fy = (float(y) + 0.5f) / float(kEnvH);
        t[0] = 0.6f + 0.4f * std::sin(2.0f * pi * fx) * std::sin(pi * fy);
        t[1] = 0.5f + 0.3f * std::cos(2.0f * pi * fx + 1.0f);
        t[2] = 0.3f + 0.4f * fy;
        if (grey) t[0] = t[1] = t[2] = 0.6f + 0.35f * std::sin(2.0f * pi * fx) * std::sin(pi * fy);
      }
    ar::srgb_decode_table(srgb.data());
    graph.f32 = {0.8f};
    graph.i32 = {7, int32_t(mode)};
    graph.nodes.assign(4, ar_graph_node{});
    graph.nodes[0].op = uint32_t(ar::Op::LOAD_CTX_P);
    graph.nodes[1].op = uint32_t(ar::Op::TEX_ENVIRONMENT), graph.nodes[1].i32_first = 0, graph.nodes[1].i32_count = 2;
    graph.nodes[2].op = uint32_t(ar::Op::CONST_F32), graph.nodes[2].f32_first = 0, graph.nodes[2].f32_count = 1;
    graph.nodes[3].op = uint32_t(ar::Op::EMIT_LE);
    graph.links = {{0u, 0u, 1u, 0u}, {1u, 0u, 3u, 0u}, {2u, 0u, 3u, 1u}};
    const ImageSlotFn slots = [](uint32_t key, uint32_t* slot) { return key == 7u ? (*slot = 0u, true) : false; };
    ar_world_desc desc = {};
    desc.surface_graph = 1;
    world = compile_world(desc, &graph, store.table(), nullptr, &slots);
  }
};

std::vector<float> observer_illuminant() {
  std::vector<float> t(ar::kObsSamples);
  for (int i = 0; i < ar::kObsSamples; ++i) t[size_t(i)] = 1.0f + 0.5f * std::sin(float(i) / 37.0f);
  return t;
}

// `program`: the world record names the program of that fixture (0005 item 32) and every ray is a
// random unit direction, so the lookup, the Cycles mapping and the lift are what is compared.
Snapshot run(Device& dev, ModuleDevice* gpu, const ProgramWorld* program = nullptr) {
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
  req.statistics = true;
  req.termination_stats = true;
  Film film(dev, FilmLayout::build(req), owner);
  DeviceBuffer film_buf = put(&film.data(), sizeof(ar::FilmData));

  std::vector<uint32_t> identity(kN), list(kFilmW * kFilmH);
  for (uint32_t i = 0; i < kN; ++i) identity[i] = i;
  for (uint32_t i = 0; i < list.size(); ++i) list[i] = i;
  DeviceBuffer queue_buf = put(identity.data(), kN * sizeof(uint32_t));
  DeviceBuffer list_buf = put(list.data(), list.size() * sizeof(uint32_t));
  const ar::StepEntry step{0, kN, 0};
  ar::StepTable table{};
  table.stage[uint32_t(ar::Stage::SHADE_BACKGROUND)] = step;
  DeviceBuffer table_buf = put(&table, sizeof table);

  // Path state, inputs from the sampler's independent stream (agree::unit), never a rand().
  std::vector<uint8_t> next(kN, uint8_t(ar::Stage::SHADE_BACKGROUND)), term(kN, uint8_t(ar::Term::ACTIVE));
  std::vector<uint32_t> flags(kN), sample_id(kN);
  std::vector<ar::U16x6> bounces(kN);
  std::vector<ar::Spec> lambda_t(kN), beta(kN), r_u(kN), r_l(kN);
  std::vector<ar::Real3> ray_o(kN), ray_d(kN);
  std::vector<uint32_t> prev_ng(kN), prev_ns(kN);
  std::vector<ar::Real> prev_dist(kN);
  const float inf = std::numeric_limits<float>::infinity();
  for (uint32_t i = 0; i < kN; ++i) {
    for (int l = 0; l < ar::kLanes; ++l) {
      lambda_t[i].v[l] = 380.0f + 400.0f * float(agree::unit(i, uint32_t(l)));
      beta[i].v[l] = 2.0f * float(agree::unit(i, 4u + uint32_t(l)));
    }
    const double bad = agree::unit(i, 8);  // 1% of paths carry a non-finite throughput lane
    if (bad < 0.005) beta[i].v[int(agree::unit(i, 9) * 4.0)] = std::numeric_limits<float>::quiet_NaN();
    else if (bad < 0.01) beta[i].v[int(agree::unit(i, 9) * 4.0)] = inf;
    sample_id[i] = i;
    flags[i] = ar::set_hero_lane(uint32_t(agree::unit(i, 10) * 4096.0) << 9, uint32_t(agree::unit(i, 11) * 4.0) & 3u) &
               ~ar::PATH_KIND;
    bounces[i] = ar::U16x6{{uint16_t(agree::unit(i, 12) * 13.0), 0, 0, 0, 0, 0}};
    // The world MIS inputs (074): r_u, r_l, the previous vertex and a delta last bounce on 30%.
    if (agree::unit(i, 13) < 0.3) flags[i] |= ar::LAST_BOUNCE_SPECULAR;
    for (int l = 0; l < ar::kLanes; ++l) {
      r_u[i].v[l] = 0.5f + float(agree::unit(i, 14u + uint32_t(l)));
      r_l[i].v[l] = 4.0f * float(agree::unit(i, 18u + uint32_t(l)));
    }
    const ar::Real3 n{float(agree::unit(i, 22)) - 0.5f, float(agree::unit(i, 23)) - 0.5f, 1.0f};
    prev_ng[i] = ar::oct_encode(ar::normalize(n));
    prev_ns[i] = ar::oct_encode(ar::normalize(n + ar::Real3{0.1f, 0.0f, 0.0f}));
    prev_dist[i] = float(agree::unit(i, 24));
    ray_o[i] = ar::Real3{float(agree::unit(i, 25)), float(agree::unit(i, 26)), float(agree::unit(i, 27))};
    ray_d[i] = ar::normalize(ar::Real3{float(agree::unit(i, 28)) - 0.5f, float(agree::unit(i, 29)) - 0.5f, 0.7f});
    if (program != nullptr) {  // uniform on the sphere
      const float z = 2.0f * float(agree::unit(i, 30)) - 1.0f, phi = 6.2831853f * float(agree::unit(i, 31));
      const float r = std::sqrt(1.0f - z * z);
      ray_d[i] = ar::Real3{r * std::cos(phi), r * std::sin(phi), z};
    }
  }
  std::vector<uint8_t> ray_type(kN, 0);
  DeviceBuffer next_buf = put(next.data(), kN), term_buf = put(term.data(), kN);
  DeviceBuffer flags_buf = put(flags.data(), kN * sizeof(uint32_t));
  DeviceBuffer bounces_buf = put(bounces.data(), kN * sizeof(ar::U16x6));
  DeviceBuffer lambda_buf = put(lambda_t.data(), kN * sizeof(ar::Spec));
  DeviceBuffer beta_buf = put(beta.data(), kN * sizeof(ar::Spec));
  DeviceBuffer ru_buf = put(r_u.data(), kN * sizeof(ar::Spec)), rl_buf = put(r_l.data(), kN * sizeof(ar::Spec));
  DeviceBuffer ro_buf = put(ray_o.data(), kN * sizeof(ar::Real3)), rd_buf = put(ray_d.data(), kN * sizeof(ar::Real3));
  DeviceBuffer png_buf = put(prev_ng.data(), kN * 4), pns_buf = put(prev_ns.data(), kN * 4);
  DeviceBuffer pd_buf = put(prev_dist.data(), kN * sizeof(ar::Real));
  ar::PathStateSoA soa{};
  DeviceBuffer sample_buf = put(sample_id.data(), kN * sizeof(uint32_t));
  soa.sample_id = static_cast<uint32_t*>(sample_buf.ptr);
  soa.next_stage = static_cast<uint8_t*>(next_buf.ptr);
  soa.term = static_cast<uint8_t*>(term_buf.ptr);
  soa.flags = static_cast<uint32_t*>(flags_buf.ptr);
  soa.bounces = static_cast<ar::U16x6*>(bounces_buf.ptr);
  soa.lambda_t = static_cast<ar::Spec*>(lambda_buf.ptr);
  soa.beta = static_cast<ar::Spec*>(beta_buf.ptr);
  soa.r_u = static_cast<ar::Spec*>(ru_buf.ptr), soa.r_l = static_cast<ar::Spec*>(rl_buf.ptr);
  soa.ray_o = static_cast<ar::Real3*>(ro_buf.ptr), soa.ray_d = static_cast<ar::Real3*>(rd_buf.ptr);
  soa.prev_ng = static_cast<uint32_t*>(png_buf.ptr), soa.prev_ns = static_cast<uint32_t*>(pns_buf.ptr);
  soa.prev_dist = static_cast<ar::Real*>(pd_buf.ptr);
  DeviceBuffer rtype_buf = put(ray_type.data(), kN);
  soa.ray_type = static_cast<uint8_t*>(rtype_buf.ptr);
  DeviceBuffer soa_buf = put(&soa, sizeof soa);

  // The light table: an RGB illuminant world between two decoy lamps, so a wrong index would read a lamp.
  ar::SpectrumRef colour{};
  colour.kind = uint8_t(ar::SpectrumKind::RGB);
  colour.semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
  colour.p[0] = 0.3f, colour.p[1] = -0.2f, colour.p[2] = 0.1f, colour.p[3] = 0.9f;
  const ar::LightRecord decoy =
      ar::light_make(ar::LightType::POINT, ar::Real3{1, 2, 3}, ar::Real(0), ar::Real(50), colour);
  const ar::SpectrumRef one = ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, ar::Real(1));
  const std::vector<ar::LightRecord> lights{
      decoy,
      program != nullptr ? ar::light_make_world(ar::Real(1.75), one, ar::kLightWorldProgram)
                         : ar::light_make_world(ar::Real(1.75), colour),
      decoy};
  DeviceBuffer lights_buf = put(lights.data(), lights.size() * sizeof(ar::LightRecord));
  // Selection probabilities for the world's MIS weight (the alias bins are never read by a miss).
  // A program world has importance 0 (host/light/light_builder.cpp), so its pmf is 0.
  const std::vector<float> pmf{0.3f, program != nullptr ? 0.0f : 0.45f, 0.25f};
  DeviceBuffer pmf_buf = put(pmf.data(), pmf.size() * sizeof(float));
  ar::LightSamplerView sampler{};
  sampler.light_count = uint32_t(lights.size());
  sampler.pmf = static_cast<const float*>(pmf_buf.ptr);
  DeviceBuffer sampler_buf = put(&sampler, sizeof sampler);
  const std::vector<float> illum = observer_illuminant();
  const std::vector<float> cmf(3 * ar::kObsSamples, 0.0f);
  DeviceBuffer illum_buf = put(illum.data(), illum.size() * sizeof(float));
  DeviceBuffer cmf_buf = put(cmf.data(), cmf.size() * sizeof(float));
  ar::SceneTables tables{};
  tables.observer = ar::ObserverView{static_cast<const float*>(cmf_buf.ptr), static_cast<const float*>(illum_buf.ptr)};
  // The program world's tables: the RGB-to-spectrum table with device addresses, the image, the sRGB
  // decode table, and the SceneData that holds the program words and their constant block.
  std::vector<DeviceBuffer> program_bufs;
  ar::Rgb2SpecTable rgb_table{};
  ar::DeviceImageView view{};
  ar::SceneData scene_data{};
  if (program != nullptr) {
    const ar::Rgb2SpecTable& host_table = program->store.table();
    rgb_table = host_table;
    const uint64_t res = uint64_t(host_table.res);
    program_bufs.push_back(put(host_table.z_nodes, res * 4));
    rgb_table.z_nodes = static_cast<const float*>(program_bufs.back().ptr);
    program_bufs.push_back(put(host_table.coeffs, 36 * res * res * res));
    rgb_table.coeffs = static_cast<const float*>(program_bufs.back().ptr);
    program_bufs.push_back(put(&rgb_table, sizeof rgb_table));
    tables.rgb2spec = static_cast<const ar::Rgb2SpecTable*>(program_bufs.back().ptr);
    program_bufs.push_back(put(program->texels.data(), program->texels.size() * 4));
    view.texels = uint64_t(uintptr_t(program_bufs.back().ptr));
    view.width = kEnvW, view.height = kEnvH, view.channels = 4;
    view.format = uint32_t(ar::TexFormat::FLOAT), view.encoding = uint32_t(ar::TexEncoding::LINEAR);
    program_bufs.push_back(put(&view, sizeof view));
    tables.images = static_cast<const ar::DeviceImageView*>(program_bufs.back().ptr);
    tables.image_count = 1;
    program_bufs.push_back(put(program->srgb.data(), program->srgb.size() * 4));
    tables.srgb_decode = static_cast<const float*>(program_bufs.back().ptr);
    program_bufs.push_back(put(program->world.program.data(), program->world.program.size() * 4));
    scene_data.world_program = static_cast<const uint32_t*>(program_bufs.back().ptr);
    scene_data.world_program_words = uint32_t(program->world.program.size());
    program_bufs.push_back(put(&program->world.block, sizeof(ar::ConstantBlock)));
    scene_data.world_constants = static_cast<const ar::ConstantBlock*>(program_bufs.back().ptr);
    program_bufs.push_back(put(&scene_data, sizeof scene_data));
  }
  DeviceBuffer tables_buf = put(&tables, sizeof tables);

  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(soa_buf.ptr);
  kg.step_table = static_cast<const ar::StepTable*>(table_buf.ptr);
  kg.main_queue = static_cast<const uint32_t*>(queue_buf.ptr);
  kg.active_pixels = static_cast<uint32_t*>(list_buf.ptr);
  kg.film = static_cast<ar::FilmData*>(film_buf.ptr);
  kg.tables = static_cast<const ar::SceneTables*>(tables_buf.ptr);
  kg.lights = static_cast<const ar::LightRecord*>(lights_buf.ptr);
  kg.light_sampler = static_cast<const ar::LightSamplerView*>(sampler_buf.ptr);
  kg.world_light = 1;  // [decoy, world, decoy]
  if (program != nullptr) kg.scene = static_cast<const ar::SceneData*>(program_bufs.back().ptr);
  kg.session.width = kFilmW;
  kg.session.height = kFilmH;
  kg.session.samples_per_pass = kK;
  DeviceBuffer kg_buf = put(&kg, sizeof kg);

  if (gpu != nullptr) {
    const uint32_t module = gpu->load_module_file(std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/shade_background.cubin");
    const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_buf.ptr);
    void* args[] = {&kg_dev};
    gpu->launch_module_kernel(module, "k_shade_background", (kN + 255) / 256, 256, args);
  } else {
    // On the CPU device the uploaded copies are host memory the stage reads directly.
    const ar::KernelGlobals host_kg = kg;
    cpu_stage_shade_background(host_kg, step, 0, kN);
  }
  dev.synchronize();

  Snapshot s;
  s.slot_L.resize(size_t(kN) * 4);
  s.slot_flags.resize(kN);
  s.next_stage.resize(kN);
  s.term.resize(kN);
  const ar::FilmData& fd = film.data();
  const auto offset = [&](const void* p) { return uint64_t(static_cast<const char*>(p) - static_cast<const char*>(film.buffer().ptr)); };
  dev.download(s.slot_L.data(), film.buffer(), offset(fd.slot_L), s.slot_L.size() * sizeof(float));
  dev.download(s.slot_flags.data(), film.buffer(), offset(fd.slot_flags), kN * sizeof(uint32_t));
  dev.download(s.next_stage.data(), next_buf, 0, kN);
  dev.download(s.term.data(), term_buf, 0, kN);
  // The COUNT planes are the contiguous region from fd.count; the ADD plane of the bounce sum is exact.
  const uint32_t n_count_planes = uint32_t(fd.term_hist) + ar::kTermCount;
  s.counts.resize(size_t(n_count_planes) * fd.plane_stride);
  dev.download(s.counts.data(), film.buffer(), offset(fd.count), s.counts.size() * sizeof(uint32_t));
  s.adds.resize(size_t(uint32_t(fd.bounce_sum) + 1u) * fd.plane_stride);
  dev.download(s.adds.data(), film.buffer(), offset(fd.add), s.adds.size() * sizeof(float));

  for (DeviceBuffer* b : {&film_buf, &queue_buf, &list_buf, &table_buf, &next_buf, &term_buf, &flags_buf, &bounces_buf,
                          &lambda_buf, &beta_buf, &ru_buf, &rl_buf, &ro_buf, &rd_buf, &png_buf, &pns_buf, &pd_buf, &pmf_buf, &sample_buf, &soa_buf, &lights_buf, &sampler_buf, &illum_buf, &cmf_buf, &tables_buf,
                          &kg_buf, &rtype_buf})
    dev.free(*b);
  for (DeviceBuffer& b : program_bufs) dev.free(b);
  return s;
}

// The two devices' runs against each other: integer fields bit for bit, radiance by D12's evaluation
// class. Returns the radiance statistics.
// `gate_radiance` false only for the colour environment (ruling 96): the radiance statistics are printed,
// not judged.
agree::Stats check_agreement(const Snapshot& c, const Snapshot& g, const char* step, bool gate_radiance = true) {
  // Integer fields: bitwise.
  CHECK(c.term == g.term);
  CHECK(c.next_stage == g.next_stage);
  CHECK(c.slot_flags == g.slot_flags);
  CHECK(c.counts == g.counts);
  CHECK(c.adds == g.adds);
  // The fixture exercises both outcomes and every path ended.
  uint32_t escaped = 0, invalid = 0, ended = 0;
  for (uint32_t i = 0; i < kN; ++i) {
    escaped += c.term[i] == uint8_t(ar::Term::ESCAPED);
    invalid += c.term[i] == uint8_t(ar::Term::INVALID);
    ended += c.next_stage[i] == uint8_t(ar::Stage::TERMINATED);
  }
  CHECK(ended == kN);
  CHECK(invalid > 500u);
  CHECK(escaped + invalid == kN);
  // An invalid sample's slot is NaN on both devices (sample_mark_invalid): the NaN positions must match
  // exactly, and are zeroed before the finite comparison below.
  std::vector<float> lc = c.slot_L, lg = g.slot_L;
  uint32_t nan_lanes = 0, nan_mismatch = 0;
  for (size_t i = 0; i < lc.size(); ++i) {
    const bool a = !(lc[i] == lc[i]), b = !(lg[i] == lg[i]);
    nan_lanes += a;
    nan_mismatch += a != b;
    if (a) lc[i] = 0.0f;
    if (b) lg[i] = 0.0f;
  }
  CHECK(nan_mismatch == 0u);
  CHECK(nan_lanes == 4u * invalid);
  double total = 0.0;
  for (float v : lc) total += double(v);
  CHECK(total > 1.0e4);  // radiance really was added, so agreement is not two zeros

  // Radiance (4 lanes per path): the evaluation class.
  agree::Results rc, rg;
  rc.n = rg.n = kN;
  rc.stride = rg.stride = 4;
  rc.v.assign(lc.begin(), lc.end());
  rg.v.assign(lg.begin(), lg.end());
  const agree::Stats e = agree::compare(rg, rc, agree::Metric::REL_L2);
  const bool pass = agree::report("shade_background", step, e, agree::Cls::EVAL, agree::NOFMA_VS_CPU_FLOAT).pass;
  if (gate_radiance) CHECK(pass);
  return e;
}


}  // namespace

TEST_SUITE("gate") {

TEST_CASE("shade_background/agreement") {
  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const Snapshot c = run(cpu, nullptr);
  const Snapshot g = run(*gpu, gpu.get());
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);
  check_agreement(c, g, "slot radiance nofma vs cpu float");
}

// The program world: 10^5 random directions through the Environment Texture program on both devices,
// Linear on a smooth grey float image (a grey's lift is analytic, so only the mapping and the lookup
// are compared). Closest is not run: a direction within 1e-6 of a texel edge may pick the neighbour on
// one device, which is the lookup's conditioning, not a defect.
TEST_CASE("shade_background/environment agreement") {
  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const ProgramWorld world(1u, true);
  REQUIRE(!world.world.magenta);
  REQUIRE(world.world.program.size() > 2);
  const Snapshot c = run(cpu, nullptr, &world);
  const Snapshot g = run(*gpu, gpu.get(), &world);
  const agree::Stats e = check_agreement(c, g, "environment linear nofma vs cpu float");
  // 1e-6 relative on the 4-lane radiance of a random direction, tighter than 0002 D12's 1e-5.
  CHECK(e.max <= 1e-6);
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);
}

// The same lookup on saturated colours, before the lift: the colour TEX_ENVIRONMENT returns for 10^5
// random directions, no-FMA GPU against CPU float, at 1e-6: the direction mapping and the bilinear
// lookup alone. (After the lift the same image differs by up to 1.4e-5: the sigmoid polynomial sums terms of 20 to 50 to an argument near 2, so it magnifies a 4e-7 input gap about 40 times.)
TEST_CASE("shade_background/environment colour lookup agreement") {
  const ProgramWorld world(1u, false);
  Ledger ledger;
  auto dev = make_cuda_device(ledger, 0);
  agree::Inputs in;
  in.n = kN, in.stride = 3;
  in.v.resize(size_t(kN) * 3);
  for (uint32_t i = 0; i < kN; ++i) {
    const float z = 2.0f * float(agree::unit(i, 30)) - 1.0f, phi = 6.2831853f * float(agree::unit(i, 31));
    const float r = std::sqrt(1.0f - z * z);
    in.v[3 * i] = r * std::cos(phi), in.v[3 * i + 1] = r * std::sin(phi), in.v[3 * i + 2] = z;
  }
  DeviceBuffer texels = dev->allocate(LedgerCategory::TEXTURES, world.texels.size() * 4, Owner{});
  dev->upload(texels, 0, world.texels.data(), world.texels.size() * 4);
  ar::DeviceImageView view{};
  view.width = kEnvW, view.height = kEnvH, view.channels = 4;
  view.format = uint32_t(ar::TexFormat::FLOAT), view.encoding = uint32_t(ar::TexEncoding::LINEAR);
  ar::DeviceImageView host_view = view;
  host_view.texels = uint64_t(uintptr_t(world.texels.data()));
  view.texels = uint64_t(uintptr_t(texels.ptr));
  DeviceBuffer vb = dev->allocate(LedgerCategory::TABLES, sizeof view, Owner{});
  dev->upload(vb, 0, &view, sizeof view);
  DeviceBuffer sb = dev->allocate(LedgerCategory::TABLES, world.srgb.size() * 4, Owner{});
  dev->upload(sb, 0, world.srgb.data(), world.srgb.size() * 4);
  uint64_t views_p = uint64_t(uintptr_t(vb.ptr)), srgb_p = uint64_t(uintptr_t(sb.ptr));
  uint32_t count = 1, mode = 1;
  const agree::Results gpu = agree::run_gpu(*dev, std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/tex_agree.cubin", "ar_env_agree", in, 3,
                                            {&views_p, &count, &srgb_p, &mode});
  ar::SceneTables tables{};
  tables.images = &host_view;
  tables.image_count = 1;
  tables.srgb_decode = world.srgb.data();
  agree::Results cpu;
  cpu.n = kN, cpu.stride = 3;
  cpu.v.resize(size_t(kN) * 3);
  for (uint32_t i = 0; i < kN; ++i) {
    float out[3];
    ar::op_tex_environment(tables, 0u, mode, &in.v[3 * i], out);
    for (int k = 0; k < 3; ++k) cpu.v[3 * i + uint32_t(k)] = out[k];
  }
  const agree::Stats e = agree::compare(gpu, cpu, agree::Metric::REL_L2);
  std::printf("agree shade_background environment pre-lift colour: n=%u max=%.3e p99.9=%.3e\n", e.n, e.max, e.p999);
  CHECK(e.max <= 1e-6);
  dev->free(texels);
  dev->free(vb);
  dev->free(sb);
  CHECK(ledger.total() == 0);
}

// Saturated colours through the whole program, lift included: reported, never judged (ruling 96). The
// rgb2spec sigmoid argument c0 l^2 + c1 l + c2 cancels about 29x (terms near -32, +50 and -19 sum to
// about -1.7), so a one-ulp libm difference in the texel position, 4e-7 in the colour, becomes up to
// 2e-5 in the radiance. The parts are gated instead: the pre-lift colour above at 1e-6, the lift in
// test_vm_agree (bit for bit on identical inputs) and the grey radiance case at 1e-6. This line keeps a
// jump in the combined figure visible in the log.
TEST_CASE("shade_background/environment colour radiance is reported") {
  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const ProgramWorld world(1u, false);
  REQUIRE(!world.world.magenta);
  const Snapshot c = run(cpu, nullptr, &world);
  const Snapshot g = run(*gpu, gpu.get(), &world);
  const agree::Stats e = check_agreement(c, g, "environment colour linear nofma vs cpu float (reported, ruling 96)", false);
  CHECK(std::isfinite(e.max));  // the integer fields and the NaN positions still agree exactly
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);
}

}  // TEST_SUITE

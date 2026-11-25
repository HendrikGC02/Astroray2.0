// SPDX-License-Identifier: Apache-2.0
// bake_environment (0004 D11): the cell function against an independent host evaluation, the
// kernel through the CPU device on a world program (an Environment Texture), the host baker
// (normalisation, cache, refusals), and the scene table build that bakes before it builds the light
// table (ENV_TABLE, the bake key, a CPU context, a refused grid).
// Ref: docs/decisions/0004-integrator.md D10 "Importance weighting", D11; 0005 item 32; kernel/stages/
// bake_environment.h (the error bound); pbrt-v4 ImageInfiniteLight (the grid per cell); Cycles
// kernel/camera/projection.h direction_to_equirectangular (the lat-long convention the host check
// inverts, as test_shade_background's env_direction).
//
// Tolerance against the host. The kernel sums the world over the 471 observer samples with
// Neumaier's compensated sum in float, then divides by the weight total (bake_environment.h). With
// u = 2^-24 and e_s the callable's relative error, the cell differs from an exact evaluation of
// the same sum by at most (e_s + 8) u, because every term is positive: the product s c costs
// 2 u (c = (x + y) + z in float) + u, the two compensated sums u each, the weight total 3 u and
// the division u; the compensation's second-order part gamma_{n-1}^2 sum |x| (Neumaier 1974, n = 471
// terms, at most 7.9e-10 of the sum, twice) is below the margin and is not counted. The analytic callable (common/analytic_world.h) has e_s = 8 u, counted
// there. The host side is double throughout and takes the direction the kernel computes (the
// mapping's own accuracy is test_env_table's), so no term of the direction enters. A plain float
// sum would add up to (118 + 3) u here; the tolerance does not allow for it, which is the point of
// the compensation. A direction-independent program world (a 1 x 1 image, Closest) radiates
// bit for bit what its folded constant record does (test_shade_background "program world equals
// folded"), so it has the constant world's e_s = u (the float strength product; the spectrum is the
// same lift on both sides): 9 u = 5.4e-7, inside the 1e-6 the task states.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/analytic_world.h"
#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/image/image_store.h"
#include "host/light/env_bake.h"
#include "host/light/env_table.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/generation.h"
#include "host/scene/mesh.h"
#include "host/scene/scene_tables.h"
#include "host/scene/world.h"
#include "host/thread_pool.h"
#include "stages/bake_environment.h"
#include "vm/ops.h"

using namespace astroray::host;
using ar::Real;
using ar::Real3;

namespace {

constexpr double kU = 1.0 / 16777216.0;  // float unit roundoff 2^-24
constexpr double kReductionUnits = 8.0;  // bake_environment.h: the sums, the weights and the division
constexpr double kAnalyticUnits = ar::kAnalyticWorldUnits;  // e_s, counted in common/analytic_world.h
constexpr uint32_t kN = 64;              // tests use 64; production uses kEnvBakeSize
// The cases that only read values, never directions, bake 32 x 32 to keep the fast tier inside B-08.
constexpr uint32_t kSmall = 32;

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

// The world colour of the uniform worlds, linear Rec.709.
constexpr float kRgb[3] = {1.0f, 0.5f, 0.25f};

// A CPU device that can be told to fail its next kernel launch, as a device error does mid-bake.
class FaultyCpu : public CpuDevice {
 public:
  using CpuDevice::CpuDevice;
  using CpuDevice::launch;
  void launch(KernelId kernel, uint32_t count) override {
    if (fail_launch) throw DeviceError("injected launch failure");
    CpuDevice::launch(kernel, count);
  }
  bool fail_launch = false;
};

// BAKE_ENVIRONMENT's slot for a test that wants a grid that varies over the sphere: the cell
// function of the production kernel on the analytic world instead of the world program.
void cpu_bake_analytic(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  for (uint32_t cell = first; cell < last; ++cell)
    kg.env_bake.grid[cell] =
        float(ar::bake_environment_cell(ar::AnalyticWorld{}, kg.tables->observer, kg.env_bake.n, cell));
}

// LOAD_CTX_P -> TEX_ENVIRONMENT (image `key`, `mode`: 0 Closest, 1 Linear) -> EMIT_LE with a constant
// strength: an Environment Texture on a Background, as the addon lowers it.
StoredGraph environment_graph(int32_t key, int32_t mode, float strength) {
  StoredGraph g;
  g.f32 = {strength};
  g.i32 = {key, mode};
  g.nodes.assign(4, ar_graph_node{});
  g.nodes[0].op = uint32_t(ar::Op::LOAD_CTX_P);
  g.nodes[1].op = uint32_t(ar::Op::TEX_ENVIRONMENT), g.nodes[1].i32_first = 0, g.nodes[1].i32_count = 2;
  g.nodes[2].op = uint32_t(ar::Op::CONST_F32), g.nodes[2].f32_first = 0, g.nodes[2].f32_count = 1;
  g.nodes[3].op = uint32_t(ar::Op::EMIT_LE);
  g.links = {{0u, 0u, 1u, 0u}, {1u, 0u, 3u, 0u}, {2u, 0u, 3u, 1u}};
  return g;
}

// A CPU device whose KernelGlobals name a world program over one float RGBA image (rows bottom-up),
// with the observer, rgb2spec and sRGB tables, all host memory the CPU device reads in place.
// `analytic` binds cpu_bake_analytic to BAKE_ENVIRONMENT instead of the production kernel.
struct Rig {
  Ledger ledger;
  astroray::ContextState ctx;
  ThreadPool pool{ctx, 4};  // hosted ci has 2-4 cores
  FaultyCpu dev{ledger, pool};
  Owner owner{1, 0};
  MaterialStore store{std::make_shared<Ledger>()};
  std::array<float, ar::kSrgbTableSize> srgb{};
  std::vector<float> texels;
  ar::DeviceImageView view{};
  ar::SceneTables tables{};
  ar::SceneData scene{};
  WorldOutput world;
  DeviceBuffer kg;
  explicit Rig(bool analytic = false) {
    if (analytic)
      dev.register_kernel(KernelId::BAKE_ENVIRONMENT, &cpu_bake_analytic);
    else
      dev.register_production_stages();
    kg = dev.allocate(LedgerCategory::TABLES, sizeof(ar::KernelGlobals), owner);
    ar::srgb_decode_table(srgb.data());
    tables.rgb2spec = &store.table();
    tables.observer = observer();
    tables.images = &view;
    tables.image_count = 1;
    tables.srgb_decode = srgb.data();
    set_uniform(1.0f);
  }
  ~Rig() { dev.free(kg); }
  // The world of image (w, h, rgba) at `strength`, compiled and bound.
  void set_world(uint32_t w, uint32_t h, std::vector<float> rgba, uint32_t mode, float strength) {
    texels = std::move(rgba);
    view = ar::DeviceImageView{};
    view.texels = uint64_t(uintptr_t(texels.data()));
    view.width = w, view.height = h, view.channels = 4, view.format = uint32_t(ar::TexFormat::FLOAT);
    view.encoding = uint32_t(ar::TexEncoding::LINEAR);
    const StoredGraph g = environment_graph(7, int32_t(mode), strength);
    const ImageSlotFn slots = [](uint32_t key, uint32_t* slot) { return key == 7u ? (*slot = 0u, true) : false; };
    const ImageMeanFn means = [](uint32_t, std::array<float, 3>* rgb) { return (*rgb = {1.0f, 1.0f, 1.0f}, true); };
    ar_world_desc desc = {};
    desc.surface_graph = 1;
    world = compile_world(desc, &g, store.table(), &means, &slots);
    REQUIRE_MESSAGE(!world.magenta, "the environment world compiled to magenta: "
                                        << (world.entries.empty() ? std::string() : world.entries[0].detail));
    REQUIRE(world.program.size() > 2);
    scene = ar::SceneData{};
    scene.world_program = world.program.data();
    scene.world_program_words = uint32_t(world.program.size());
    scene.world_constants = &world.block;
    bind(true);
  }
  // A world of one colour everywhere: a 1 x 1 image of kRgb, Closest.
  void set_uniform(float strength) { set_world(1, 1, {kRgb[0], kRgb[1], kRgb[2], 1.0f}, 0u, strength); }
  // `program` false binds KernelGlobals without a scene, as a host bug that launched a bake for a
  // world without a program would.
  void bind(bool program) {
    ar::KernelGlobals g{};
    g.scene = program ? &scene : nullptr;
    g.tables = &tables;
    dev.upload(kg, 0, &g, sizeof g);
    dev.set_kernel_globals(kg);
  }
  // display_energy of the uniform world's colour at `strength`: what every cell must hold.
  double uniform_energy(float strength) const {
    ar::SpectrumRef ref;
    uint32_t flags = 0;
    ar::make_rgb_ref(store.table(), ar::Rgb{{Real(kRgb[0]), Real(kRgb[1]), Real(kRgb[2])}}, ar::SpectrumSemantic::ILLUMINANT,
                     &ref, &flags);
    return display_energy({{ref, strength}}, observer());
  }
  // The raw n x n grid of the production kernel, as the baker would read it.
  std::vector<float> raw_grid(uint32_t n) {
    DeviceBuffer grid = dev.allocate(LedgerCategory::TABLES, uint64_t(n) * n * sizeof(float), owner);
    const ar::EnvBake b{static_cast<float*>(grid.ptr), n};
    dev.upload(kg, offsetof(ar::KernelGlobals, env_bake), &b, sizeof b);
    dev.launch(KernelId::BAKE_ENVIRONMENT, n * n);
    std::vector<float> out(size_t(n) * n);
    dev.download(out.data(), grid, 0, out.size() * sizeof(float));
    dev.free(grid);
    return out;
  }
};

// The equirectangular (u, v) of a unit direction, Cycles' direction_to_equirectangular with its
// default range in double: u = 0.5 - atan2(y, x) / (2 pi), v = 1 - acos(z) / pi (v = 1 straight up).
std::array<double, 2> equirect_uv(double x, double y, double z) {
  const double pi = 3.14159265358979323846;
  return {0.5 - std::atan2(y, x) / (2.0 * pi), 1.0 - std::acos(std::max(-1.0, std::min(1.0, z))) / pi};
}

// ---- a scene, for "which edits touch the world" and the scene table build ------------------------

struct GB {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  std::vector<int32_t> i;
  int add(ar::Op op, std::vector<float> fp = {}, std::vector<int32_t> ip = {}) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size()), n.f32_count = uint32_t(fp.size());
    n.i32_first = uint32_t(i.size()), n.i32_count = uint32_t(ip.size());
    f.insert(f.end(), fp.begin(), fp.end());
    i.insert(i.end(), ip.begin(), ip.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size()), d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data(), d.params_i32 = i.data();
    return d;
  }
};

GB constant_emission(float strength) {
  GB g;
  const int color = g.add(ar::Op::CONST_F32X3, {1.0f, 0.5f, 0.25f});
  const int power = g.add(ar::Op::CONST_F32, {strength});
  const int e = g.add(ar::Op::EMIT_LE);
  g.links.push_back({uint32_t(color), 0u, uint32_t(e), 0u});
  g.links.push_back({uint32_t(power), 0u, uint32_t(e), 1u});
  return g;
}

constexpr ar_key kWorldGraph = 40, kImage = 5, kEmitter = 60, kEmitterMesh = 100;

// A CPU context (no scene device): the scene table build bakes on the cache's own CPU device.
struct Fixture {
  astroray::ContextState ctx;
  Scene scene{ctx, SceneConfig{2, {}}};
  Fixture() {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
    scene.table_cache().bake_size = kSmall;
  }
  // The world: graph kWorldGraph an Environment Texture of image kImage (Closest) at `strength`.
  void set_world(Edit& e, float strength) {
    const StoredGraph g = environment_graph(int32_t(kImage), 0, strength);
    const ar_graph_desc d = g.desc();
    REQUIRE(scene.materials->set_graph(e, kWorldGraph, &d) == AR_OK);
    ar_world_desc w = {};
    w.surface_graph = kWorldGraph;
    REQUIRE(scene.materials->set_world(e, &w) == AR_OK);
  }
  // Image kImage: 2 x 1 linear float RGBA pixels.
  void set_image(Edit& e, const std::vector<float>& rgba) {
    ar_image_desc d = {};
    d.key = kImage, d.source = AR_IMAGE_SOURCE_PIXELS, d.color_interop = AR_COLOR_INTEROP_LIN_REC709, d.width = 2, d.height = 1;
    ar_image_buffers b = {};
    REQUIRE(scene.materials->image_begin(e, &d, &b) == AR_OK);
    REQUIRE(b.pixels_count == rgba.size());
    std::memcpy(*b.pixels, rgba.data(), rgba.size() * sizeof(float));
    REQUIRE(e.item_commit(kImage) == AR_OK);
  }
  // PROGRAM_REJECTED entries on the world graph.
  uint32_t rejected() {
    Report& r = scene.report();
    r.snapshot();
    ar_report_info info{};
    REQUIRE(r.info(&info) == AR_OK);
    uint32_t n = 0;
    for (uint32_t i = 0; i < info.entry_count; ++i) {
      ar_report_entry e{};
      REQUIRE(r.entry(i, &e) == AR_OK);
      n += e.code == AR_REPORT_PROGRAM_REJECTED && e.subject == kWorldGraph;
    }
    return n;
  }
  // A quad, mesh kEmitterMesh, at height z.
  void set_quad(Edit& e, float z) {
    const std::vector<float> pos{0, 0, z, 1, 0, z, 1, 1, z, 0, 1, z};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    ar_mesh_desc d = {};
    d.key = kEmitterMesh, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1;
    ar_mesh_buffers b = {};
    REQUIRE(mesh_begin(scene, e, &d, &b) == AR_OK);
    std::memcpy(*b.positions, pos.data(), pos.size() * 4);
    std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
    std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
    for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
    REQUIRE(e.item_commit(kEmitterMesh) == AR_OK);
  }
  // Material kEmitter: a constant Emission at `strength`, sampled from the front.
  void set_emitter(Edit& e, float strength) {
    const GB g = constant_emission(strength);
    const ar_graph_desc d = g.desc();
    REQUIRE(scene.materials->set_graph(e, kEmitter + 1, &d) == AR_OK);
    ar_material_desc m = {};
    m.surface_graph = kEmitter + 1, m.emission_sampling = AR_EMISSION_SAMPLING_FRONT;
    REQUIRE(scene.materials->set_material(e, kEmitter, &m) == AR_OK);
  }
  // The quad instanced once with kEmitter.
  void set_instance(Edit& e) {
    std::vector<ar_key> geom{kEmitterMesh}, object{1}, pool{kEmitter};
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
    REQUIRE(set_instances(e, &d) == AR_OK);
  }
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
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("bake_environment/analytic against host") {
  const ar::ObserverView& obs = observer();
  const double tol = (kAnalyticUnits + kReductionUnits) * kU;
  double worst = 0.0;
  for (uint32_t cell = 0; cell < kN * kN; ++cell) {
    const double got = double(ar::bake_environment_cell(ar::AnalyticWorld{}, obs, kN, cell));
    const double want = ar::analytic_cell_reference(obs, kN, cell);
    REQUIRE(want > 0.0);
    worst = std::max(worst, std::fabs(got - want) / want);
  }
  std::printf("bake_environment analytic: worst relative error %.3e, bound %.3e (%g u, u = 2^-24)\n", worst, tol,
              kAnalyticUnits + kReductionUnits);
  CHECK(worst <= tol);
  // The function has a spread the bound does not hide behind: the grid is not constant.
  CHECK(ar::analytic_cell_reference(obs, kN, 0) != doctest::Approx(ar::analytic_cell_reference(obs, kN, kN * kN / 2 + 5)).epsilon(0.01));
}

TEST_CASE("bake_environment/constant world uniform") {
  Rig rig;
  const double want = rig.uniform_energy(1.0f);
  REQUIRE(want > 0.0);
  const std::vector<float> grid = rig.raw_grid(kSmall);
  // Bit-equal: every cell is the same reduction over the same inputs.
  uint32_t different = 0;
  for (float v : grid) different += std::memcmp(&v, &grid[0], sizeof(float)) != 0;
  CHECK(different == 0u);
  CHECK(std::fabs(double(grid[0]) - want) <= 1e-6 * want);

  EnvironmentBaker baker;
  const BakedEnvironment b = baker.get(rig.dev, rig.kg, {1}, nullptr, kSmall);
  REQUIRE(b.table);
  CHECK(b.max == double(grid[0]));
  CHECK(b.mean == double(grid[0]));  // 1024 equal floats sum exactly in double
  CHECK(std::fabs(b.max - want) <= 1e-6 * want);
  CHECK(std::fabs(b.mean - want) <= 1e-6 * want);
  // The table is a density: uniform, 1 in every cell, the integral 1, for both distributions.
  CHECK(b.table->n == kSmall);
  for (const EnvDistribution* d : {&b.table->plain, &b.table->compensated}) {
    CHECK(d->func_integral == 1.0f);
    CHECK(std::all_of(d->func.begin(), d->func.end(), [](float f) { return f == 1.0f; }));
  }
  // The kernel left the KernelGlobals as it found them.
  ar::KernelGlobals back{};
  rig.dev.download(&back, rig.kg, 0, sizeof back);
  CHECK(back.env_bake.grid == nullptr);
  CHECK(back.env_bake.n == 0u);
  CHECK(rig.ledger.total() == rig.kg.bytes);
}

TEST_CASE("bake_environment/program world cells") {
  // One bright texel in a dim 64 x 32 map whose every texel is its own colour (Closest). Every cell
  // whose centre direction is clear of a texel edge holds the display energy of the texel the
  // lat-long convention puts there, evaluated on the host from the texel's RGB and the strength;
  // the brightest cell lies in the bright texel.
  constexpr uint32_t W = 64, H = 32, ix0 = 40, iy0 = 22;
  const float strength = 0.75f;
  const auto texel = [](uint32_t ix, uint32_t iy) -> std::array<float, 3> {
    if (ix == ix0 && iy == iy0) return {6.0f, 3.0f, 1.5f};
    return {0.05f + 0.1f * float(ix) / float(W), 0.05f + 0.1f * float(iy) / float(H), 0.08f};
  };
  std::vector<float> px(size_t(W) * H * 4, 1.0f);
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x)
      for (int k = 0; k < 3; ++k) px[(size_t(y) * W + x) * 4 + size_t(k)] = texel(x, y)[size_t(k)];
  Rig rig;
  rig.set_world(W, H, px, 0u, strength);
  const std::vector<float> grid = rig.raw_grid(kN);
  double worst = 0.0;
  uint32_t checked = 0, skipped = 0, brightest = 0;
  for (uint32_t cell = 0; cell < kN * kN; ++cell) {
    if (grid[cell] > grid[brightest]) brightest = cell;
    const Real3 d = ar::bake_environment_direction(kN, cell);
    const std::array<double, 2> uv = equirect_uv(double(d.x), double(d.y), double(d.z));
    const double fu = uv[0] * W, fv = uv[1] * H;
    // A centre within 1e-3 texel of an edge may round to either texel in float: not a texel test.
    const double eu = std::fabs(fu - std::round(fu)), ev = std::fabs(fv - std::round(fv));
    if (eu < 1e-3 || ev < 1e-3 || fv >= H - 1e-3) {
      ++skipped;
      continue;
    }
    const uint32_t ix = uint32_t(fu) % W, iy = std::min(uint32_t(fv), H - 1);
    const std::array<float, 3> c = texel(ix, iy);
    ar::SpectrumRef ref;
    uint32_t flags = 0;
    ar::make_rgb_ref(rig.store.table(), ar::Rgb{{Real(c[0]), Real(c[1]), Real(c[2])}}, ar::SpectrumSemantic::ILLUMINANT, &ref,
                     &flags);
    const double want = display_energy({{ref, strength}}, observer());
    worst = std::max(worst, std::fabs(double(grid[cell]) - want) / want);
    ++checked;
  }
  std::printf("bake_environment program world: %u cells checked, %u at texel edges, worst relative error %.3e\n", checked,
              skipped, worst);
  CHECK(checked > kN * kN * 3 / 4);
  CHECK(worst <= 1e-5);
  const Real3 db = ar::bake_environment_direction(kN, brightest);
  const std::array<double, 2> uvb = equirect_uv(double(db.x), double(db.y), double(db.z));
  const double pi = 3.14159265358979323846;
  std::printf("bake_environment program world: brightest cell %u at (%.4f, %.4f, %.4f), texel (%.3f, %.3f); bright texel "
              "centre lat %.4f lon %.4f rad\n",
              brightest, double(db.x), double(db.y), double(db.z), uvb[0] * W, uvb[1] * H, pi * ((double(iy0) + 0.5) / H - 0.5),
              pi - 2.0 * pi * (double(ix0) + 0.5) / W);
  CHECK(uint32_t(uvb[0] * W) == ix0);
  CHECK(uint32_t(uvb[1] * H) == iy0);
  // ASTRORAY_EVIDENCE names a directory: the grid goes there as a PFM for a look (rows bottom-up).
  char* dir = nullptr;
  size_t len = 0;
  if (_dupenv_s(&dir, &len, "ASTRORAY_EVIDENCE") == 0 && dir != nullptr) {
    const std::string path = std::string(dir) + "/bake_grid.pfm";
    std::free(dir);
    std::ofstream f(path, std::ios::binary);
    f << "Pf\n" << kN << " " << kN << "\n-1.0\n";
    f.write(reinterpret_cast<const char*>(grid.data()), std::streamsize(grid.size() * sizeof(float)));
  }
}

TEST_CASE("bake_environment/tiny uniform world builds") {
  // BACKLOG 55: build_env_table throws on a uniform grid whose integral is below FLT_MIN. A baked
  // grid holds floats, so for equal cells the double mean is exact and the older ulp-residual
  // mechanism (a mean that rounds below the value) cannot arise; what remains is a world whose
  // radiance is itself below FLT_MIN (strength 1e-39 gives cells near 6e-40). The baker divides
  // by the maximum, so both worlds build, as the density 1.
  Rig rig;
  for (float strength : {1e-30f, 1e-39f}) {
    CAPTURE(strength);
    rig.set_uniform(strength);
    const std::vector<float> grid = rig.raw_grid(kSmall);
    REQUIRE(grid[0] > 0.0f);
    if (strength < 1e-38f) {
      // The un-normalised grid is refused (the case would prove nothing if it were not).
      CHECK_THROWS_AS(build_env_table(std::vector<double>(grid.begin(), grid.end()), kSmall), std::invalid_argument);
    }
    EnvironmentBaker baker;
    BakedEnvironment b;
    REQUIRE_NOTHROW(b = baker.get(rig.dev, rig.kg, {1}, nullptr, kSmall));
    REQUIRE(b.table);
    CHECK(b.max == double(grid[0]));
    CHECK(std::all_of(b.table->plain.func.begin(), b.table->plain.func.end(), [](float f) { return f == 1.0f; }));
    CHECK(std::all_of(b.table->compensated.func.begin(), b.table->compensated.func.end(),
                      [](float f) { return f == 1.0f; }));
  }
}

TEST_CASE("bake_environment/black world gives no table") {
  Rig rig;
  EnvironmentBaker baker;
  rig.set_uniform(0.0f);
  BakedEnvironment b = baker.get(rig.dev, rig.kg, {1}, nullptr, kSmall);
  CHECK(b.table == nullptr);
  CHECK(b.max == 0.0);
  CHECK(b.mean == 0.0);
  CHECK(baker.rebuild_count() == 1u);
  // A launch without a world program is a host bug: its cells are NaN and the grid is refused,
  // never read as a black world (rule 5).
  rig.bind(false);
  CHECK_THROWS_AS(baker.get(rig.dev, rig.kg, {2}, nullptr, kSmall), BakeRefused);
  CHECK(baker.rebuild_count() == 1u);
}

TEST_CASE("bake_environment/non-finite cell is refused") {
  Rig rig;
  EnvironmentBaker baker;
  // The compiler refuses a negative or non-finite strength (EMIT_LE), so a world reaches the bake with
  // a non-finite radiance only by overflow: 1e38 times a texel of 1000 is inf. A NaN cell (a program
  // that cannot run) is "black world gives no table"'s second half.
  for (float bad : {1e38f}) {
    rig.set_world(1, 1, {1000.0f, 1000.0f, 1000.0f, 1.0f}, 0u, bad);
    std::string what;
    try {
      baker.get(rig.dev, rig.kg, {1}, nullptr, kSmall);
    } catch (const BakeRefused& e) {
      what = e.what();
    }
    CAPTURE(bad);
    CHECK(what.find("bake_environment: cell 0 (iu 0, iv 0) at direction (") != std::string::npos);
    CHECK(what.find("not a finite non-negative radiance") != std::string::npos);
    // Cell 0 of 32 x 32 is centred at (1/64, 1/64), near the south pole (z = -0.996).
    const size_t at = what.find("direction (");
    REQUIRE(at != std::string::npos);
    char* end = nullptr;
    const double x = std::strtod(what.c_str() + at + 11, &end);
    const double y = std::strtod(end + 1, &end);
    const double z = std::strtod(end + 1, &end);
    CHECK(std::fabs(x * x + y * y + z * z - 1.0) < 1e-5);
    CHECK(z < -0.99);
  }
  // The refusals left no cache: a good world under the same key bakes.
  CHECK(baker.rebuild_count() == 0u);
  rig.set_uniform(2.0f);
  CHECK(baker.get(rig.dev, rig.kg, {1}, nullptr, kSmall).table != nullptr);
  CHECK(baker.rebuild_count() == 1u);
  CHECK_THROWS_AS(baker.get(rig.dev, rig.kg, {1}, nullptr, 0), std::invalid_argument);
}

TEST_CASE("bake_environment/world key follows the program and its images") {
  const ar_key G = 41, M = 50, MESH = 100;
  Fixture fx;
  const auto key = [&] {
    return EnvironmentBaker::world_key(fx.scene.materials->world().get(), fx.scene.materials->images().arrays().get());
  };
  // A constant world needs no bake: its key is empty.
  fx.commit([&](Edit& e) {
    const GB g = constant_emission(3.0f);
    const ar_graph_desc d = g.desc();
    ar_world_desc w = {};
    w.surface_graph = kWorldGraph;
    REQUIRE(fx.scene.materials->set_graph(e, kWorldGraph, &d) == AR_OK);
    REQUIRE(fx.scene.materials->set_world(e, &w) == AR_OK);
  });
  CHECK(key().empty());
  fx.commit([&](Edit& e) {
    fx.set_image(e, {1.0f, 0.5f, 0.25f, 1.0f, 0.2f, 0.4f, 0.6f, 1.0f});
    fx.set_world(e, 2.0f);
  });
  const std::vector<uint8_t> first = key();
  REQUIRE(!first.empty());
  // A camera, a material and a mesh leave it.
  fx.commit([&](Edit& e) {
    ar_camera_desc cam = {};
    cam.type = AR_CAMERA_PERSP;
    REQUIRE(e.set_camera(&cam) == AR_OK);
  });
  CHECK(key() == first);
  fx.commit([&](Edit& e) {
    const GB g = constant_emission(7.0f);
    const ar_graph_desc d = g.desc();
    ar_material_desc m = {};
    m.surface_graph = G;
    REQUIRE(fx.scene.materials->set_graph(e, G, &d) == AR_OK);
    REQUIRE(fx.scene.materials->set_material(e, M, &m) == AR_OK);
  });
  CHECK(key() == first);
  {
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    fx.commit([&](Edit& e) {
      ar_mesh_desc d = {};
      d.key = MESH, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(fx.scene, e, &d, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(MESH) == AR_OK);
    });
  }
  CHECK(key() == first);
  // The strength is part of the program; a new image under the same key and slot recompiles the
  // world word for word the same, and the key still moves.
  fx.commit([&](Edit& e) { fx.set_world(e, 4.0f); });
  const std::vector<uint8_t> second = key();
  CHECK(second != first);
  const std::vector<uint32_t> words = fx.scene.materials->world()->program;
  fx.commit([&](Edit& e) { fx.set_image(e, {0.1f, 0.1f, 0.1f, 1.0f, 0.9f, 0.9f, 0.9f, 1.0f}); });
  CHECK(fx.scene.materials->world()->program == words);
  CHECK(key() != second);
}

TEST_CASE("scene_tables/bake before light table") {
  // One bake and one light table per world edit, the table built after the bake with its result;
  // an edit that leaves the world builds neither.
  Fixture fx;
  fx.commit([&](Edit& e) {
    fx.set_image(e, {1.0f, 0.5f, 0.25f, 1.0f, 0.2f, 0.4f, 0.6f, 1.0f});
    fx.set_world(e, 2.0f);
  });
  EnvironmentBaker& baker = fx.scene.table_cache().baker;
  const auto set1 = acquire_scene_tables(fx.scene);
  REQUIRE(set1->lights);
  CHECK(baker.rebuild_count() == 1u);
  CHECK(fx.scene.light_rebuilds() == 1u);
  CHECK(set1->lights->env_baked);
  REQUIRE(bool(set1->lights->env));
  CHECK(set1->lights->env->n == kSmall);
  CHECK(set1->lights->env_max > set1->lights->env_mean);
  CHECK(set1->lights->env_mean > 0.0);
  CHECK(set1->lights->world_light != ar::kNoLight);
  CHECK(set1->counts[uint32_t(TableId::ENV_TABLE)] > 0u);
  // A camera edit: the same set, nothing baked or built.
  fx.commit([&](Edit& e) {
    ar_camera_desc cam = {};
    cam.type = AR_CAMERA_PERSP;
    REQUIRE(e.set_camera(&cam) == AR_OK);
  });
  CHECK(acquire_scene_tables(fx.scene) == set1);
  CHECK(baker.rebuild_count() == 1u);
  CHECK(fx.scene.light_rebuilds() == 1u);
  // The world's strength: one bake, one light table; the mean doubles.
  fx.commit([&](Edit& e) { fx.set_world(e, 4.0f); });
  const auto set2 = acquire_scene_tables(fx.scene);
  CHECK(baker.rebuild_count() == 2u);
  CHECK(fx.scene.light_rebuilds() == 2u);
  CHECK(set2->lights->env_mean == doctest::Approx(2.0 * set1->lights->env_mean).epsilon(1e-6));
  CHECK(set2->tables[uint32_t(TableId::ENV_TABLE)] != set1->tables[uint32_t(TableId::ENV_TABLE)]);
  // A new image: one bake, one light table, and the grid follows the image.
  fx.commit([&](Edit& e) { fx.set_image(e, {0.1f, 0.1f, 0.1f, 1.0f, 0.9f, 0.9f, 0.9f, 1.0f}); });
  const auto set3 = acquire_scene_tables(fx.scene);
  CHECK(baker.rebuild_count() == 3u);
  CHECK(fx.scene.light_rebuilds() == 3u);
  CHECK(set3->lights->env_mean != set2->lights->env_mean);
  // Scene::lights() alone bakes nothing and keeps the table that has the bake.
  CHECK(fx.scene.lights() == set3->lights);
  CHECK(baker.rebuild_count() == 3u);
}

TEST_CASE("scene_tables/a constant world builds its light table once") {
  // A world that needs no bake: the snapshot builds the light table, the scene table build does not
  // build it again, so each emitter edit costs exactly one light table.
  Fixture fx;
  fx.commit([&](Edit& e) {
    const GB g = constant_emission(3.0f);
    const ar_graph_desc d = g.desc();
    ar_world_desc w = {};
    w.surface_graph = kWorldGraph;
    REQUIRE(fx.scene.materials->set_graph(e, kWorldGraph, &d) == AR_OK);
    REQUIRE(fx.scene.materials->set_world(e, &w) == AR_OK);
    fx.set_emitter(e, 5.0f);
    fx.set_quad(e, 0.0f);
  });
  fx.commit([&](Edit& e) { fx.set_instance(e); });
  const uint64_t before = fx.scene.light_rebuilds();
  const auto set1 = acquire_scene_tables(fx.scene);
  CHECK(fx.scene.light_rebuilds() == before + 1u);
  REQUIRE(set1->lights);
  CHECK(set1->lights->world_light != ar::kNoLight);
  CHECK(set1->lights->lights.size() == 3u);  // two triangles and the world
  fx.commit([&](Edit& e) { fx.set_emitter(e, 50.0f); });
  const auto set2 = acquire_scene_tables(fx.scene);
  CHECK(set2->lights != set1->lights);
  CHECK(fx.scene.light_rebuilds() == before + 2u);
  CHECK(fx.scene.table_cache().baker.rebuild_count() == 0u);
}

TEST_CASE("scene_tables/an emitter mesh edit keeps the bake") {
  // A program world and an emissive instanced mesh: editing the mesh builds one new light table,
  // with the bake it already had (no new bake) and the very same ENV_TABLE copy.
  Fixture fx;
  fx.commit([&](Edit& e) {
    fx.set_image(e, {1.0f, 0.5f, 0.25f, 1.0f, 0.2f, 0.4f, 0.6f, 1.0f});
    fx.set_world(e, 2.0f);
    fx.set_emitter(e, 5.0f);
    fx.set_quad(e, 0.0f);
  });
  fx.commit([&](Edit& e) { fx.set_instance(e); });
  EnvironmentBaker& baker = fx.scene.table_cache().baker;
  const auto set1 = acquire_scene_tables(fx.scene);
  const uint64_t rebuilds = fx.scene.light_rebuilds();
  CHECK(baker.rebuild_count() == 1u);
  REQUIRE(bool(set1->lights->env));
  const auto env1 = set1->tables[uint32_t(TableId::ENV_TABLE)];
  REQUIRE(env1);
  REQUIRE(env1->ptr() != nullptr);
  fx.commit([&](Edit& e) { fx.set_quad(e, 0.5f); });
  const auto set2 = acquire_scene_tables(fx.scene);
  CHECK(set2->lights != set1->lights);
  CHECK(fx.scene.light_rebuilds() == rebuilds + 1u);
  CHECK(baker.rebuild_count() == 1u);
  CHECK(set2->lights->env == set1->lights->env);
  CHECK(set2->tables[uint32_t(TableId::ENV_TABLE)] == env1);
  CHECK(set2->tables[uint32_t(TableId::ENV_TABLE)]->ptr() == env1->ptr());
  CHECK(!set2->uploaded[uint32_t(TableId::ENV_TABLE)]);
}

TEST_CASE("scene_tables/cpu context bakes") {
  // No scene device: the cache's CPU device bakes, and ENV_TABLE holds both distributions of the
  // light table's bake, in env_table_floats' order.
  Fixture fx;
  REQUIRE(fx.scene.device() == nullptr);
  fx.commit([&](Edit& e) {
    fx.set_image(e, {3.0f, 0.5f, 0.25f, 1.0f, 0.2f, 0.4f, 0.6f, 1.0f});
    fx.set_world(e, 1.0f);
  });
  const auto set = acquire_scene_tables(fx.scene);
  CHECK(bool(fx.scene.table_cache().bake_device));
  REQUIRE(bool(set->lights->env));
  const EnvTable& t = *set->lights->env;
  std::vector<float> want;
  env_table_floats(t, &want);
  const uint64_t n = kSmall;
  CHECK(want.size() == 2 * (n * n + n * (n + 1) + n + (n + 1)));
  REQUIRE(set->counts[uint32_t(TableId::ENV_TABLE)] == want.size());
  const auto& table = set->tables[uint32_t(TableId::ENV_TABLE)];
  REQUIRE(table->bytes() == want.size() * 4);
  CHECK(std::memcmp(table->ptr(), want.data(), want.size() * 4) == 0);
  // The compensated half starts after the plain one, at an offset n alone gives.
  const float* f = static_cast<const float*>(table->ptr());
  const size_t half = size_t(n * n + n * (n + 1) + n + (n + 1));
  CHECK(std::memcmp(f, t.plain.func.data(), t.plain.func.size() * 4) == 0);
  CHECK(std::memcmp(f + half, t.compensated.func.data(), t.compensated.func.size() * 4) == 0);
  // The grid is the world's, not uniform: the red half of the image is the brighter.
  CHECK(set->lights->env_max > 1.2 * set->lights->env_mean);
}

TEST_CASE("scene_tables/refused grid renders black") {
  // A world whose radiance overflows to inf: the set is still built, the world light is gone (misses
  // render black, never the constant), and one PROGRAM_REJECTED entry names the world graph. A good
  // world under the same graph brings the light back and the entry goes.
  Fixture fx;
  fx.commit([&](Edit& e) {
    fx.set_image(e, {1000.0f, 1000.0f, 1000.0f, 1.0f, 1000.0f, 1000.0f, 1000.0f, 1.0f});
    fx.set_world(e, 1e38f);
  });
  std::shared_ptr<const SceneTableSet> set;
  REQUIRE_NOTHROW(set = acquire_scene_tables(fx.scene));
  REQUIRE(set->lights);
  CHECK(set->lights->world_light == ar::kNoLight);
  CHECK(!set->lights->env);
  CHECK(set->counts[uint32_t(TableId::ENV_TABLE)] == 0u);
  CHECK(fx.rejected() == 1u);
  // Latching again does not bake again or raise a second entry.
  CHECK(acquire_scene_tables(fx.scene) == set);
  CHECK(fx.rejected() == 1u);
  fx.commit([&](Edit& e) { fx.set_world(e, 1.0f); });
  const auto good = acquire_scene_tables(fx.scene);
  CHECK(good->lights->world_light != ar::kNoLight);
  CHECK(bool(good->lights->env));
  CHECK(fx.rejected() == 0u);
}

TEST_CASE("bake_environment/non-uniform max mean and table") {
  // A grid that varies over the sphere, through the baker: max, mean and the table against the
  // host's double evaluation of every cell. Cells carry the (e_s + 8) u bound of the header; the
  // table's entries are cell / max, so (2 (e_s + 8) + 1) u relative (the max, the cell, the float
  // store), and the compensated entries are absolute differences of values in [0, 1].
  const ar::ObserverView& obs = observer();
  const double tol = (kAnalyticUnits + kReductionUnits) * kU;
  Rig rig(true);
  EnvironmentBaker baker;
  const BakedEnvironment b = baker.get(rig.dev, rig.kg, {1}, nullptr, kN);
  REQUIRE(b.table != nullptr);
  std::vector<double> ref(size_t(kN) * kN);
  double max_ref = 0.0, sum_ref = 0.0;
  for (uint32_t i = 0; i < ref.size(); ++i) {
    ref[i] = ar::analytic_cell_reference(obs, kN, i);
    max_ref = std::max(max_ref, ref[i]);
    sum_ref += ref[i];
  }
  const double mean_ref = sum_ref / double(ref.size());
  CHECK(b.mean < 0.9 * b.max);  // not a uniform grid
  CHECK(std::fabs(b.max - max_ref) <= tol * max_ref);
  CHECK(std::fabs(b.mean - mean_ref) <= tol * mean_ref);
  double worst_plain = 0.0, worst_comp = 0.0;
  for (size_t i = 0; i < ref.size(); ++i) {
    const double want = ref[i] / max_ref;
    worst_plain = std::max(worst_plain, std::fabs(double(b.table->plain.func[i]) - want) / want);
    const double want_comp = std::max(ref[i] - mean_ref, 0.0) / max_ref;
    worst_comp = std::max(worst_comp, std::fabs(double(b.table->compensated.func[i]) - want_comp));
  }
  CHECK(worst_plain <= (2.0 * (kAnalyticUnits + kReductionUnits) + 1.0) * kU);
  CHECK(worst_comp <= 4.0 * tol);
  CHECK(double(b.table->plain.func_integral) == doctest::Approx(mean_ref / max_ref).epsilon(3.0 * tol));
}

TEST_CASE("bake_environment/failed launch restores the kernel globals") {
  Rig rig;
  EnvironmentBaker baker;
  const uint64_t baseline = rig.ledger.total();
  rig.dev.fail_launch = true;
  CHECK_THROWS_AS(baker.get(rig.dev, rig.kg, {1}, nullptr, kSmall), DeviceError);
  rig.dev.fail_launch = false;
  ar::KernelGlobals back{};
  rig.dev.download(&back, rig.kg, 0, sizeof back);
  CHECK(back.env_bake.grid == nullptr);
  CHECK(back.env_bake.n == 0u);
  CHECK(rig.ledger.total() == baseline);
  CHECK(baker.rebuild_count() == 0u);
  // The same key bakes afterwards: the failure left no cache behind.
  CHECK(baker.get(rig.dev, rig.kg, {1}, nullptr, kSmall).table != nullptr);
  CHECK(baker.rebuild_count() == 1u);
}

TEST_CASE("bake_environment/the cache pins what its key names") {
  // world_key holds Image addresses, which name one Image only while it lives: the cache keeps the
  // key's pin for as long as it keeps the key, so no freed Image's address can match it again. The
  // pin outlives every other holder, survives a cached call, and goes with the key.
  Rig rig;
  EnvironmentBaker baker;
  auto pin = std::make_shared<int>(7);
  const std::weak_ptr<int> watch = pin;
  REQUIRE(baker.get(rig.dev, rig.kg, {1}, pin, kSmall).table);
  pin.reset();
  CHECK(!watch.expired());
  REQUIRE(baker.get(rig.dev, rig.kg, {1}, nullptr, kSmall).table);  // cached: the key and its pin stay
  CHECK(baker.rebuild_count() == 1u);
  CHECK(!watch.expired());
  REQUIRE(baker.get(rig.dev, rig.kg, {2}, nullptr, kSmall).table);
  CHECK(baker.rebuild_count() == 2u);
  CHECK(watch.expired());
}

}  // TEST_SUITE("fast")

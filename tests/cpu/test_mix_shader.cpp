// SPDX-License-Identifier: Apache-2.0
// Mix Shader graphs (0005 items 31, 38, 43; Verification 6): BRANCH_MIX selects each child with
// its weight (binomial within 3 sigma over independent-sampler uniforms, nested mixes with the
// product of their factors, one factor per point), the uniform it leaves is again uniform, the
// surface entry is laid out as shared run, selection and one block per child (a value two
// children read is computed once), MAY_BE_DELTA is the OR over the
// children's own constants, and malformed or not yet supported Mix graphs are rejected loudly.
// Ref: docs/decisions/0005-material-model.md items 31, 38, 43, Verification 6; 0004 D14; pbrt-v4
// src/pbrt/util/sampling.h SampleDiscrete; Cycles src/kernel/svm/closure.h svm_node_mix_closure.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <memory>

#include "../../include/astroray/astroray.h"
#include "device/cpu/cpu_device.h"
#include "film/present.h"
#include "host/film/film_layout.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "host/thread_pool.h"
#include "kernel/stages/shade_surface.h"
#include "../../src/host/data_resource.h"
#include "../../src/host/material/compiler.h"
#include "../../src/host/material/graph.h"
#include "../../src/host/material/op_meta.h"
#include "material/material_header.h"
#include "sample/sobol.h"
#include "vm/interp.h"
#include "vm/ops/emit_le.h"

namespace {

using ar::Op;
using ar::RecordSlot;
using astroray::host::CompileResult;
using astroray::host::mix_ref;

std::vector<uint8_t> read_data(const std::string& rel) {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/" + rel, std::ios::binary);
  if (!f) f.open("data/" + rel, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/" << rel << " not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Fixture {
  std::vector<uint8_t> jh_bytes;
  astroray::ObserverTables obs;
  ar::Rgb2SpecTable table;
  ar::SceneTables tables = {};
  Fixture() {
    auto ob = read_data("spectral/observer.bin");
    std::string err = astroray::parse_observer(ob.data(), ob.size(), &obs);
    REQUIRE_MESSAGE(err.empty(), err);
    jh_bytes = read_data("spectral/rec709.jh");
    const char* e = ar::jh_view(jh_bytes.data(), jh_bytes.size(), &table);
    REQUIRE_MESSAGE(e == nullptr, (e ? e : ""));
    tables.rgb2spec = &table;
    tables.observer.cmf = &obs.cmf[0][0];
    tables.observer.illum = obs.d65;
  }
};
const Fixture& fixture() {
  static const Fixture f;
  return f;
}

// An ar_graph_desc built as the addon would, with child tags and Mix nodes.
struct GB {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  std::vector<int32_t> i;
  int add(Op op, std::vector<float> fp = {}, std::vector<int32_t> ip = {}) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size()), n.f32_count = uint32_t(fp.size());
    n.i32_first = uint32_t(i.size()), n.i32_count = uint32_t(ip.size());
    f.insert(f.end(), fp.begin(), fp.end());
    i.insert(i.end(), ip.begin(), ip.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  void link(int from, int to, uint32_t socket) { links.push_back({uint32_t(from), 0u, uint32_t(to), socket}); }
  int op2(Op op, int a, int b) {
    const int n = add(op);
    link(a, n, 0);
    link(b, n, 1);
    return n;
  }
  int store(int child, int src, RecordSlot s) {
    const int n = add(Op::STORE_SLOT, {}, {int32_t(s), child});
    link(src, n, 0);
    return n;
  }
  int store_const(int child, float v, RecordSlot s) { return store(child, add(Op::CONST_F32, {v}), s); }
  // Mix(a, b, factor): a and b are child ids or mix_ref(tag).
  int mix(int32_t tag, int factor, int32_t a, int32_t b) {
    const int n = add(Op::BRANCH_MIX, {}, {tag, a, b});
    link(factor, n, 0);
    return n;
  }
  int mix_const(int32_t tag, float factor, int32_t a, int32_t b) { return mix(tag, add(Op::CONST_F32, {factor}), a, b); }
  // Channel k of UV set 0: a per-point scalar the test sets through the context.
  int uv_channel(int32_t k) {
    const int ch = add(Op::CHANNEL_F32X3, {}, {k});
    link(add(Op::LOAD_CTX_UV, {}, {0}), ch, 0);
    return ch;
  }
  // An Emission of grey colour c and strength L in `child`.
  int emit(int child, float c, float strength) {
    const int e = add(Op::EMIT_LE, {}, {child});
    link(add(Op::CONST_F32X3, {c, c, c}), e, 0);
    link(add(Op::CONST_F32, {strength}), e, 1);
    return e;
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size()), d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data(), d.params_i32 = i.data();
    return d;
  }
  CompileResult compile() const {
    const ar_graph_desc d = desc();
    return astroray::host::compile_surface(d, fixture().table);
  }
  astroray::host::MaterialOutput material() const {
    const ar_graph_desc d = desc();
    return astroray::host::compile_material(d, fixture().table);
  }
};

// Runs the surface entry with selection uniform u; returns the record's
// base_metalness, which each test child sets to its own marker, and the uniform it leaves.
struct Run {
  float marker;
  float u_out;
};
Run run_surface(const CompileResult& r, float u, ar::Real2 uv = {0.0f, 0.0f}) {
  ar::VmRegs regs = {};
  ar::ShadeContext ctx = {};
  ctx.uv[0] = uv;
  ctx.lambda_t = ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}};
  ar::OpenPBRParams rec = {};
  rec.base_metalness = -1.0f;
  ar::Spec le = {};
  float su = u;
  REQUIRE(ar::run_entry(r.program.data(), r.program.at(0), &regs, ctx, fixture().tables, &rec, &le, &su) == ar::VmStatus::OK);
  return Run{float(rec.base_metalness), su};
}

// One entry run: the record it leaves, Le and the selection uniform. `u` < 0 runs without a
// selection uniform (an entry with a BRANCH_MIX then fails).
struct Full {
  ar::OpenPBRParams rec;
  ar::Spec le;
  float u_out;
};
Full run_entry_at(const CompileResult& r, uint32_t entry, float u, ar::Real2 uv = {0.0f, 0.0f}) {
  ar::VmRegs regs = {};
  ar::ShadeContext ctx = {};
  ctx.uv[0] = uv;
  ctx.lambda_t = ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}};
  Full f = {};
  f.rec.geometry_opacity = -1.0f;
  f.rec.closure_weight = 1.0f;  // as resolve_constant_record leaves it
  f.u_out = u;
  REQUIRE(ar::run_entry(r.program.data(), entry, &regs, ctx, fixture().tables, &f.rec, &f.le, u < 0.0f ? nullptr : &f.u_out) ==
          ar::VmStatus::OK);
  return f;
}

// Le of an Emission of grey colour c and strength L at the run_entry_at wavelengths.
ar::Spec emission_le(float c, float strength) {
  const float rgb[3] = {c, c, c};
  ar::Spec le = {};
  ar::op_emit_le(fixture().tables, rgb, strength, ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}}, &le);
  return le;
}

void check_rel(double got, double want, double tol) {
  INFO("got " << got << " want " << want);
  CHECK(std::fabs(got - want) <= tol * std::fabs(want));
}

// Sample i of one pixel in independent mode (0003 d23).
constexpr uint32_t kSeed = 0x3a1c5u;
float uniform(uint32_t i, uint32_t d) { return ar::sample_independent(0u, i, 0u, d, kSeed); }

// |count - n p| within 3 binomial sigma.
void check_binomial(uint32_t count, uint32_t n, double p) {
  const double mean = double(n) * p, sigma = std::sqrt(double(n) * p * (1.0 - p));
  INFO("count " << count << " expected " << mean << " +- " << 3.0 * sigma);
  CHECK(std::fabs(double(count) - mean) <= 3.0 * sigma);
}

// Words of an instruction including its immediates, from the ops.def operand tokens.
uint32_t length_of(Op op) {
  uint32_t imm = 0;
  for (const char* p = astroray::host::op_meta_table()[int(op)].operand_types; *p;) {
    const char* e = std::strchr(p, '+');
    const std::string t(p, e ? size_t(e - p) : std::strlen(p));
    imm += t == "imm" || t == "slotimm" || t == "jmpoff";
    p = e ? e + 1 : p + std::strlen(p);
  }
  return 1 + imm;
}

std::vector<Op> surface_ops(const CompileResult& r, std::vector<uint32_t>* words_at = nullptr) {
  std::vector<Op> out;
  for (uint32_t pc = r.program.at(0); pc < r.program.size();) {
    const Op op = Op(r.program[pc] & 0xffu);
    out.push_back(op);
    if (words_at) words_at->push_back(pc);
    if (op == Op::END) break;
    pc += length_of(op);
  }
  return out;
}

bool has_op(const CompileResult& r, Op op) {
  const std::vector<Op> ops = surface_ops(r);
  return std::find(ops.begin(), ops.end(), op) != ops.end();
}

}  // namespace

// A real shade_surface stage over n camera paths that all hit one quad at z = 0 (UV 0.25, 0.25)
// whose material is compiled from a GB graph: what a render does at one texel, with the film's
// termination histogram to count INVALID. The layout follows alpha_pass_through's rig.
namespace stage_rig {
using namespace astroray;
using namespace astroray::host;

constexpr ar_key kMesh = 100, kMat = 1, kGraphKey = 1001;

struct Rig {
  ContextState sctx;
  Scene scene{sctx, SceneConfig{2, {}, {}}};
  std::shared_ptr<const SceneTableSet> set;
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool{ctx, 1};
  CpuDevice dev{ledger, pool};
  std::unique_ptr<Film> film;
  ar::DisplayTarget display{};
  ar::KernelGlobals kg{};
  ar::PathStateSoA path{};
  ar::LightSamplerView no_lights{};  // no NEE: the decision and the weights are what is under test
  uint32_t n;
  std::vector<uint32_t> queue, active, sample_id, flags, hit_prim, hit_inst, prev_ng, prev_ns;
  std::vector<uint8_t> next_stage, term, ray_type;
  std::vector<uint16_t> event_index;
  std::vector<ar::U16x6> bounces;
  std::vector<ar::Real3> ray_o, ray_d;
  std::vector<ar::Real2> hit_uv;
  std::vector<ar::Real> hit_t, eta_scale, prev_dist;
  std::vector<ar::Spec> lambda_t, beta, r_u, r_l;

  Rig(const ar_graph_desc& graph, uint32_t count) : n(count) {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
    commit([&](Edit& e) {
      REQUIRE(scene.materials->set_graph(e, kGraphKey, &graph) == AR_OK);
      ar_material_desc m = {};
      m.surface_graph = kGraphKey;
      m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
      REQUIRE(scene.materials->set_material(e, kMat, &m) == AR_OK);
      const std::vector<float> pos{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
      const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
      ar_mesh_desc md = {};
      md.key = kMesh, md.vertex_count = 4, md.corner_count = 4, md.face_count = 1, md.motion_steps = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(scene, e, &md, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(kMesh) == AR_OK);
    });
    commit([&](Edit& e) {
      std::vector<ar_key> geom{kMesh}, object{500}, keys{kMat};
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
      d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = keys.data();
      REQUIRE(set_instances(e, &d) == AR_OK);
    });
    set = acquire_scene_tables(scene);
    REQUIRE(set);

    FilmRequest req;
    req.max_width = n;
    req.max_height = 1;
    req.slot_capacity = n;
    req.statistics = true;
    req.termination_stats = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->data().inv_radiance_scale = 1.0;

    for (uint32_t i = 0; i < n; ++i) {
      queue.push_back(i), active.push_back(i), sample_id.push_back(i);
      flags.push_back(ar::set_hero_lane(ar::LAST_BOUNCE_SPECULAR, i & 3u));
      next_stage.push_back(uint8_t(ar::Stage::SHADE_SURFACE)), term.push_back(uint8_t(ar::Term::ACTIVE));
      ray_type.push_back(1u);
      event_index.push_back(0);
      bounces.push_back(ar::U16x6{{0, 0, 0, 0, 0, 0}});
      hit_inst.push_back(0u), hit_prim.push_back(0u);
      hit_uv.push_back(ar::Real2{ar::Real(0.25), ar::Real(0.25)});
      hit_t.push_back(ar::Real(1));
      ray_o.push_back(ar::Real3{0, 0, 1});
      ray_d.push_back(ar::Real3{ar::Real(0.1), ar::Real(-0.2), ar::Real(-1)});
      eta_scale.push_back(ar::Real(1));
      prev_ng.push_back(0x1234u), prev_ns.push_back(0x5678u), prev_dist.push_back(ar::Real(7));
      ar::Spec lt, b, ru, rl;
      for (int l = 0; l < ar::kLanes; ++l) {
        lt.v[l] = ar::Real(430 + 70 * l);
        b.v[l] = ar::Real(0.5 + 0.125 * double(l));
        ru.v[l] = ar::Real(1.0 + 0.25 * double(l));
        rl.v[l] = ar::Real(2.0 + 0.5 * double(l));
      }
      lambda_t.push_back(lt), beta.push_back(b), r_u.push_back(ru), r_l.push_back(rl);
    }
    path.sample_id = sample_id.data(), path.next_stage = next_stage.data(), path.term = term.data();
    path.ray_type = ray_type.data(), path.flags = flags.data(), path.bounces = bounces.data();
    path.event_index = event_index.data(), path.ray_o = ray_o.data(), path.ray_d = ray_d.data();
    path.lambda_t = lambda_t.data(), path.beta = beta.data(), path.r_u = r_u.data(), path.r_l = r_l.data();
    path.eta_scale = eta_scale.data(), path.prev_ng = prev_ng.data(), path.prev_ns = prev_ns.data();
    path.prev_dist = prev_dist.data();
    path.hit_t = hit_t.data(), path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data();
    path.hit_uv = hit_uv.data();

    KgSession in;
    in.film = &film->data(), in.display = &display, in.active_pixels = active.data();
    in.uniforms.width = n, in.uniforms.height = 1, in.uniforms.samples_per_pass = 1;
    in.uniforms.seed = 23;
    in.uniforms.sampler_mode = ar::SAMPLER_INDEPENDENT;
    in.uniforms.max_bounces[0] = 8;
    in.uniforms.max_bounces[5] = 4;
    kg = build_kernel_globals(set.get(), in);
    kg.path = &path;
    kg.main_queue = queue.data();
    kg.light_sampler = &no_lights;
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
  void run() { cpu_stage_shade_surface(kg, ar::StepEntry{0, n, 0}, 0, n); }
  uint32_t hist(ar::Term t) const {
    const ar::FilmData& f = film->data();
    uint32_t total = 0;
    for (uint32_t p = 0; p < n; ++p) total += f.count[uint64_t(uint32_t(f.term_hist) + uint32_t(t)) * f.plane_stride + p];
    return total;
  }
  // The null event (a pass-through): the path stays active, no surface bounce, one transparent one.
  bool passed(uint32_t s) const {
    return term[s] == uint8_t(ar::Term::ACTIVE) && bounces[s].v[0] == 0u && bounces[s].v[5] == 1u;
  }
};
}  // namespace stage_rig

TEST_SUITE("fast") {

TEST_CASE("mix_shader/selection frequencies") {
  constexpr uint32_t kN = 100000;
  {  // Mix(A, B, 0.7): weights 0.3 and 0.7 (Cycles: the second closure takes the factor).
    GB g;
    g.store_const(0, 0.25f, RecordSlot::base_metalness);
    g.store_const(1, 0.75f, RecordSlot::base_metalness);
    g.mix_const(0, 0.7f, 0, 1);
    const CompileResult r = g.compile();
    REQUIRE_MESSAGE(r.ok, r.error);
    uint32_t second = 0, direct = 0;
    double u_sum[2] = {0.0, 0.0};
    for (uint32_t i = 0; i < kN; ++i) {
      direct += uint32_t(uniform(i, 0) >= 1.0f - 0.7f);
      const Run x = run_surface(r, uniform(i, 0));
      REQUIRE((x.marker == 0.25f || x.marker == 0.75f));
      const int side = x.marker == 0.75f;
      second += uint32_t(side);
      REQUIRE((x.u_out >= 0.0f && x.u_out < 1.0f));
      u_sum[side] += double(x.u_out);
    }
    CHECK(second == direct);  // the program selects exactly where u >= 1 - f
    check_binomial(second, kN, 0.7);
    // The uniform the selection leaves is uniform again on each side (mean 1/2, sigma 1/sqrt(12 n)).
    const uint32_t count[2] = {kN - second, second};
    for (int s = 0; s < 2; ++s) {
      INFO("side " << s);
      CHECK(std::fabs(u_sum[s] / double(count[s]) - 0.5) <= 3.0 / std::sqrt(12.0 * double(count[s])));
    }
  }
  {  // Mix(Mix(A, B, f1), C, 0.25) with f1 = 0.4 per point (a UV channel, computed before the
     // selection): weights (0.6 * 0.75, 0.4 * 0.75, 0.25).
    GB g;
    g.store_const(0, 0.1f, RecordSlot::base_metalness);
    g.store_const(1, 0.2f, RecordSlot::base_metalness);
    g.store_const(2, 0.3f, RecordSlot::base_metalness);
    const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
    const int f1 = g.add(Op::CHANNEL_F32X3, {}, {0});
    g.link(uv, f1, 0);
    g.mix(7, f1, 0, 1);
    g.mix_const(3, 0.25f, mix_ref(7), 2);
    const CompileResult r = g.compile();
    REQUIRE_MESSAGE(r.ok, r.error);
    uint32_t n[3] = {0, 0, 0};
    for (uint32_t i = 0; i < kN; ++i) {
      const float m = run_surface(r, uniform(i, 1), ar::Real2{0.4f, 0.9f}).marker;
      REQUIRE((m == 0.1f || m == 0.2f || m == 0.3f));
      ++n[m == 0.1f ? 0 : m == 0.2f ? 1 : 2];
    }
    check_binomial(n[0], kN, 0.6 * 0.75);
    check_binomial(n[1], kN, 0.4 * 0.75);
    check_binomial(n[2], kN, 0.25);
    REQUIRE(r.children.size() == 3u);
    CHECK(r.children[0].id == 0);
    CHECK(r.children[1].id == 1);
    CHECK(r.children[2].id == 2);
  }
}

TEST_CASE("mix_shader/surface layout") {
  // Mix(A, B, f) where both children read one texture: the shared run computes it once before the
  // BRANCH_MIX, block A ends in a JMP to END, block B falls into END, and each child's constants are
  // code in its block while the constant block keeps the defaults.
  GB g;
  const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
  const int tex = g.add(Op::TEX_IMAGE, {}, {3, 0});
  g.link(uv, tex, 0);
  g.store(0, tex, RecordSlot::base_color);
  g.store(1, tex, RecordSlot::coat_color);
  g.store_const(0, 0.25f, RecordSlot::base_metalness);
  g.store_const(1, 0.75f, RecordSlot::base_metalness);
  g.mix_const(0, 0.5f, 0, 1);
  const CompileResult r = g.compile();
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK_FALSE(r.constant_surface);
  std::vector<uint32_t> at;
  const std::vector<Op> ops = surface_ops(r, &at);
  const std::vector<Op> want = {Op::LOAD_CTX_UV, Op::TEX_IMAGE,  Op::CONST_F32, Op::BRANCH_MIX, Op::STORE_SLOT,
                                Op::CONST_F32,   Op::STORE_SLOT, Op::JMP,       Op::STORE_SLOT,  Op::CONST_F32,
                                Op::STORE_SLOT,  Op::END};
  CHECK(ops == want);
  REQUIRE(ops.size() == want.size());
  const std::vector<uint32_t>& p = r.program;
  CHECK(p[at[3] + 1] == at[8] - at[3]);   // BRANCH_MIX to block B
  CHECK(p[at[7] + 1] == at[11] - at[7]);  // JMP to END
  CHECK(((p[at[3]] >> 8) & 0xffu) == ((p[at[2]] >> 8) & 0xffu));  // BRANCH_MIX reads the factor CONST wrote
  const ar::ConstantBlock def = ar::default_constant_block(fixture().table);
  CHECK(r.block.base_metalness == def.base_metalness);
  REQUIRE(r.children.size() == 2u);
  CHECK(r.children[0].block.base_metalness == 0.25f);
  CHECK(r.children[1].block.base_metalness == 0.75f);
  CHECK(r.children[0].runtime_slots == (uint64_t(1) << uint32_t(RecordSlot::base_color)));
  CHECK(r.children[1].runtime_slots == (uint64_t(1) << uint32_t(RecordSlot::coat_color)));
  CHECK(run_surface(r, 0.25f).marker == 0.25f);
  CHECK(run_surface(r, 0.75f).marker == 0.75f);
}

TEST_CASE("mix_shader/a shared value stays live into a later block") {
  // A per-point colour (UV set 0) read by both children is computed before the selection. Block A
  // reads it first and then fills registers with its own per-point temporaries; block B, laid out
  // after A, stores the colour again. Its register must not be reused inside A, so B's record
  // holds the colour's lift, checked against the lift computed here.
  GB g;
  const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
  g.store(0, uv, RecordSlot::base_color);
  int t = g.add(Op::LOAD_CTX_P);
  const RecordSlot sink[] = {RecordSlot::specular_color, RecordSlot::fuzz_color, RecordSlot::geometry_tangent,
                             RecordSlot::geometry_coat_tangent, RecordSlot::subsurface_color, RecordSlot::transmission_color};
  std::vector<int> temps{t};
  for (int k = 0; k < 6; ++k) temps.push_back(t = g.op2(k % 2 ? Op::ADD_F32X3 : Op::MUL_F32X3, t, temps[size_t(k) / 2]));
  for (int k = 0; k < 6; ++k) g.store(0, temps[size_t(k) + 1], sink[k]);
  g.store(1, uv, RecordSlot::coat_color);
  g.mix_const(0, 0.5f, 0, 1);
  const CompileResult r = g.compile();
  REQUIRE_MESSAGE(r.ok, r.error);
  ar::VmRegs regs = {};
  ar::ShadeContext ctx = {};
  ctx.uv[0] = ar::Real2{0.3f, 0.6f};
  ctx.P = ar::Real3{0.5f, -1.25f, 2.0f};
  ctx.lambda_t = ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}};
  ar::OpenPBRParams rec = {};
  ar::Spec le = {};
  float su = 0.75f;  // the second child
  REQUIRE(ar::run_entry(r.program.data(), r.program.at(0), &regs, ctx, fixture().tables, &rec, &le, &su) == ar::VmStatus::OK);
  uint32_t flags = 0;
  const ar::Spec want = ar::albedo(fixture().table, ar::Rgb{{ar::Real(0.3f), ar::Real(0.6f), ar::Real(0)}}, ctx.lambda_t, &flags);
  for (int l = 0; l < ar::kLanes; ++l) CHECK(rec.coat_color.v[l] == want.v[l]);
}

TEST_CASE("mix_shader/may be delta per child") {
  // Each child's specular roughness lives only in its own block: the flag is the OR over the
  // children, never the constant block's defaults.
  auto flags = [](float rough_a, float rough_b) {
    GB g;
    g.store_const(0, rough_a, RecordSlot::specular_roughness);
    g.store_const(1, rough_b, RecordSlot::specular_roughness);
    g.mix_const(0, 0.5f, 0, 1);
    const astroray::host::MaterialOutput m = g.material();
    REQUIRE_FALSE(m.magenta);
    return m.flags;
  };
  CHECK((flags(0.5f, 0.6f) & ar::MAY_BE_DELTA) == 0u);
  CHECK((flags(0.5f, 0.0f) & ar::MAY_BE_DELTA) != 0u);  // only the second child is a mirror
  CHECK((flags(0.0f, 0.5f) & ar::MAY_BE_DELTA) != 0u);
  // A per-point roughness in one child sets it too; a child with no stores has the defaults.
  GB g;
  const int ch = g.add(Op::CHANNEL_F32X3, {}, {1});
  g.link(g.add(Op::LOAD_CTX_UV, {}, {0}), ch, 0);
  g.store(1, ch, RecordSlot::specular_roughness);
  g.store_const(0, 0.5f, RecordSlot::specular_roughness);
  g.mix_const(0, 0.5f, 0, 1);
  CHECK((g.material().flags & ar::MAY_BE_DELTA) != 0u);
  GB e;
  e.store_const(0, 0.5f, RecordSlot::specular_roughness);
  e.mix_const(0, 0.5f, 0, 1);  // child 1 stores nothing: OpenPBR's default roughness
  CHECK((e.material().flags & ar::MAY_BE_DELTA) == 0u);
}

TEST_CASE("mix_shader/interior ior from the children") {
  // The medium IOR (interior_ior) of a Mix material is the children's constant specular_ior when
  // they agree; otherwise it is 1 with an approximated entry, never the block's default.
  auto material = [](float ior_a, float ior_b) {
    GB g;
    g.store_const(0, ior_a, RecordSlot::specular_ior);
    g.store_const(1, ior_b, RecordSlot::specular_ior);
    g.mix_const(0, 0.5f, 0, 1);
    return g.material();
  };
  const astroray::host::MaterialOutput same = material(1.33f, 1.33f), differ = material(1.33f, 1.7f);
  REQUIRE_FALSE(same.magenta);
  CHECK(same.entries.empty());
  CHECK(same.compiled.block.interior_ior.p[0] == 1.33f);
  REQUIRE(differ.entries.size() == 1u);
  CHECK(differ.entries[0].code == AR_REPORT_NODE_APPROXIMATED);
  CHECK(differ.compiled.block.interior_ior.p[0] == 1.0f);
}

TEST_CASE("mix_shader/malformed or unsupported graphs are rejected") {
  struct Case {
    const char* what;
    GB g;
    const char* error;
  };
  std::vector<Case> cases;
  {
    GB g;
    g.store_const(0, 0.5f, RecordSlot::base_metalness);
    g.store_const(1, 0.5f, RecordSlot::base_metalness);
    cases.push_back({"a child tag without a Mix", g, "has no Mix Shader"});
  }
  {
    GB g;
    g.mix_const(0, 0.5f, 0, 1);
    g.mix_const(1, 0.5f, 2, 3);
    cases.push_back({"two roots", g, "exactly one root Mix"});
  }
  {
    GB g;
    g.mix_const(0, 0.5f, 0, mix_ref(1));
    g.mix_const(1, 0.5f, 0, 2);
    cases.push_back({"a child named twice", g, "is named twice"});
  }
  {
    GB g;
    g.mix_const(0, 0.5f, 0, mix_ref(4));
    cases.push_back({"a missing tag", g, "which the graph lacks"});
  }
  {
    GB g;
    g.mix_const(0, 0.5f, 0, 1);
    g.mix_const(1, 0.5f, 2, mix_ref(2));
    g.mix_const(2, 0.5f, 3, mix_ref(1));
    cases.push_back({"a cycle beside the tree", g, "cycle or a second tree"});
  }
  {
    GB g;
    g.mix_const(0, 0.5f, 0, 1);
    g.store_const(5, 0.5f, RecordSlot::base_metalness);
    cases.push_back({"a child no Mix names", g, "no Mix Shader names it"});
  }
  {
    GB g;
    g.mix_const(0, 0.5f, 0, 1);
    g.store_const(0, 0.5f, RecordSlot::base_metalness);
    g.store_const(0, 0.6f, RecordSlot::base_metalness);
    cases.push_back({"a slot stored twice in one child", g, "stored twice in child 0"});
  }
  {
    GB g;
    g.mix_const(0, 0.5f, 0, 1);
    g.store_const(0, 0.5f, RecordSlot::base_metalness);
    g.emit(1, 1.0f, 1.0f);
    g.store_const(1, 0.5f, RecordSlot::geometry_opacity);
    cases.push_back({"an emission with an opacity and no BSDF store", g, "is none of the child kinds"});
  }
  {
    GB g;
    g.mix_const(0, 0.5f, 0, 1);
    g.store_const(0, 0.5f, RecordSlot::base_metalness);
    g.store(1, g.uv_channel(0), RecordSlot::geometry_opacity);
    cases.push_back({"a per-point opacity with no BSDF store", g, "is none of the child kinds"});
  }
  {
    GB g;
    g.mix_const(0, 0.5f, 0, 1);
    g.store_const(0, 0.5f, RecordSlot::base_metalness);
    const int e = g.add(Op::EMIT_LE, {}, {1});
    g.link(g.add(Op::BLACKBODY, {5000.0f, 1.0f}), e, 0);
    g.link(g.add(Op::CONST_F32, {1.0f}), e, 1);
    cases.push_back({"a Blackbody emission under a Mix", g, "Blackbody emission cannot sit under a Mix Shader"});
  }
  for (const Case& c : cases) {
    INFO(c.what);
    const CompileResult r = c.g.compile();
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.error.find(c.error) != std::string::npos, r.error);
    const astroray::host::MaterialOutput m = c.g.material();
    CHECK(m.magenta);
    REQUIRE(m.entries.size() == 1u);
    CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
  }
}


TEST_CASE("mix_shader/emission has no selection variance") {
  // Mix(Emission, Diffuse, f): the Emission child is pruned, so no BRANCH_MIX is left; Le is
  // (1 - f) Le_E for every selection uniform, bit for bit, and the Diffuse child's closure weight
  // is f (the second input takes f, Cycles svm_node_mix_closure). The emission entry, which NEE on
  // an emissive triangle runs, gives the same Le.
  const ar::Spec le_e = emission_le(0.8f, 3.0f);
  {
    GB g;
    g.emit(0, 0.8f, 3.0f);
    g.store_const(1, 0.25f, RecordSlot::base_metalness);
    g.mix_const(0, 0.3f, 0, 1);
    const CompileResult r = g.compile();
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK_FALSE(has_op(r, Op::BRANCH_MIX));
    CHECK(r.children.size() == 1u);
    REQUIRE(r.program[2] != 0u);
    const Full first = run_entry_at(r, r.program[0], uniform(0, 2));
    for (uint32_t i = 0; i < 1000; ++i) {
      const Full x = run_entry_at(r, r.program[0], uniform(i, 2));
      CHECK(x.rec.base_metalness == 0.25f);
      CHECK(x.rec.closure_weight == 0.3f);
      for (int l = 0; l < ar::kLanes; ++l) CHECK(x.le.v[l] == first.le.v[l]);
    }
    const Full em = run_entry_at(r, r.program[2], -1.0f);
    for (int l = 0; l < ar::kLanes; ++l) {
      CHECK(em.le.v[l] == first.le.v[l]);
      check_rel(double(first.le.v[l]), 0.7 * double(le_e.v[l]), 1e-6);
    }
  }
  {  // A per-point factor f = uv.x: Le = (1 - f) Le_E and closure weight f at every point.
    GB g;
    g.emit(0, 0.8f, 3.0f);
    g.store_const(1, 0.25f, RecordSlot::base_metalness);
    g.mix(0, g.uv_channel(0), 0, 1);
    const CompileResult r = g.compile();
    REQUIRE_MESSAGE(r.ok, r.error);
    for (float f : {0.1f, 0.5f, 0.85f}) {
      INFO("f " << f);
      const ar::Real2 uv{f, 0.0f};
      const Full first = run_entry_at(r, r.program[0], 0.5f, uv);
      CHECK(first.rec.closure_weight == f);
      for (uint32_t i = 0; i < 1000; ++i) {
        const Full x = run_entry_at(r, r.program[0], uniform(i, 3), uv);
        for (int l = 0; l < ar::kLanes; ++l) CHECK(x.le.v[l] == first.le.v[l]);
      }
      for (int l = 0; l < ar::kLanes; ++l) check_rel(double(first.le.v[l]), (1.0 - double(f)) * double(le_e.v[l]), 1e-6);
    }
  }
}

TEST_CASE("mix_shader/each child's emission carries its own opacity") {
  // Emission sits inside each child's opacity mix (items 4, 38): child i adds w_i a_i Le_i, with
  // its own a_i, never another child's.
  const ar::Spec le_e = emission_le(0.8f, 3.0f);
  {  // Mix(Emission, S with alpha 0.4, 0.3): Le = 0.7 Le_E; S survives with a' = 1 - 0.3 (1 - 0.4)
     // and closure weight 0.3 x 0.4 / a', so a' x closure weight = w_S a_S.
    GB g;
    g.emit(0, 0.8f, 3.0f);
    g.store_const(1, 0.25f, RecordSlot::base_metalness);
    g.store_const(1, 0.4f, RecordSlot::geometry_opacity);
    g.mix_const(0, 0.3f, 0, 1);
    const astroray::host::MaterialOutput m = g.material();
    REQUIRE_FALSE(m.magenta);
    const CompileResult& r = m.compiled;
    const double a2 = 1.0 - 0.3 * 0.6;
    for (uint32_t i = 0; i < 100; ++i) {
      const Full x = run_entry_at(r, r.program[0], uniform(i, 4));
      for (int l = 0; l < ar::kLanes; ++l) check_rel(double(x.le.v[l]), 0.7 * double(le_e.v[l]), 1e-6);
      check_rel(double(x.rec.geometry_opacity), a2, 1e-6);
      check_rel(double(x.rec.closure_weight), 0.3 * 0.4 / a2, 1e-6);
    }
    check_rel(double(r.block.geometry_opacity), a2, 1e-6);  // T = 0.3 x 0.6, constant: no opacity entry
    CHECK(r.program[1] == 0u);
    CHECK((m.flags & ar::NON_OPAQUE) != 0u);
  }
  for (bool per_point : {false, true}) {
    // Mix(S1 alpha 0.5, S2 alpha a2 with an emission, 0.25): Le = 0.25 a2 Le_E whichever child is
    // selected; S1 is selected as often as before, S2 still emits when S1 is selected.
    INFO("per-point a2 " << per_point);
    GB g;
    g.store_const(0, 0.1f, RecordSlot::base_metalness);
    g.store_const(0, 0.5f, RecordSlot::geometry_opacity);
    g.store_const(1, 0.2f, RecordSlot::base_metalness);
    if (per_point) g.store(1, g.uv_channel(1), RecordSlot::geometry_opacity);
    else g.store_const(1, 0.6f, RecordSlot::geometry_opacity);
    g.emit(1, 0.8f, 3.0f);
    g.mix_const(0, 0.25f, 0, 1);
    const astroray::host::MaterialOutput m = g.material();
    REQUIRE_FALSE(m.magenta);
    const CompileResult& r = m.compiled;
    REQUIRE(r.children.size() == 2u);
    // A per-point a2 enters Le through CLAMP_F32, as the decision clamps it: nothing says "unclamped".
    CHECK(std::none_of(m.entries.begin(), m.entries.end(), [](const astroray::host::MaterialEntry& e) {
            return e.detail.find("unclamped") != std::string::npos;
          }));
    const ar::Real2 uv{0.0f, 0.6f};
    const Full first = run_entry_at(r, r.program[0], 0.1f, uv);
    uint32_t seen[2] = {0, 0};
    for (uint32_t i = 0; i < 1000; ++i) {
      const Full x = run_entry_at(r, r.program[0], uniform(i, 5), uv);
      ++seen[x.rec.base_metalness == 0.2f];
      CHECK(x.rec.geometry_opacity == (x.rec.base_metalness == 0.2f ? 0.6f : 0.5f));
      CHECK(x.rec.closure_weight == 1.0f);
      for (int l = 0; l < ar::kLanes; ++l) CHECK(x.le.v[l] == first.le.v[l]);
    }
    CHECK(seen[0] > 0u);
    CHECK(seen[1] > 0u);
    for (int l = 0; l < ar::kLanes; ++l) check_rel(double(first.le.v[l]), 0.25 * 0.6 * double(le_e.v[l]), 1e-6);
  }
}

TEST_CASE("mix_shader/opacity entry sums weights") {
  // Mix(Mix(S0, S1, f1), S2, 0.25) with opacities a0 = 0.3, a1 and a2 = 0.8: the opacity entry
  // writes a_eff with 1 - a_eff = sum w_i (1 - a_i), with no selection uniform and for every one,
  // and the surface entry's selected child's 1 - a_i averages to the same T over u (the null event
  // is an unbiased estimate of T, item 38).
  using astroray::host::MaterialOutput;
  for (bool per_point : {true, false}) {
    INFO("per-point f1 and a1 " << per_point);
    GB g;
    g.store_const(0, 0.1f, RecordSlot::base_metalness);
    g.store_const(0, 0.3f, RecordSlot::geometry_opacity);
    g.store_const(1, 0.2f, RecordSlot::base_metalness);
    g.store_const(2, 0.3f, RecordSlot::base_metalness);
    g.store_const(2, 0.8f, RecordSlot::geometry_opacity);
    if (per_point) {
      g.store(1, g.uv_channel(1), RecordSlot::geometry_opacity);
      g.mix(7, g.uv_channel(0), 0, 1);
    } else {
      g.store_const(1, 0.45f, RecordSlot::geometry_opacity);
      g.mix_const(7, 0.4f, 0, 1);
    }
    g.mix_const(3, 0.25f, mix_ref(7), 2);
    const MaterialOutput m = g.material();
    REQUIRE_FALSE(m.magenta);
    const CompileResult& r = m.compiled;
    CHECK((m.flags & ar::NON_OPAQUE) != 0u);
    const ar::Real2 uv{0.4f, 0.45f};
    const double f1 = 0.4, a1 = 0.45;
    const double t = (1.0 - f1) * 0.75 * 0.7 + f1 * 0.75 * (1.0 - a1) + 0.25 * 0.2;
    if (per_point) {
      REQUIRE(r.program[1] != 0u);
      const Full none = run_entry_at(r, r.program[1], -1.0f, uv);
      check_rel(1.0 - double(none.rec.geometry_opacity), t, 1e-6);
      for (uint32_t i = 0; i < 100; ++i) {
        const Full x = run_entry_at(r, r.program[1], uniform(i, 6), uv);
        CHECK(x.rec.geometry_opacity == none.rec.geometry_opacity);
        CHECK(x.u_out == uniform(i, 6));  // the opacity entry selects nothing
      }
    } else {
      CHECK(r.program[1] == 0u);  // a constant opacity: the block holds a_eff
      check_rel(1.0 - double(r.block.geometry_opacity), t, 1e-6);
    }
    constexpr uint32_t kN = 100000;
    double sum = 0.0;
    for (uint32_t k = 0; k < kN; ++k) sum += 1.0 - double(run_entry_at(r, r.program[0], (float(k) + 0.5f) / float(kN), uv).rec.geometry_opacity);
    CHECK(std::fabs(sum / kN - t) <= 1e-4);  // stratified u: three step boundaries of 1 / kN each
  }
}

TEST_CASE("mix_shader/transparent folds into opacity") {
  // Mix(Transparent, S, a) is S with opacity a x a_S and closure weight 1 (item 38, OpenPBR's
  // top-level mix): no BRANCH_MIX is left. A non-white Transparent (opacity 1 - mean colour) is
  // reported once and leaves S the exact a' and closure weight of the pruning rule.
  auto transparent = [](float t_opacity, int factor_mode) {
    GB g;
    g.store_const(0, t_opacity, RecordSlot::geometry_opacity);  // the Transparent child
    g.store_const(1, 0.25f, RecordSlot::base_metalness);
    g.store_const(1, 0.5f, RecordSlot::geometry_opacity);
    if (factor_mode == 0) g.mix_const(0, 0.6f, 0, 1);
    else g.mix(0, g.uv_channel(0), 0, 1);
    return g.material();
  };
  auto folded_entry = [](const astroray::host::MaterialOutput& m) {
    return std::any_of(m.entries.begin(), m.entries.end(), [](const astroray::host::MaterialEntry& e) {
      return e.code == AR_REPORT_NODE_APPROXIMATED && e.detail.find("colour taken as its mean") != std::string::npos;
    });
  };
  {  // Constant a = 0.6: a' = 0.3 folds on the host.
    const astroray::host::MaterialOutput m = transparent(0.0f, 0);
    REQUIRE_FALSE(m.magenta);
    CHECK_FALSE(folded_entry(m));
    CHECK_FALSE(has_op(m.compiled, Op::BRANCH_MIX));
    CHECK(m.compiled.children.size() == 1u);
    CHECK(m.compiled.program[1] == 0u);
    check_rel(double(m.compiled.block.geometry_opacity), 0.3, 1e-6);
    for (float u : {0.05f, 0.5f, 0.95f}) {
      const Full x = run_entry_at(m.compiled, m.compiled.program[0], u);
      CHECK(x.rec.base_metalness == 0.25f);
      check_rel(double(x.rec.geometry_opacity), 0.3, 1e-6);
      CHECK(x.rec.closure_weight == 1.0f);
    }
  }
  {  // Per-point a = uv.x: the opacity is MUL_F32 of a and a_S; the opacity entry agrees.
    const astroray::host::MaterialOutput m = transparent(0.0f, 1);
    REQUIRE_FALSE(m.magenta);
    REQUIRE(m.compiled.program[1] != 0u);
    for (float a : {0.2f, 0.6f, 0.9f}) {
      INFO("a " << a);
      const ar::Real2 uv{a, 0.0f};
      const Full x = run_entry_at(m.compiled, m.compiled.program[0], 0.5f, uv);
      CHECK(x.rec.geometry_opacity == ar::op_mul_f32(a, 0.5f));
      CHECK(x.rec.closure_weight == 1.0f);
      check_rel(double(run_entry_at(m.compiled, m.compiled.program[1], -1.0f, uv).rec.geometry_opacity), double(a) * 0.5, 1e-6);
    }
  }
  {  // Transparent opacity 0.25 (mean colour 0.75), a = 0.6, a_S = 0.5: T = 0.4 x 0.75 + 0.6 x 0.5,
     // a' = 1 - T, closure weight 0.6 x 0.5 / a'.
    const astroray::host::MaterialOutput m = transparent(0.25f, 0);
    REQUIRE_FALSE(m.magenta);
    CHECK(folded_entry(m));
    const double a2 = 1.0 - (0.4 * 0.75 + 0.6 * 0.5);
    const Full x = run_entry_at(m.compiled, m.compiled.program[0], 0.5f);
    check_rel(double(x.rec.geometry_opacity), a2, 1e-6);
    check_rel(double(x.rec.closure_weight), 0.3 / a2, 1e-6);
    check_rel(double(m.compiled.block.geometry_opacity), a2, 1e-6);
  }
}

TEST_CASE("mix_shader/emission estimate sums weights") {
  // Item 43: the estimate of Mix(Emission 2, Emission 4, 0.25) is 0.75 x 2 + 0.25 x 4; with no
  // BSDF child the surface stores closure_weight 0 and Le is the same weighted sum.
  GB g;
  g.emit(0, 1.0f, 2.0f);
  g.emit(1, 1.0f, 4.0f);
  g.mix_const(0, 0.25f, 0, 1);
  const astroray::host::MaterialOutput m = g.material();
  REQUIRE_FALSE(m.magenta);
  double sum = 0.0;
  for (const astroray::host::EmissionTerm& t : m.emission_estimate) sum += double(t.strength);
  CHECK(m.emission_estimate.size() == 2u);
  check_rel(sum, 0.75 * 2.0 + 0.25 * 4.0, 1e-6);
  CHECK((m.flags & ar::CONSTANT_EMISSION) == 0u);
  CHECK(m.compiled.children.empty());
  const Full x = run_entry_at(m.compiled, m.compiled.program[0], 0.5f);
  CHECK(x.rec.closure_weight == 0.0f);
  const ar::Spec le_1 = emission_le(1.0f, 1.0f);
  for (int l = 0; l < ar::kLanes; ++l) check_rel(double(x.le.v[l]), 2.5 * double(le_1.v[l]), 1e-6);
  // A per-point factor is not known: each term is bounded by its authored strength, never 0.
  GB p;
  p.emit(0, 1.0f, 2.0f);
  p.emit(1, 1.0f, 4.0f);
  p.mix(0, p.uv_channel(0), 0, 1);
  double bound = 0.0;
  for (const astroray::host::EmissionTerm& t : p.material().emission_estimate) bound += double(t.strength);
  CHECK(bound == 6.0);
}


TEST_CASE("mix_shader/nested collapse") {
  // Mix(Emission, Mix(Transparent a_T = 0.2, S a_S = 0.9, 0.7), 0.6): w_E = 0.4, w_T = 0.6 x 0.3,
  // w_S = 0.6 x 0.7. By hand: T = 0.18 x 0.8 + 0.42 x 0.1 = 0.186, so S takes a' = 0.814 and
  // closure weight w_S a_S / a' = 0.378 / 0.814 (the surface returns w_S a_S f_S); Le = 0.4 Le_E.
  const ar::Spec le_e = emission_le(0.8f, 3.0f);
  {
    GB g;
    g.emit(0, 0.8f, 3.0f);
    g.store_const(1, 0.2f, RecordSlot::geometry_opacity);
    g.store_const(2, 0.25f, RecordSlot::base_metalness);
    g.store_const(2, 0.9f, RecordSlot::geometry_opacity);
    g.mix_const(1, 0.7f, 1, 2);
    g.mix_const(0, 0.6f, 0, mix_ref(1));
    const CompileResult r = g.compile();
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK_FALSE(has_op(r, Op::BRANCH_MIX));
    const Full x = run_entry_at(r, r.program[0], 0.5f);
    CHECK(x.rec.base_metalness == 0.25f);
    check_rel(double(x.rec.geometry_opacity), 0.814, 1e-6);
    check_rel(double(x.rec.closure_weight), 0.378 / 0.814, 1e-6);
    check_rel(double(r.block.geometry_opacity), 0.814, 1e-6);
    for (int l = 0; l < ar::kLanes; ++l) check_rel(double(x.le.v[l]), 0.4 * double(le_e.v[l]), 1e-6);
  }
  {  // Mix(Emission, Mix(S1, S2, 0.3), 0.6): the inner mix still selects (0.7 / 0.3) and both
     // children take closure weight 0.6, the outer collapse's own weight.
    GB g;
    g.emit(0, 0.8f, 3.0f);
    g.store_const(1, 0.1f, RecordSlot::base_metalness);
    g.store_const(2, 0.2f, RecordSlot::base_metalness);
    g.mix_const(1, 0.3f, 1, 2);
    g.mix_const(0, 0.6f, 0, mix_ref(1));
    const CompileResult r = g.compile();
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(has_op(r, Op::BRANCH_MIX));
    const Full a = run_entry_at(r, r.program[0], 0.1f), b = run_entry_at(r, r.program[0], 0.9f);
    CHECK(a.rec.base_metalness == 0.1f);
    CHECK(b.rec.base_metalness == 0.2f);
    CHECK(a.rec.closure_weight == 0.6f);
    CHECK(b.rec.closure_weight == 0.6f);
    for (int l = 0; l < ar::kLanes; ++l) CHECK(a.le.v[l] == b.le.v[l]);
  }
}

TEST_CASE("mix_shader/per-point alpha under an emission mix") {
  // 0005 item 38 with DIV_F32 and CLAMP_F32. Mix(Emission, S with a per-point alpha a, s = 0.5):
  // S survives with a' = 1 - s (1 - a) and closure weight s a / a', so a' x weight = s a, the
  // emission child keeps w_E = 1 - s, and the opacity entry stores the same a'.
  const ar::Spec le_e = emission_le(0.8f, 3.0f);
  {
    GB g;
    g.emit(0, 0.8f, 3.0f);
    g.store_const(1, 0.25f, RecordSlot::base_metalness);
    g.store(1, g.uv_channel(0), RecordSlot::geometry_opacity);
    g.mix_const(0, 0.5f, 0, 1);
    const CompileResult r = g.compile();
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(has_op(r, Op::DIV_F32));
    CHECK(has_op(r, Op::CLAMP_F32));
    REQUIRE(r.program[1] != 0u);
    for (float a : {0.2f, 0.6f, 0.9f, 1.0f}) {
      INFO("alpha " << a);
      const ar::Real2 uv{a, 0.0f};
      const double a2 = 1.0 - 0.5 * (1.0 - double(a));
      const Full x = run_entry_at(r, r.program[0], 0.5f, uv);
      check_rel(double(x.rec.geometry_opacity), a2, 1e-6);
      check_rel(double(x.rec.closure_weight), 0.5 * double(a) / a2, 1e-6);
      check_rel(double(x.rec.geometry_opacity) * double(x.rec.closure_weight), 0.5 * double(a), 1e-6);
      for (int l = 0; l < ar::kLanes; ++l) check_rel(double(x.le.v[l]), 0.5 * double(le_e.v[l]), 1e-6);
      check_rel(double(run_entry_at(r, r.program[1], -1.0f, uv).rec.geometry_opacity), a2, 1e-6);
    }
    {  // A NaN alpha is not an unread weight: the store refuses it and the path would end INVALID.
      ar::VmRegs regs = {};
      ar::ShadeContext ctx = {};
      ctx.uv[0] = {std::nanf(""), 0.0f};
      ctx.lambda_t = ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}};
      ar::OpenPBRParams rec = {};
      ar::Spec le = {};
      float u = 0.5f;
      CHECK(ar::run_entry(r.program.data(), r.program.at(0), &regs, ctx, fixture().tables, &rec, &le, &u) ==
            ar::VmStatus::NON_FINITE_INPUT);
    }
  }
  {  // A per-point factor of 1.5 weights as 1 and one of -0.5 as 0: Mix(Emission, S, f = uv.x).
    GB g;
    g.emit(0, 0.8f, 3.0f);
    g.store_const(1, 0.25f, RecordSlot::base_metalness);
    g.mix(0, g.uv_channel(0), 0, 1);
    const CompileResult r = g.compile();
    REQUIRE_MESSAGE(r.ok, r.error);
    const Full hi = run_entry_at(r, r.program[0], 0.5f, ar::Real2{1.5f, 0.0f});
    CHECK(hi.rec.closure_weight == 1.0f);
    for (int l = 0; l < ar::kLanes; ++l) CHECK(hi.le.v[l] == 0.0f);
    const Full lo = run_entry_at(r, r.program[0], 0.5f, ar::Real2{-0.5f, 0.0f});
    CHECK(lo.rec.closure_weight == 0.0f);
    for (int l = 0; l < ar::kLanes; ++l) check_rel(double(lo.le.v[l]), double(le_e.v[l]), 1e-6);
  }
  {  // A per-point opacity outside [0, 1] scales Le by its clamped value, as the decision clamps it:
     // Mix(S1, S2 with alpha uv.y and an emission, 0.25) weights the emission by 0.25 a.
    GB g;
    g.store_const(0, 0.1f, RecordSlot::base_metalness);
    g.store_const(0, 0.5f, RecordSlot::geometry_opacity);
    g.store_const(1, 0.2f, RecordSlot::base_metalness);
    g.store(1, g.uv_channel(1), RecordSlot::geometry_opacity);
    g.emit(1, 0.8f, 3.0f);
    g.mix_const(0, 0.25f, 0, 1);
    const CompileResult r = g.compile();
    REQUIRE_MESSAGE(r.ok, r.error);
    for (int l = 0; l < ar::kLanes; ++l) {
      check_rel(double(run_entry_at(r, r.program[0], 0.5f, ar::Real2{0.0f, 1.6f}).le.v[l]), 0.25 * double(le_e.v[l]), 1e-6);
      CHECK(run_entry_at(r, r.program[0], 0.5f, ar::Real2{0.0f, -0.4f}).le.v[l] == 0.0f);
    }
    // Without a Mix the emission entry takes the same clamped opacity.
    GB p;
    p.store(0, p.uv_channel(1), RecordSlot::geometry_opacity);
    p.emit(0, 0.8f, 3.0f);
    const CompileResult q = p.compile();
    REQUIRE_MESSAGE(q.ok, q.error);
    REQUIRE(q.program[2] != 0u);
    for (int l = 0; l < ar::kLanes; ++l) {
      check_rel(double(run_entry_at(q, q.program[2], -1.0f, ar::Real2{0.0f, 1.6f}).le.v[l]), double(le_e.v[l]), 1e-6);
      CHECK(run_entry_at(q, q.program[2], -1.0f, ar::Real2{0.0f, -0.4f}).le.v[l] == 0.0f);
    }
  }
  {  // Ruling 115: factor 1 over a per-point alpha of exactly 0 (uv.x - uv.x) gives a' = 0, where the
     // survivor weight s a / a' is 0/0. The program stores weight 0 instead, the stage takes the null
     // event for every path (the surface is fully transparent), and no path ends INVALID.
    GB g;
    g.emit(0, 0.8f, 3.0f);
    g.store_const(1, 0.25f, RecordSlot::base_metalness);
    g.store(1, g.op2(Op::SUB_F32, g.uv_channel(0), g.uv_channel(0)), RecordSlot::geometry_opacity);
    g.mix_const(0, 1.0f, 0, 1);
    const CompileResult r = g.compile();
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(has_op(r, Op::DIV_F32));
    const Full x = run_entry_at(r, r.program[0], 0.5f, ar::Real2{0.25f, 0.25f});
    CHECK(x.rec.geometry_opacity == 0.0f);
    CHECK(x.rec.closure_weight == 0.0f);
    const ar_graph_desc d = g.desc();
    stage_rig::Rig rig(d, 256);
    rig.run();
    uint32_t passes = 0;
    for (uint32_t s = 0; s < rig.n; ++s) passes += rig.passed(s);
    CHECK(passes == rig.n);
    CHECK(rig.hist(ar::Term::INVALID) == 0u);
  }
}

TEST_CASE("mix_shader/per-point factors and opacity are clamped, not reported") {
  // A per-point Mix factor or opacity that reaches a weight, an emission or T goes through
  // CLAMP_F32 (item 38), so no entry says it enters unclamped; a constant one is clamped on the host.
  using astroray::host::MaterialOutput;
  auto unclamped = [](const MaterialOutput& m) {
    return std::any_of(m.entries.begin(), m.entries.end(),
                       [](const astroray::host::MaterialEntry& e) { return e.detail.find("unclamped") != std::string::npos; });
  };
  for (bool per_point : {false, true}) {
    INFO("per-point factor " << per_point);
    GB em;  // the emission weight 1 - f and the closure weight f
    em.emit(0, 1.0f, 1.0f);
    em.store_const(1, 0.25f, RecordSlot::base_metalness);
    if (per_point) em.mix(0, em.uv_channel(0), 0, 1);
    else em.mix_const(0, 0.5f, 0, 1);
    const MaterialOutput me = em.material();
    REQUIRE_FALSE(me.magenta);
    CHECK_FALSE(unclamped(me));
    CHECK(has_op(me.compiled, Op::CLAMP_F32) == per_point);
    GB op;  // T = (1 - f) x 0.5 only: two BSDF children, the first half transparent
    op.store_const(0, 0.1f, RecordSlot::base_metalness);
    op.store_const(0, 0.5f, RecordSlot::geometry_opacity);
    op.store_const(1, 0.2f, RecordSlot::base_metalness);
    if (per_point) op.mix(0, op.uv_channel(0), 0, 1);
    else op.mix_const(0, 0.5f, 0, 1);
    const MaterialOutput mo = op.material();
    REQUIRE_FALSE(mo.magenta);
    CHECK_FALSE(unclamped(mo));
    GB black;  // closure weight only: a black Transparent (a = 1) collapses into S; no emission, T = 0
    black.store_const(0, 1.0f, RecordSlot::geometry_opacity);
    black.store_const(1, 0.2f, RecordSlot::base_metalness);
    if (per_point) black.mix(0, black.uv_channel(0), 0, 1);
    else black.mix_const(0, 0.5f, 0, 1);
    const MaterialOutput mk = black.material();
    REQUIRE_FALSE(mk.magenta);
    CHECK((mk.flags & ar::NON_OPAQUE) == 0u);
    CHECK_FALSE(unclamped(mk));
    GB sel;  // opaque BSDF children only: the factor only selects, and BRANCH_MIX clamps it itself
    sel.store_const(0, 0.1f, RecordSlot::base_metalness);
    sel.store_const(1, 0.2f, RecordSlot::base_metalness);
    if (per_point) sel.mix(0, sel.uv_channel(0), 0, 1);
    else sel.mix_const(0, 0.5f, 0, 1);
    const MaterialOutput ms = sel.material();
    CHECK_FALSE(unclamped(ms));
    CHECK_FALSE(has_op(ms.compiled, Op::CLAMP_F32));  // nothing reads the clamped factor, so none is emitted
  }
}

TEST_CASE("mix_shader/emission estimate takes the image mean of a clamped opacity and factor") {
  // Item 43: a texture-driven weight keeps the image's mean through CLAMP_F32. Image 3's channel 1
  // has mean 0.5 and channel 2 mean 0.75; without the means the weight is bounded by 1.
  const astroray::host::ImageMeanFn means = [](uint32_t image, std::array<float, 3>* rgb) {
    if (image != 3) return false;
    *rgb = {0.25f, 0.5f, 0.75f};
    return true;
  };
  auto channel_of_image = [](GB& g, int32_t ch) {
    const int tex = g.add(Op::TEX_IMAGE, {}, {3, 0});
    g.link(g.add(Op::LOAD_CTX_UV, {}, {0}), tex, 0);
    const int c = g.add(Op::CHANNEL_F32X3, {}, {ch});
    g.link(tex, c, 0);
    return c;
  };
  auto estimate = [&](const GB& g, bool with_means) {
    const ar_graph_desc d = g.desc();
    const astroray::host::MaterialOutput m = astroray::host::compile_material(d, fixture().table, with_means ? &means : nullptr);
    REQUIRE_FALSE(m.magenta);
    REQUIRE(m.emission_estimate.size() == 1u);
    return double(m.emission_estimate[0].strength);
  };
  {  // Mix(S1, S2 with opacity channel 2 and an emission of strength 3, 0.25): 0.25 x 0.75 x 3.
    GB g;
    g.store_const(0, 0.1f, RecordSlot::base_metalness);
    g.store_const(1, 0.2f, RecordSlot::base_metalness);
    g.store(1, channel_of_image(g, 2), RecordSlot::geometry_opacity);
    g.emit(1, 1.0f, 3.0f);
    g.mix_const(0, 0.25f, 0, 1);
    check_rel(estimate(g, true), 0.25 * 0.75 * 3.0, 1e-6);
    check_rel(estimate(g, false), 0.25 * 3.0, 1e-6);  // the opacity bounded by 1
  }
  {  // Mix(S, Emission of strength 4, f = channel 1): the second socket weighs the emission by f.
    GB g;
    g.store_const(0, 0.1f, RecordSlot::base_metalness);
    g.emit(1, 1.0f, 4.0f);
    g.mix(0, channel_of_image(g, 1), 0, 1);
    check_rel(estimate(g, true), 0.5 * 4.0, 1e-6);
    check_rel(estimate(g, false), 4.0, 1e-6);  // the factor bounded by 1
  }
}

TEST_CASE("mix_shader/emission estimate keeps constant weights") {
  // Item 43: Mix(S1, S2 with a per-point alpha and an emission of strength 3, 0.01): the weight
  // 0.01 x a counts 0.01 at its value and bounds only the per-point a by 1, so the estimate is
  // 0.03, not the authored 3.
  GB g;
  g.store_const(0, 0.1f, RecordSlot::base_metalness);
  g.store_const(1, 0.2f, RecordSlot::base_metalness);
  g.store(1, g.uv_channel(0), RecordSlot::geometry_opacity);
  g.emit(1, 1.0f, 3.0f);
  g.mix_const(0, 0.01f, 0, 1);
  const astroray::host::MaterialOutput m = g.material();
  REQUIRE_FALSE(m.magenta);
  REQUIRE(m.emission_estimate.size() == 1u);
  check_rel(double(m.emission_estimate[0].strength), 0.01 * 3.0, 1e-6);
}

}  // TEST_SUITE

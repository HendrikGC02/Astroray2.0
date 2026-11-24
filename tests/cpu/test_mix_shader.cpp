// SPDX-License-Identifier: Apache-2.0
// Mix Shader graphs (0005 items 31, 38, 43; Verification 6): BRANCH_MIX selects each child with
// its weight (binomial within 3 sigma over independent-sampler uniforms, nested mixes with the
// product of their factors, one factor per point), the uniform it leaves is again uniform, the
// surface entry is laid out as shared run, selection and one block per child (a value two
// children read is computed once, r31 is never allocated), MAY_BE_DELTA is the OR over the
// children's own constants, and malformed or not yet supported Mix graphs are rejected loudly.
// Ref: docs/decisions/0005-material-model.md items 31, 38, 43, Verification 6; 0004 D14; pbrt-v4
// src/pbrt/util/sampling.h SampleDiscrete; Cycles src/kernel/svm/closure.h svm_node_mix_closure.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "../../src/host/material/compiler.h"
#include "../../src/host/material/graph.h"
#include "../../src/host/material/op_meta.h"
#include "material/material_header.h"
#include "sample/sobol.h"
#include "vm/interp.h"

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

// Runs the surface entry with uniform u in the selection register; returns the record's
// base_metalness, which each test child sets to its own marker, and the uniform left in r31.
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
  regs.r[ar::kSelectReg] = u;
  REQUIRE(ar::run_entry(r.program.data(), r.program.at(0), &regs, ctx, fixture().tables, &rec, &le) == ar::VmStatus::OK);
  return Run{float(rec.base_metalness), regs.r[ar::kSelectReg]};
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

}  // namespace

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
  CHECK(((p[at[3]] >> 8) & 0xffu) == ar::kSelectReg);
  for (size_t k = 0; k < ops.size(); ++k)
    if (ops[k] != Op::BRANCH_MIX && ops[k] != Op::END && ops[k] != Op::JMP) {
      INFO(k);
      CHECK(((p[at[k]] >> 8) & 0xffu) < ar::kSelectReg);  // nothing else names r31
    }
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
    g.store_const(1, 0.5f, RecordSlot::geometry_opacity);
    cases.push_back({"opacity under a Mix", g, "geometry_opacity in a Mix Shader graph"});
  }
  {
    GB g;
    g.mix_const(0, 0.5f, 0, 1);
    const int e = g.add(Op::EMIT_LE, {}, {1});
    g.link(g.add(Op::CONST_F32X3, {1.0f, 1.0f, 1.0f}), e, 0);
    g.link(g.add(Op::CONST_F32, {1.0f}), e, 1);
    cases.push_back({"an emission under a Mix", g, "an emission in a Mix Shader graph"});
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

}  // TEST_SUITE

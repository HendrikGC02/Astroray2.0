// SPDX-License-Identifier: Apache-2.0
// 0005 item 35 and Verification 6 (first clause): the constant the compiler folds for every op
// that ops.def marks foldable equals what the interpreter computes for the same instruction, bit
// for bit, on 1e4 random inputs; every other op with a graph encoding stays in the program. The
// op list and each op's shape come from op_meta_table(), graph.h and the row's operand tokens, so
// a new foldable op is covered, or refused with a message, without a test edit; only the range of
// an i32 immediate is written here per op.
// Ref: docs/decisions/0005-material-model.md items 30, 35, Verification 6.
#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../src/host/ledger.h"
#include "../../src/host/material/compiler.h"
#include "../../src/host/material/graph.h"
#include "../../src/host/material/material_store.h"
#include "../../src/host/material/op_meta.h"
#include "sample/sobol.h"
#include "vm/interp.h"

namespace {

using ar::Op;
using astroray::host::compile_surface;
using astroray::host::CompileResult;
using astroray::host::op_input_type;
using astroray::host::op_meta_count;
using astroray::host::op_meta_table;
using astroray::host::op_param_counts;
using astroray::host::OpMeta;
using astroray::host::VType;

const ar::Rgb2SpecTable& table() {
  static const auto store = std::make_shared<astroray::host::MaterialStore>(std::make_shared<astroray::host::Ledger>());
  return store->table();
}

uint32_t bits(float f) { return ar::bit_cast<uint32_t>(f); }

// Inputs are the sampler's, never a rand(): values in [-4, 4).
constexpr uint32_t kSeed = 0x57a11u;
float input(uint32_t i, uint32_t d) { return ar::sample_independent(i, 0u, 0u, d, kSeed) * 8.0f - 4.0f; }

// A graph in the addon's description form, with a constants pool per node.
struct GB {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  std::vector<int32_t> i;
  int add(Op op, const std::vector<float>& fp = {}, const std::vector<int32_t>& ip = {}) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size());
    n.f32_count = uint32_t(fp.size());
    n.i32_first = uint32_t(i.size());
    n.i32_count = uint32_t(ip.size());
    f.insert(f.end(), fp.begin(), fp.end());
    i.insert(i.end(), ip.begin(), ip.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  void link(int from, int to, uint32_t socket) { links.push_back({uint32_t(from), 0u, uint32_t(to), socket}); }
  int store(int src, ar::RecordSlot s) {
    const int n = add(Op::STORE_SLOT, {}, {int32_t(s)});
    link(src, n, 0);
    return n;
  }
  CompileResult compile() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size());
    d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size());
    d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data();
    d.links = links.data();
    d.params_f32 = f.data();
    d.params_i32 = i.data();
    return compile_surface(d, table());
  }
};

std::vector<std::string> tokens_of(const OpMeta& m) {
  std::vector<std::string> tok;
  for (const char* p = m.operand_types; *p;) {
    const char* e = std::strchr(p, '+');
    tok.emplace_back(p, e ? size_t(e - p) : std::strlen(p));
    p = e ? e + 1 : p + std::strlen(p);
  }
  return tok;
}

// The values an i32 parameter of each foldable op may take, written out per op: the test feeds
// 0 .. count-1, and a foldable op with i32 parameters that is not listed is refused with a message
// rather than fed values its parser may reject.
bool i32_param_count(const OpMeta& m, uint32_t* count) {
  if (std::strcmp(m.name, "CHANNEL_F32X3") == 0) return *count = 3, true;  // channel 0-2
  return false;
}

// The row's operands as the test rebuilds them: a destination register, `arity` input registers,
// then immediates (the node's f32 parameters, then its i32 ones). Returns the immediate count, or -1 with *why set for any other layout.
int immediates_of(const OpMeta& m, std::string* why) {
  const std::vector<std::string> tok = tokens_of(m);
  const size_t regs = 1u + size_t(m.arity);
  if (tok.size() < regs) return *why = "has fewer register operands than 1 + arity", -1;
  for (size_t k = 0; k < tok.size(); ++k)
    if (tok[k] != (k < regs ? "reg" : "imm")) return *why = "operand tokens are not reg x (1 + arity) then imm", -1;
  return int(tok.size() - regs);
}

uint32_t word(Op op, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
  return uint32_t(op) | (a << 8) | (b << 16) | (c << 24);
}

// The opcodes of the surface entry (word 0 of the entry table), up to its END. Immediate words
// follow from each row's operand tokens.
std::vector<Op> program_ops(const CompileResult& r) {
  std::vector<Op> out;
  REQUIRE(!r.program.empty());
  for (uint32_t pc = r.program[0]; pc < r.program.size();) {
    const Op op = Op(r.program[pc] & 0xffu);
    out.push_back(op);
    if (op == Op::END) break;
    uint32_t imm = 0;
    for (const std::string& t : tokens_of(op_meta_table()[int(op)])) imm += t == "imm" || t == "slotimm" || t == "jmpoff";
    pc += 1 + imm;
  }
  return out;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("compiler_fold/folded equals interpreted per foldable op") {
  constexpr uint32_t kN = 10000;
  int checked = 0;
  for (int k = 0; k < op_meta_count(); ++k) {
    const OpMeta& m = op_meta_table()[k];
    if (!m.foldable) continue;
    INFO(m.name);
    std::string why;
    const int imms = immediates_of(m, &why);
    REQUIRE_MESSAGE(imms >= 0, m.name << " is foldable but " << why << ": extend compiler_fold/folded equals interpreted");
    const bool vec3 = std::strcmp(m.result_type, "f32x3") == 0;
    const bool scalar = std::strcmp(m.result_type, "f32") == 0;
    REQUIRE_MESSAGE((vec3 || scalar), m.name << " is foldable with result type " << m.result_type);
    uint32_t nf = 0, ni = 0;
    REQUIRE_MESSAGE(op_param_counts(m.op, &nf, &ni), m.name << " is foldable but a graph cannot hold it");
    const bool matches = int(nf + ni) == imms;
    REQUIRE_MESSAGE(matches, m.name << ": the f32 then i32 parameters must be the immediates");
    uint32_t i32_values = 0;
    REQUIRE_MESSAGE((ni == 0 || i32_param_count(m, &i32_values)),
                    m.name << " has i32 immediates: list their range in i32_param_count");
    const ar::RecordSlot slot = vec3 ? ar::RecordSlot::geometry_tangent : ar::RecordSlot::base_metalness;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < kN; ++i) {
      GB g;
      std::vector<uint32_t> code = {0, 0, 0, 0};  // the entry table, then the entry at word 4
      const uint32_t dst = 16;
      const uint32_t src[2] = {0, 4};
      uint32_t off = 0;
      std::vector<int> leaf;
      for (int s = 0; s < m.arity; ++s) {
        const VType t = op_input_type(m.op, s);
        const bool typed = t == VType::F32 || t == VType::F32X3;
        REQUIRE_MESSAGE(typed, m.name << ": input " << s << " is neither f32 nor f32x3");
        if (t == VType::F32) {
          leaf.push_back(g.add(Op::CONST_F32, {input(i, off)}));
          code.push_back(word(Op::CONST_F32, src[s]));
          code.push_back(bits(input(i, off)));
          off += 1;
        } else {
          leaf.push_back(g.add(Op::CONST_F32X3, {input(i, off), input(i, off + 1), input(i, off + 2)}));
          code.push_back(word(Op::CONST_F32X3, src[s]));
          for (uint32_t d = 0; d < 3; ++d) code.push_back(bits(input(i, off + d)));
          off += 3;
        }
      }
      std::vector<float> params;
      for (uint32_t d = 0; d < nf; ++d) params.push_back(input(i, 8 + d));
      std::vector<int32_t> iparams;
      for (uint32_t d = 0; d < ni; ++d) iparams.push_back(int32_t((i + d) % i32_values));
      const int node = g.add(m.op, params, iparams);
      for (int s = 0; s < m.arity; ++s) g.link(leaf[size_t(s)], node, uint32_t(s));
      g.store(node, slot);
      code.push_back(word(m.op, dst, src[0], src[1]));
      for (float p : params) code.push_back(bits(p));
      for (int32_t p : iparams) code.push_back(uint32_t(p));
      code.push_back(word(Op::STORE_SLOT, dst));
      code.push_back(uint32_t(slot));
      code.push_back(word(Op::END));

      const CompileResult r = g.compile();
      REQUIRE_MESSAGE(r.ok, r.error);
      REQUIRE_MESSAGE(r.constant_surface, m.name << " did not fold");
      ar::VmRegs regs = {};
      ar::ShadeContext ctx = {};
      ar::OpenPBRParams rec = {};
      ar::Spec le = {};
      ar::SceneTables tables = {};
      tables.rgb2spec = &table();  // STORE_SLOT requires it even for a scalar slot
      REQUIRE(ar::run_entry(code.data(), 4, &regs, ctx, tables, &rec, &le) == ar::VmStatus::OK);
      if (vec3) {
        const float want[3] = {float(rec.geometry_tangent.x), float(rec.geometry_tangent.y), float(rec.geometry_tangent.z)};
        for (int d = 0; d < 3; ++d) bad += bits(r.block.geometry_tangent[d]) != bits(want[d]);
      } else {
        bad += bits(r.block.base_metalness) != bits(float(rec.base_metalness));
      }
    }
    CHECK_MESSAGE(bad == 0, m.name << ": " << bad << " folded components differ from the interpreter");
    ++checked;
  }
  CHECK(checked >= 6);
}

TEST_CASE("compiler_fold/non foldable ops stay in the program") {
  int checked = 0;
  for (int k = 0; k < op_meta_count(); ++k) {
    const OpMeta& m = op_meta_table()[k];
    uint32_t nf = 0, ni = 0;
    if (m.foldable || m.result_type[0] == '\0' || !op_param_counts(m.op, &nf, &ni)) continue;
    // These two lower to a SpectrumRef in the constant block, never to a program op (see
    // compiler/spectrum kinds accepted per semantic).
    if (m.op == Op::BLACKBODY || m.op == Op::CAUCHY_ABBE) continue;
    INFO(m.name);
    GB g;
    std::vector<int> leaf;
    for (int s = 0; s < m.arity; ++s) {
      const VType t = op_input_type(m.op, s);
      leaf.push_back(t == VType::F32 ? g.add(Op::CONST_F32, {0.5f}) : g.add(Op::CONST_F32X3, {0.25f, 0.5f, 0.75f}));
    }
    const int node = g.add(m.op, std::vector<float>(nf, 0.5f), std::vector<int32_t>(ni, 0));
    for (int s = 0; s < m.arity; ++s) g.link(leaf[size_t(s)], node, uint32_t(s));
    const char* rt = m.result_type;
    g.store(node, std::strcmp(rt, "spec") == 0    ? ar::RecordSlot::base_color
                  : std::strcmp(rt, "f32x3") == 0 ? ar::RecordSlot::geometry_tangent
                                                  : ar::RecordSlot::base_metalness);
    const CompileResult r = g.compile();
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK_FALSE(r.constant_surface);
    bool found = false;
    for (Op o : program_ops(r)) found = found || o == m.op;
    CHECK_MESSAGE(found, m.name << " is not foldable but is missing from the compiled program");
    ++checked;
  }
  CHECK(checked >= 4);
}

}  // TEST_SUITE

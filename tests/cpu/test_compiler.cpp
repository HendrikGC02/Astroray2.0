// SPDX-License-Identifier: Apache-2.0
// 0005 items 29-32, 35, 38: the material compiler core. Constant graphs fold to length 0 with the
// constant block as expected, folding stops at texture and context reads, the register allocator
// rejects a program that does not fit, and compiled programs run through the interpreter give the
// record that hand evaluation gives.
// Ref: docs/decisions/0005-material-model.md items 29-32, 35, 38.
#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "../../src/host/material/compiler.h"
#include "material/constant_block.h"
#include "vm/interp.h"

namespace {

using ar::Op;
using astroray::host::compile_surface;
using astroray::host::CompileResult;

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
  ar::SceneTables tables;
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

// Builds an ar_graph_desc from calls, as the addon would.
struct GB {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  std::vector<int32_t> i;
  int add(Op op, std::vector<float> fp = {}, std::vector<int32_t> ip = {}) {
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
  int op2(Op op, int a, int b) {
    const int n = add(op);
    link(a, n, 0);
    link(b, n, 1);
    return n;
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size());
    d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size());
    d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data();
    d.links = links.data();
    d.params_f32 = f.data();
    d.params_i32 = i.data();
    return d;
  }
};

CompileResult compile(const GB& g) {
  const ar_graph_desc d = g.desc();
  return compile_surface(d, fixture().table);
}

// The opcodes of the entry at `offset`, up to and including its END.
std::vector<Op> ops_of(const CompileResult& r, uint32_t offset) {
  std::vector<Op> out;
  for (uint32_t pc = offset; pc < r.program.size();) {
    const Op op = Op(r.program[pc] & 0xffu);
    out.push_back(op);
    if (op == Op::END) break;
    uint32_t imm = 0;
    if (op == Op::CONST_F32 || op == Op::TEX_IMAGE || op == Op::STORE_SLOT || op == Op::JMP) imm = 1;
    if (op == Op::CONST_F32X3) imm = 3;
    pc += 1 + imm;
  }
  return out;
}

bool has(const std::vector<Op>& v, Op o) {
  for (Op x : v)
    if (x == o) return true;
  return false;
}

float f(float v) { return v; }

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("compiler/constant surface folds to length 0") {
  GB g;
  const int rgb = g.add(Op::CONST_F32X3, {0.2f, 0.4f, 0.6f});
  const int half = g.add(Op::CONST_F32, {0.5f});
  g.store(g.op2(Op::MUL_F32X3_F32, rgb, half), ar::RecordSlot::base_color);
  g.store(g.add(Op::CONST_F32, {0.7f}), ar::RecordSlot::base_metalness);
  g.store(g.add(Op::CONST_F32, {1.45f}), ar::RecordSlot::specular_ior);
  g.store(g.add(Op::CONST_F32, {1.5f}), ar::RecordSlot::transmission_color);  // above 1 in a REFLECTANCE slot
  g.store(g.add(Op::CONST_F32X3, {0.0f, 1.0f, 0.0f}), ar::RecordSlot::geometry_tangent);
  const int ec = g.add(Op::CONST_F32X3, {1.0f, 0.5f, 0.25f});
  const int es = g.add(Op::CONST_F32, {3.0f});
  const int emit = g.add(Op::EMIT_LE);
  g.link(ec, emit, 0);
  g.link(es, emit, 1);
  const CompileResult r = compile(g);
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK(r.constant_surface);
  CHECK(r.program.size() == 0);
  std::printf("compiler: constant surface program length %zu words\n", r.program.size());

  // Expected block: the folded value (0.2, 0.4, 0.6) * 0.5 lifted once on the host (023).
  ar::SpectrumRef want;
  uint32_t fl = 0;
  REQUIRE(ar::make_rgb_ref(fixture().table, ar::Rgb{{0.1f, 0.2f, 0.3f}}, ar::SpectrumSemantic::REFLECTANCE, &want, &fl));
  CHECK(r.block.base_color.kind == uint8_t(ar::SpectrumKind::RGB));
  for (int k = 0; k < 4; ++k) CHECK(r.block.base_color.p[k] == want.p[k]);
  CHECK(r.block.base_metalness == 0.7f);
  CHECK(r.block.specular_ior.kind == uint8_t(ar::SpectrumKind::CONSTANT));
  CHECK(r.block.specular_ior.p[0] == 1.45f);
  CHECK(r.block.transmission_color.p[0] == 1.0f);
  CHECK((r.clamp_flags & ar::kRgbAboveOneClamped) != 0u);
  CHECK(r.block.geometry_tangent[1] == 1.0f);
  CHECK(r.block.emission_luminance == 3.0f);
  CHECK(r.block.emission_color.kind == uint8_t(ar::SpectrumKind::RGB));
  CHECK(r.block.base_weight == 1.0f);  // untouched slots keep the OpenPBR default
}

TEST_CASE("compiler/texture base colour keeps its ops and folds the rest") {
  GB g;
  const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
  const int tex = g.add(Op::TEX_IMAGE, {}, {3});
  g.link(uv, tex, 0);
  const int lift = g.add(Op::RGB_LIFT, {}, {int32_t(ar::SpectrumSemantic::REFLECTANCE)});
  g.link(tex, lift, 0);
  g.store(lift, ar::RecordSlot::base_color);
  const int a = g.add(Op::CONST_F32, {0.5f});
  g.store(g.op2(Op::MUL_F32, a, a), ar::RecordSlot::specular_roughness);
  const CompileResult r = compile(g);
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK_FALSE(r.constant_surface);
  const std::vector<Op> want = {Op::LOAD_CTX_UV, Op::TEX_IMAGE, Op::RGB_LIFT, Op::STORE_SLOT, Op::END};
  CHECK(ops_of(r, 4) == want);
  CHECK(r.block.specular_roughness == 0.25f);
  // The pre-lifted flag (bit 8) marks a spectrum source.
  CHECK((r.program[r.program.size() - 2] & 0x1ffu) == (uint32_t(ar::RecordSlot::base_color) | ar::kStoreSlotSpecSource));
}

TEST_CASE("compiler/folding never crosses LOAD_CTX_UV") {
  GB g;
  const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
  const int half = g.add(Op::CONST_F32, {0.5f});
  g.store(g.op2(Op::MUL_F32X3_F32, uv, half), ar::RecordSlot::base_color);
  const CompileResult r = compile(g);
  REQUIRE_MESSAGE(r.ok, r.error);
  const std::vector<Op> ops = ops_of(r, 4);
  CHECK(has(ops, Op::LOAD_CTX_UV));
  CHECK(has(ops, Op::MUL_F32X3_F32));
  CHECK(has(ops, Op::CONST_F32));  // the folded constant operand is materialised for its reader
  CHECK_FALSE(r.constant_surface);
}

TEST_CASE("compiler/no folding across texture read") {
  GB g;
  const int c = g.add(Op::CONST_F32X3, {0.25f, 0.5f, 0.0f});  // constant coordinates, still a texture read
  const int tex = g.add(Op::TEX_IMAGE, {}, {1});
  g.link(c, tex, 0);
  const int half = g.add(Op::CONST_F32, {0.5f});
  g.store(g.op2(Op::MUL_F32X3_F32, tex, half), ar::RecordSlot::base_color);
  const CompileResult r = compile(g);
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK_FALSE(r.constant_surface);
  const std::vector<Op> ops = ops_of(r, 4);
  CHECK(has(ops, Op::TEX_IMAGE));
  CHECK(has(ops, Op::MUL_F32X3_F32));
}

TEST_CASE("compiler/register overflow rejected") {
  GB g;
  std::vector<int> loads;
  for (int k = 0; k < 14; ++k) loads.push_back(g.add(Op::LOAD_CTX_P));  // 14 x 3 = 42 registers live
  int sum = loads[0];
  for (int k = 1; k < 14; ++k) sum = g.op2(Op::ADD_F32X3, sum, loads[size_t(k)]);
  g.store(sum, ar::RecordSlot::geometry_normal);
  const CompileResult r = compile(g);
  CHECK_FALSE(r.ok);
  CHECK(r.error.find("needs ") == 0);
  CHECK(r.error.find(" registers, 32 available") != std::string::npos);
}

TEST_CASE("compiler/type errors and cycles are returned") {
  GB g;
  const int a = g.add(Op::CONST_F32, {1.0f});
  g.store(a, ar::RecordSlot::geometry_normal);  // f32 into a frame slot
  CHECK_FALSE(compile(g).ok);
  GB c;
  const int x = c.add(Op::ADD_F32X3);
  const int y = c.add(Op::ADD_F32X3);
  c.link(y, x, 0);
  c.link(y, x, 1);
  c.link(x, y, 0);
  c.link(x, y, 1);
  const CompileResult r = compile(c);
  CHECK_FALSE(r.ok);
  CHECK(r.error.find("cycle") != std::string::npos);
}

TEST_CASE("compiler/compiled programs match hand evaluation") {
  const Fixture& fx = fixture();
  ar::VmRegs regs = {};
  ar::ShadeContext ctx = {};
  ctx.lambda_t = ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}};
  ctx.P = {0.8f, -0.4f, 2.0f};
  ctx.uv[0] = {0.6f, 0.2f};

  {  // A: uv * 0.5 + 0.1 into base_color, lifted at the slot's semantic.
    GB g;
    const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
    const int half = g.add(Op::CONST_F32, {0.5f});
    const int off = g.add(Op::CONST_F32X3, {0.1f, 0.1f, 0.1f});
    g.store(g.op2(Op::ADD_F32X3, g.op2(Op::MUL_F32X3_F32, uv, half), off), ar::RecordSlot::base_color);
    const CompileResult r = compile(g);
    REQUIRE_MESSAGE(r.ok, r.error);
    std::printf("compiler: graph A (uv tint) program length %zu words\n", r.program.size());
    ar::OpenPBRParams rec = {};
    ar::Spec le = {};
    REQUIRE(ar::run_entry(r.program.data(), r.program.at(0), &regs, ctx, fx.tables, &rec, &le) == ar::VmStatus::OK);
    uint32_t fl = 0;
    const ar::Rgb rgb{{f(f(0.6f * 0.5f) + 0.1f), f(f(0.2f * 0.5f) + 0.1f), f(f(0.0f * 0.5f) + 0.1f)}};
    const ar::Spec want = ar::albedo(fx.table, rgb, ctx.lambda_t, &fl);
    for (int k = 0; k < ar::kLanes; ++k) CHECK(rec.base_color.v[k] == want.v[k]);
  }
  {  // B: emission P * 0.5 with strength 2, run as the surface entry and as the emission entry.
    GB g;
    const int p = g.add(Op::LOAD_CTX_P);
    const int half = g.add(Op::CONST_F32, {0.5f});
    const int col = g.op2(Op::MUL_F32X3_F32, p, half);
    const int emit = g.add(Op::EMIT_LE);
    g.link(col, emit, 0);
    g.link(g.add(Op::CONST_F32, {2.0f}), emit, 1);
    g.store(g.add(Op::CONST_F32, {0.3f}), ar::RecordSlot::base_metalness);  // constant: stays out of the code
    const CompileResult r = compile(g);
    REQUIRE_MESSAGE(r.ok, r.error);
    REQUIRE(r.program.at(2) != 0u);
    std::printf("compiler: graph B (emission) program length %zu words\n", r.program.size());
    uint32_t fl = 0;
    const ar::Rgb rgb{{f(0.8f * 0.5f), f(-0.4f * 0.5f), f(2.0f * 0.5f)}};
    const ar::Spec s = ar::illuminant(fx.table, fx.tables.observer, rgb, ctx.lambda_t, &fl);
    for (uint32_t entry : {r.program[0], r.program[2]}) {
      ar::OpenPBRParams rec = {};
      ar::Spec le = {};
      REQUIRE(ar::run_entry(r.program.data(), entry, &regs, ctx, fx.tables, &rec, &le) == ar::VmStatus::OK);
      for (int k = 0; k < ar::kLanes; ++k) CHECK(le.v[k] == s.v[k] * ar::Real(2.0f));
    }
  }
  {  // C: texture, explicit lift, pre-lifted store. Filtering is not implemented, so a texel is magenta.
    GB g;
    const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
    const int tex = g.add(Op::TEX_IMAGE, {}, {0});
    g.link(uv, tex, 0);
    const int lift = g.add(Op::RGB_LIFT, {}, {int32_t(ar::SpectrumSemantic::REFLECTANCE)});
    g.link(tex, lift, 0);
    g.store(lift, ar::RecordSlot::base_color);
    const CompileResult r = compile(g);
    REQUIRE_MESSAGE(r.ok, r.error);
    std::printf("compiler: graph C (texture lift) program length %zu words\n", r.program.size());
    ar::OpenPBRParams rec = {};
    ar::Spec le = {};
    REQUIRE(ar::run_entry(r.program.data(), r.program.at(0), &regs, ctx, fx.tables, &rec, &le) == ar::VmStatus::OK);
    uint32_t fl = 0;
    const ar::Spec want = ar::albedo(fx.table, ar::Rgb{{1.0f, 0.0f, 1.0f}}, ctx.lambda_t, &fl);
    for (int k = 0; k < ar::kLanes; ++k) CHECK(rec.base_color.v[k] == want.v[k]);
  }
}

}  // TEST_SUITE

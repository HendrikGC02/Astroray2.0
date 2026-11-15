// SPDX-License-Identifier: Apache-2.0
// 0005 items 30-32, 35 and Verification 6 (CPU part): every M1a opcode run through the
// interpreter equals the same kernel-library function called directly (what the host constant
// folder does), bit for bit, on 10^4 random inputs; expected values are also computed
// independently (double arithmetic, which is exact for one float product or sum, and the
// analytic values of the lifts); a surface program followed by an emission program in one
// register file gives the emission program's Le unchanged; every error path stops the run with
// its status.
// Ref: docs/decisions/0005-material-model.md items 30, 31, 32, 35, 38, Verification 6;
// docs/decisions/0003-spectral-model-and-path-state.md d16 (grey lifts are analytic).
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "material/constant_block.h"
#include "sample/sobol.h"
#include "vm/interp.h"

namespace {

using ar::Op;
using ar::VmStatus;

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
  // A 3x2 sRGB image for TEX_IMAGE, slot 0 of the table; slot 1 is outside it.
  std::vector<uint8_t> texels = {0, 40, 255, 128, 7, 64, 200, 90, 13, 1, 254, 77, 33, 160, 99, 250, 20, 180};
  ar::DeviceImageView image = {};
  float srgb[ar::kSrgbTableSize];
  ar::SceneTables tables;
  Fixture() {
    ar::srgb_decode_table(srgb);
    tables.srgb_decode = srgb;
    image.texels = uint64_t(uintptr_t(texels.data()));
    image.width = 3, image.height = 2, image.channels = 3, image.encoding = uint32_t(ar::TexEncoding::SRGB);
    tables.images = &image;
    tables.image_count = 1;
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

uint32_t bits(float f) { return ar::bit_cast<uint32_t>(f); }
uint32_t fb(float f) { return bits(f); }

// A program is an entry table (4 words) and code; the one entry starts at word 4.
struct Prog {
  std::vector<uint32_t> w{0, 0, 0, 0};
  uint32_t here() const { return uint32_t(w.size()); }
  Prog& op(Op o, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, std::initializer_list<uint32_t> imm = {}) {
    w.push_back(uint32_t(o) | (a << 8) | (b << 16) | (c << 24));
    for (uint32_t v : imm) w.push_back(v);
    return *this;
  }
  Prog& raw(uint32_t word) {
    w.push_back(word);
    return *this;
  }
  Prog& end() { return op(Op::END); }
};

struct Rig {
  ar::VmRegs regs = {};
  ar::ShadeContext ctx = {};
  ar::OpenPBRParams rec = {};
  ar::Spec le = {};
  ar::SceneTables tables = fixture().tables;
  VmStatus run(const Prog& p, uint32_t entry = 4) {
    return ar::run_entry(p.w.data(), entry, &regs, ctx, tables, &rec, &le);
  }
};

// Row of random inputs: 16 registers in [-4, 4), P in [-4, 4), two UV sets in [0, 1), four
// wavelengths in [360, 830). The values are the sampler's, never a rand().
constexpr uint32_t kSeed = 0x57a11u;
float unit(uint32_t i, uint32_t d) { return ar::sample_independent(i, 0u, 0u, d, kSeed); }
void fill(Rig& g, float* row, uint32_t i) {
  for (uint32_t d = 0; d < 16; ++d) row[d] = unit(i, d) * 8.0f - 4.0f;
  for (uint32_t d = 0; d < 16; ++d) g.regs.r[d] = row[d];
  g.ctx.P = {unit(i, 16) * 8.0f - 4.0f, unit(i, 17) * 8.0f - 4.0f, unit(i, 18) * 8.0f - 4.0f};
  g.ctx.uv[0] = {unit(i, 19), unit(i, 20)};
  g.ctx.uv[1] = {unit(i, 21), unit(i, 22)};
  for (int l = 0; l < 4; ++l) g.ctx.lambda_t.nm[l] = float(360.0 + 470.0 * double(unit(i, 23 + uint32_t(l))));
}

float prod(float x, float y) { return float(double(x) * double(y)); }  // exact in double, one rounding
float sum(float x, float y) { return float(double(x) + double(y)); }

// Counts, per opcode, the inputs where the interpreter differs from the direct call ("fold")
// and from the independent expectation.
struct Tally {
  std::map<std::string, uint32_t> fold_bad, ref_bad;
  void fold(const char* op, bool same) { fold_bad[op] += same ? 0u : 1u; }
  void ref(const char* op, bool ok) { ref_bad[op] += ok ? 0u : 1u; }
};

// Bit-exact: the fold calls the same op functions from the same zeroed start.
bool same_spec(const ar::Spec& a, const ar::Spec& b) {
  for (int i = 0; i < ar::kLanes; ++i)
    if (bits(float(a.v[i])) != bits(float(b.v[i]))) return false;
  return true;
}

// For the independent reference only: Le accumulates onto a +0 start, so (+0 + a) + b differs
// from the reference sum a + b only when a = b = -0, where one is +0 and the other -0.
bool same_f(float a, float b) { return bits(a) == bits(b) || (a == 0.0f && b == 0.0f); }

bool in_range(const ar::Spec& s, float lo, float hi) {
  for (int i = 0; i < ar::kLanes; ++i)
    if (!(s.v[i] >= lo && s.v[i] <= hi)) return false;
  return true;
}

void poison(ar::VmRegs& r) { std::memset(&r, 0xff, sizeof r); }  // NaN in every register

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("vm/fold equals interpret") {
  constexpr uint32_t kN = 10000;
  Tally t;
  for (uint32_t i = 0; i < kN; ++i) {
    Rig g;
    float row[16];
    fill(g, row, i);
    const float* x = row;

    {  // CONST_F32 and CONST_F32X3: the immediates' bits arrive unchanged.
      Prog p;
      p.op(Op::CONST_F32, 16, 0, 0, {fb(x[0])}).op(Op::CONST_F32X3, 20, 0, 0, {fb(x[1]), fb(x[2]), fb(x[3])}).end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      t.fold("CONST_F32", bits(h.regs.r[16]) == bits(ar::op_const_f32(fb(x[0]))));
      t.ref("CONST_F32", bits(h.regs.r[16]) == bits(x[0]));
      bool fold3 = true, ref3 = true;
      for (int k = 0; k < 3; ++k) {
        fold3 = fold3 && bits(h.regs.r[20 + k]) == bits(ar::op_const_f32(fb(x[1 + k])));
        ref3 = ref3 && bits(h.regs.r[20 + k]) == bits(x[1 + k]);
      }
      t.fold("CONST_F32X3", fold3);
      t.ref("CONST_F32X3", ref3);
    }
    {  // MUL_F32
      Prog p;
      p.op(Op::MUL_F32, 16, 0, 1).end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      t.fold("MUL_F32", bits(h.regs.r[16]) == bits(ar::op_mul_f32(x[0], x[1])));
      t.ref("MUL_F32", bits(h.regs.r[16]) == bits(prod(x[0], x[1])));
    }
    {  // MUL_F32X3, destination apart from the sources and equal to the first source.
      Prog p;
      p.op(Op::MUL_F32X3, 16, 0, 3).op(Op::MUL_F32X3, 0, 0, 6).end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      float want[3], want2[3];
      ar::op_mul_f32x3(x, x + 3, want);
      ar::op_mul_f32x3(x, x + 6, want2);
      bool fold_ok = true, ref_ok = true;
      for (int k = 0; k < 3; ++k) {
        fold_ok = fold_ok && bits(h.regs.r[16 + k]) == bits(want[k]) && bits(h.regs.r[k]) == bits(want2[k]);
        ref_ok = ref_ok && bits(h.regs.r[16 + k]) == bits(prod(x[k], x[3 + k])) &&
                 bits(h.regs.r[k]) == bits(prod(x[k], x[6 + k]));
      }
      t.fold("MUL_F32X3", fold_ok);
      t.ref("MUL_F32X3", ref_ok);
    }
    {  // MUL_F32X3_F32: the scalar is a separate register.
      Prog p;
      p.op(Op::MUL_F32X3_F32, 16, 0, 3).end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      float want[3];
      ar::op_mul_f32x3_f32(x, x[3], want);
      bool fold_ok = true, ref_ok = true;
      for (int k = 0; k < 3; ++k) {
        fold_ok = fold_ok && bits(h.regs.r[16 + k]) == bits(want[k]);
        ref_ok = ref_ok && bits(h.regs.r[16 + k]) == bits(prod(x[k], x[3]));
      }
      t.fold("MUL_F32X3_F32", fold_ok);
      t.ref("MUL_F32X3_F32", ref_ok);
    }
    {  // ADD_F32X3
      Prog p;
      p.op(Op::ADD_F32X3, 16, 0, 3).end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      float want[3];
      ar::op_add_f32x3(x, x + 3, want);
      bool fold_ok = true, ref_ok = true;
      for (int k = 0; k < 3; ++k) {
        fold_ok = fold_ok && bits(h.regs.r[16 + k]) == bits(want[k]);
        ref_ok = ref_ok && bits(h.regs.r[16 + k]) == bits(sum(x[k], x[3 + k]));
      }
      t.fold("ADD_F32X3", fold_ok);
      t.ref("ADD_F32X3", ref_ok);
    }
    {  // LOAD_CTX_UV (both sets) and LOAD_CTX_P
      Prog p;
      p.op(Op::LOAD_CTX_UV, 16, 0).op(Op::LOAD_CTX_UV, 19, 1).op(Op::LOAD_CTX_P, 22).end();
      Rig h = g;
      h.regs.r[18] = 9.0f;  // z must be overwritten with 0
      REQUIRE(h.run(p) == VmStatus::OK);
      float u0[3], u1[3], pp[3];
      ar::op_load_ctx_uv(g.ctx, 0, u0);
      ar::op_load_ctx_uv(g.ctx, 1, u1);
      ar::op_load_ctx_p(g.ctx, pp);
      bool fold_ok = true;
      for (int k = 0; k < 3; ++k)
        fold_ok = fold_ok && bits(h.regs.r[16 + k]) == bits(u0[k]) && bits(h.regs.r[19 + k]) == bits(u1[k]) &&
                  bits(h.regs.r[22 + k]) == bits(pp[k]);
      t.fold("LOAD_CTX", fold_ok);
      t.ref("LOAD_CTX", h.regs.r[16] == float(g.ctx.uv[0].x) && h.regs.r[17] == float(g.ctx.uv[0].y) &&
                            bits(h.regs.r[18]) == 0u && h.regs.r[19] == float(g.ctx.uv[1].x) &&
                            h.regs.r[20] == float(g.ctx.uv[1].y) && bits(h.regs.r[21]) == 0u &&
                            h.regs.r[22] == float(g.ctx.P.x) && h.regs.r[23] == float(g.ctx.P.y) &&
                            h.regs.r[24] == float(g.ctx.P.z));
    }
    {  // TEX_IMAGE in every mode at (r0, r1) in [-4, 4), outside [0, 1) so Repeat and Extend both
       // act; slot 1 is outside the table and samples magenta.
      for (uint32_t mode = 0; mode < 4; ++mode) {
        Prog p;
        p.op(Op::TEX_IMAGE, 16, 0, 0, {0u, mode}).op(Op::TEX_IMAGE, 19, 0, 0, {1u, mode}).end();
        Rig h = g;
        REQUIRE(h.run(p) == VmStatus::OK);
        float want[3];
        ar::op_tex_image(g.tables, 0u, mode, x, want);
        bool fold_ok = true, ref_ok = h.regs.r[19] == 1.0f && bits(h.regs.r[20]) == 0u && h.regs.r[21] == 1.0f;
        for (int k = 0; k < 3; ++k) {
          fold_ok = fold_ok && bits(h.regs.r[16 + k]) == bits(want[k]);
          ref_ok = ref_ok && h.regs.r[16 + k] >= 0.0f && h.regs.r[16 + k] <= 1.0f;  // decoded bytes lie in [0, 1]
        }
        t.fold("TEX_IMAGE", fold_ok);
        t.ref("TEX_IMAGE", ref_ok);
      }
    }
    {  // RGB_LIFT with each semantic, at the context's wavelengths.
      Prog p;
      p.op(Op::RGB_LIFT, 16, 0, 0).op(Op::RGB_LIFT, 20, 0, 1).op(Op::RGB_LIFT, 24, 0, 2).end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      const ar::SpectrumSemantic sems[3] = {ar::SpectrumSemantic::REFLECTANCE, ar::SpectrumSemantic::UNBOUNDED,
                                            ar::SpectrumSemantic::ILLUMINANT};
      bool fold_ok = true, ref_ok = true;
      for (int s = 0; s < 3; ++s) {
        const ar::Spec want = ar::op_rgb_lift(g.tables, x, sems[s], g.ctx.lambda_t);
        ar::Spec got;
        for (int l = 0; l < 4; ++l) got.v[l] = h.regs.r[16 + 4 * s + l];
        fold_ok = fold_ok && same_spec(got, want);
        // A reflectance lies in [0, 1] whatever the input; the other two are never negative.
        ref_ok = ref_ok && (s == 0 ? in_range(got, 0.0f, 1.0f) : in_range(got, 0.0f, 1e30f));
      }
      t.fold("RGB_LIFT", fold_ok);
      t.ref("RGB_LIFT", ref_ok);
    }
    {  // STORE_SLOT: a scalar, an RGB colour (lifted at the slot's semantic), the IOR scalar, a
       // frame vector and a lifted spectrum stored as is.
      Prog p;
      p.op(Op::STORE_SLOT, 0, 0, 0, {uint32_t(ar::RecordSlot::base_metalness)})
          .op(Op::STORE_SLOT, 1, 0, 0, {uint32_t(ar::RecordSlot::base_color)})
          .op(Op::STORE_SLOT, 2, 0, 0, {uint32_t(ar::RecordSlot::subsurface_radius_scale)})
          .op(Op::STORE_SLOT, 4, 0, 0, {uint32_t(ar::RecordSlot::specular_ior)})
          .op(Op::STORE_SLOT, 5, 0, 0, {uint32_t(ar::RecordSlot::geometry_normal)})
          .op(Op::RGB_LIFT, 16, 8, 0)
          .op(Op::STORE_SLOT, 16, 0, 0, {uint32_t(ar::RecordSlot::fuzz_color) | ar::kStoreSlotSpecSource})
          .end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      const ar::Lambda& lam = g.ctx.lambda_t;
      const ar::Spec base = ar::op_rgb_lift(g.tables, x + 1, ar::SpectrumSemantic::REFLECTANCE, lam);
      const ar::Spec rs = ar::op_rgb_lift(g.tables, x + 2, ar::SpectrumSemantic::UNBOUNDED, lam);
      const ar::Spec fz = ar::op_rgb_lift(g.tables, x + 8, ar::SpectrumSemantic::REFLECTANCE, lam);
      bool fold_ok = bits(float(h.rec.base_metalness)) == bits(x[0]) && same_spec(h.rec.base_color, base) &&
                     same_spec(h.rec.subsurface_radius_scale, rs) &&
                     same_spec(h.rec.specular_ior, ar::spec_fill(x[4])) && same_spec(h.rec.fuzz_color, fz) &&
                     bits(float(h.rec.geometry_normal.x)) == bits(x[5]) &&
                     bits(float(h.rec.geometry_normal.y)) == bits(x[6]) &&
                     bits(float(h.rec.geometry_normal.z)) == bits(x[7]);
      t.fold("STORE_SLOT", fold_ok);
      t.ref("STORE_SLOT", in_range(h.rec.base_color, 0.0f, 1.0f) && in_range(h.rec.fuzz_color, 0.0f, 1.0f) &&
                              in_range(h.rec.subsurface_radius_scale, 0.0f, 1e30f) &&
                              h.rec.specular_ior.v[3] == x[4] && h.rec.base_metalness == x[0]);
    }
    {  // EMIT_LE twice: Le accumulates colour x strength, colour lifted as ILLUMINANT.
      Prog p;
      p.op(Op::EMIT_LE, 0).op(Op::EMIT_LE, 4).end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      // A volatile +0 start: the interpreter adds onto a +0 held in memory, and a compile-time
      // zero here would let this TU's optimiser simplify `0 + x` and change the sign of a zero.
      volatile float zero = 0.0f;
      ar::Spec want = ar::spec_fill(zero);
      ar::op_emit_le(g.tables, x, x[3], g.ctx.lambda_t, &want);
      ar::op_emit_le(g.tables, x + 4, x[7], g.ctx.lambda_t, &want);
      t.fold("EMIT_LE", same_spec(h.le, want));
      const ar::Spec s0 = ar::op_rgb_lift(g.tables, x, ar::SpectrumSemantic::ILLUMINANT, g.ctx.lambda_t);
      const ar::Spec s1 = ar::op_rgb_lift(g.tables, x + 4, ar::SpectrumSemantic::ILLUMINANT, g.ctx.lambda_t);
      bool ref_ok = true;
      for (int l = 0; l < 4; ++l) {
        const float a = prod(float(s0.v[l]), x[3]), b = prod(float(s1.v[l]), x[7]);
        ref_ok = ref_ok && same_f(float(h.le.v[l]), sum(a, b));
      }
      t.ref("EMIT_LE", ref_ok);
    }
  }
  for (const auto& kv : t.fold_bad) {
    INFO(kv.first);
    CHECK(kv.second == 0u);
  }
  for (const auto& kv : t.ref_bad) {
    INFO(kv.first);
    CHECK(kv.second == 0u);
  }
  // Every opcode of the M1a list was exercised, so a renamed key cannot hide a missing check.
  CHECK(t.fold_bad.size() == 11u);
  CHECK(t.ref_bad.size() == 11u);
}

TEST_CASE("vm/lift analytic values") {
  // Grey lifts are analytic (c0 = c1 = 0, S(0) = 1/2), so these are exact, not tolerances.
  const Fixture& fx = fixture();
  Rig g;
  g.ctx.lambda_t = ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}};
  struct Case {
    float rgb;
    ar::SpectrumSemantic sem;
    float want;
  };
  const Case cases[] = {
      {0.5f, ar::SpectrumSemantic::REFLECTANCE, 0.5f}, {1.0f, ar::SpectrumSemantic::REFLECTANCE, 1.0f},
      {0.0f, ar::SpectrumSemantic::REFLECTANCE, 0.0f}, {2.0f, ar::SpectrumSemantic::REFLECTANCE, 1.0f},  // clamp
      {-1.0f, ar::SpectrumSemantic::REFLECTANCE, 0.0f}, {2.0f, ar::SpectrumSemantic::UNBOUNDED, 2.0f},   // 2m S(1/2)
      {0.0f, ar::SpectrumSemantic::UNBOUNDED, 0.0f},   {7.0f, ar::SpectrumSemantic::UNBOUNDED, 7.0f}};
  for (const Case& c : cases) {
    Prog p;
    p.op(Op::CONST_F32X3, 0, 0, 0, {fb(c.rgb), fb(c.rgb), fb(c.rgb)}).op(Op::RGB_LIFT, 8, 0, uint32_t(c.sem)).end();
    Rig h = g;
    REQUIRE(h.run(p) == VmStatus::OK);
    for (int l = 0; l < 4; ++l) {
      INFO("rgb " << c.rgb << " semantic " << int(c.sem) << " lane " << l);
      CHECK(h.regs.r[8 + l] == c.want);
    }
  }
  // ILLUMINANT white (1, 1, 1) is S = 1/2 times scale 2 = 1, times the illuminant: at whole
  // nanometres that is the table entry itself (360 + i nm).
  Prog p;
  p.op(Op::CONST_F32X3, 0, 0, 0, {fb(1.0f), fb(1.0f), fb(1.0f)}).op(Op::RGB_LIFT, 8, 0, 2).end();
  Rig h = g;
  REQUIRE(h.run(p) == VmStatus::OK);
  const int idx[4] = {90, 160, 230, 300};
  for (int l = 0; l < 4; ++l) CHECK(h.regs.r[8 + l] == fx.obs.d65[idx[l]]);
  // A wavelength outside the observer table has no illuminant: the lift is exactly 0.
  Rig o = g;
  o.ctx.lambda_t = ar::Lambda{{300.0f, 900.0f, 450.0f, 450.0f}};
  REQUIRE(o.run(p) == VmStatus::OK);
  CHECK(o.regs.r[8] == 0.0f);
  CHECK(o.regs.r[9] == 0.0f);
  CHECK(o.regs.r[10] == fx.obs.d65[90]);
}

TEST_CASE("vm/store slot and emit values") {
  // The record's scalar slots are numbered in item 2(c)'s order, so slot i is the i-th Real.
  CHECK(uint32_t(ar::RecordSlot::base_weight) == 0u);
  CHECK(uint32_t(ar::RecordSlot::closure_weight) == 23u);
  CHECK(uint32_t(ar::RecordSlot::base_color) == 24u);
  CHECK(uint32_t(ar::RecordSlot::coat_color) == 32u);
  CHECK(uint32_t(ar::RecordSlot::geometry_normal) == 33u);
  CHECK(uint32_t(ar::RecordSlot::geometry_coat_tangent) == 36u);
  CHECK(uint32_t(ar::RecordSlot::COUNT_) == 37u);
  // The semantic each colour slot lifts with is item 10's table (the constant block's).
#define AR_X(n, s, sem) CHECK(ar::SpectrumSemantic::sem == ar::kSlotSemantic[int(ar::Slot::s)]);
  AR_RECORD_SPECTRAL(AR_X)
#undef AR_X

  Rig g;
  g.ctx.lambda_t = ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}};
  Prog p;
  for (uint32_t s = 0; s < 24; ++s)
    p.op(Op::CONST_F32, 0, 0, 0, {fb(float(s) + 0.25f)}).op(Op::STORE_SLOT, 0, 0, 0, {s});
  p.end();
  REQUIRE(g.run(p) == VmStatus::OK);
  const ar::Real* scalars = &g.rec.base_weight;
  for (uint32_t s = 0; s < 24; ++s) {
    INFO("scalar slot " << s);
    CHECK(scalars[s] == ar::Real(float(s) + 0.25f));
  }
  // The other fields were not touched.
  CHECK(g.rec.metal_ior == 0u);
  CHECK(g.rec.flags == 0u);

  // specular_ior is flat, a grey colour is its grey (exact), a spectrum register group is stored
  // as is, and the frame vectors take three registers.
  Rig h;
  h.ctx = g.ctx;
  Prog q;
  q.op(Op::CONST_F32, 0, 0, 0, {fb(1.45f)})
      .op(Op::STORE_SLOT, 0, 0, 0, {uint32_t(ar::RecordSlot::specular_ior)})
      .op(Op::CONST_F32X3, 1, 0, 0, {fb(0.5f), fb(0.5f), fb(0.5f)})
      .op(Op::STORE_SLOT, 1, 0, 0, {uint32_t(ar::RecordSlot::coat_color)})
      .op(Op::CONST_F32X3, 4, 0, 0, {fb(0.0f), fb(0.0f), fb(1.0f)})
      .op(Op::STORE_SLOT, 4, 0, 0, {uint32_t(ar::RecordSlot::geometry_coat_tangent)})
      .op(Op::CONST_F32, 8, 0, 0, {fb(0.1f)})
      .op(Op::CONST_F32, 9, 0, 0, {fb(0.2f)})
      .op(Op::CONST_F32, 10, 0, 0, {fb(0.3f)})
      .op(Op::CONST_F32, 11, 0, 0, {fb(0.4f)})
      .op(Op::STORE_SLOT, 8, 0, 0, {uint32_t(ar::RecordSlot::transmission_scatter) | ar::kStoreSlotSpecSource})
      .end();
  REQUIRE(h.run(q) == VmStatus::OK);
  for (int l = 0; l < 4; ++l) {
    CHECK(h.rec.specular_ior.v[l] == 1.45f);
    CHECK(h.rec.coat_color.v[l] == 0.5f);
  }
  CHECK(h.rec.geometry_coat_tangent.x == 0.0f);
  CHECK(h.rec.geometry_coat_tangent.y == 0.0f);
  CHECK(h.rec.geometry_coat_tangent.z == 1.0f);
  CHECK(h.rec.transmission_scatter.v[0] == 0.1f);
  CHECK(h.rec.transmission_scatter.v[1] == 0.2f);
  CHECK(h.rec.transmission_scatter.v[2] == 0.3f);
  CHECK(h.rec.transmission_scatter.v[3] == 0.4f);

  // EMIT_LE with white at whole nanometres: Le = illuminant(lambda) x strength, summed over two
  // nodes (1 x 2.5 and 1 x 0.5 give 3.0 times the table value in float arithmetic).
  Rig e;
  e.ctx.lambda_t = g.ctx.lambda_t;
  Prog w;
  w.op(Op::CONST_F32X3, 0, 0, 0, {fb(1.0f), fb(1.0f), fb(1.0f)})
      .op(Op::CONST_F32, 3, 0, 0, {fb(2.5f)})
      .op(Op::EMIT_LE, 0)
      .op(Op::CONST_F32, 3, 0, 0, {fb(0.5f)})
      .op(Op::EMIT_LE, 0)
      .end();
  REQUIRE(e.run(w) == VmStatus::OK);
  const int idx[4] = {90, 160, 230, 300};
  for (int l = 0; l < 4; ++l) {
    const float d = fixture().obs.d65[idx[l]];
    CHECK(float(e.le.v[l]) == d * 2.5f + d * 0.5f);
  }
}

TEST_CASE("vm/register file reuse") {
  // Item 32: kernels hold one register file, and an emission entry run after a surface entry
  // (NEE on an emissive triangle) must give the Le it gives alone. The emission program reads
  // only registers it writes, including the z that LOAD_CTX_UV must clear.
  Prog p;
  // Surface entry: leaves non-zero values in r8-r19 and its own Le and record.
  const uint32_t surface = p.here();
  p.op(Op::CONST_F32X3, 8, 0, 0, {fb(5.0f), fb(6.0f), fb(7.0f)})
      .op(Op::CONST_F32X3, 12, 0, 0, {fb(0.9f), fb(0.8f), fb(0.7f)})
      .op(Op::MUL_F32X3, 16, 8, 12)
      .op(Op::CONST_F32, 19, 0, 0, {fb(9.0f)})
      .op(Op::EMIT_LE, 16)
      .op(Op::STORE_SLOT, 8, 0, 0, {uint32_t(ar::RecordSlot::base_color)})
      .end();
  // Emission entry: Le = illuminant(uv, 0) * tint, strength 3.
  const uint32_t emission = p.here();
  p.op(Op::LOAD_CTX_UV, 8, 0)
      .op(Op::CONST_F32X3, 12, 0, 0, {fb(0.25f), fb(0.5f), fb(2.0f)})
      .op(Op::MUL_F32X3, 16, 8, 12)
      .op(Op::CONST_F32, 19, 0, 0, {fb(3.0f)})
      .op(Op::EMIT_LE, 16)
      .end();

  for (uint32_t i = 0; i < 1000; ++i) {
    Rig base;
    float row[16];
    fill(base, row, i);
    std::memset(&base.regs, 0, sizeof base.regs);

    Rig alone = base;
    REQUIRE(alone.run(p, emission) == VmStatus::OK);

    Rig after = base;
    REQUIRE(after.run(p, surface) == VmStatus::OK);
    CHECK(after.regs.r[10] == 7.0f);  // the surface entry did leave stale values behind
    after.le = ar::Spec{};
    REQUIRE(after.run(p, emission) == VmStatus::OK);
    CHECK(same_spec(after.le, alone.le));

    Rig dirty = base;
    poison(dirty.regs);
    REQUIRE(dirty.run(p, emission) == VmStatus::OK);
    CHECK(same_spec(dirty.le, alone.le));

    CHECK(alone.le.v[0] > 0.0f);  // the comparison is not between two zeros
  }
}

TEST_CASE("vm/control flow and entries") {
  Rig g;
  {  // JMP skips exactly `off` words, counted from the JMP word.
    Prog p;
    p.op(Op::CONST_F32, 1, 0, 0, {fb(1.0f)});
    const uint32_t jmp = p.here();
    p.op(Op::JMP, 0, 0, 0, {4u})                         // JMP word + its immediate
        .op(Op::CONST_F32, 1, 0, 0, {fb(2.0f)})          // skipped (2 words)
        .op(Op::CONST_F32, 2, 0, 0, {fb(3.0f)})          // the target
        .end();
    CHECK(p.w[jmp] == uint32_t(Op::JMP));
    Rig h;
    REQUIRE(h.run(p) == VmStatus::OK);
    CHECK(h.regs.r[1] == 1.0f);
    CHECK(h.regs.r[2] == 3.0f);
  }
  {  // END stops the entry; the words after it never run.
    Prog p;
    p.op(Op::CONST_F32, 1, 0, 0, {fb(1.0f)}).end().op(Op::CONST_F32, 2, 0, 0, {fb(2.0f)});
    Rig h;
    REQUIRE(h.run(p) == VmStatus::OK);
    CHECK(h.regs.r[1] == 1.0f);
    CHECK(h.regs.r[2] == 0.0f);
  }
  {  // Offset 0 is "absent or constant" (item 32): nothing runs, nothing changes.
    Prog p;
    p.op(Op::CONST_F32, 1, 0, 0, {fb(1.0f)}).end();
    Rig h;
    REQUIRE(h.run(p, 0) == VmStatus::OK);
    CHECK(h.regs.r[1] == 0.0f);
  }
  {  // The program is a pure function of its inputs: running it twice is the same.
    Prog p;
    p.op(Op::LOAD_CTX_P, 0).op(Op::MUL_F32X3, 3, 0, 0).op(Op::ADD_F32X3, 6, 3, 0).end();
    Rig a;
    a.ctx.P = {1.5f, -2.0f, 0.25f};
    Rig b = a;
    REQUIRE(a.run(p) == VmStatus::OK);
    REQUIRE(b.run(p) == VmStatus::OK);
    REQUIRE(b.run(p) == VmStatus::OK);
    CHECK(std::memcmp(&a.regs, &b.regs, sizeof a.regs) == 0);
    CHECK(a.regs.r[3] == 2.25f);   // 1.5^2
    CHECK(a.regs.r[4] == 4.0f);    // (-2)^2
    CHECK(a.regs.r[5] == 0.0625f);
    CHECK(a.regs.r[6] == 3.75f);   // 2.25 + 1.5
    CHECK(a.regs.r[7] == 2.0f);    // 4 - 2
    CHECK(a.regs.r[8] == 0.3125f); // 0.0625 + 0.25
  }
}

TEST_CASE("vm/errors stop the run") {
  struct Probe {
    const char* what;
    Prog p;
    VmStatus want;
  };
  const uint32_t kBadOpcodeByte = uint32_t(Op::COUNT_);
  std::vector<Probe> probes;
  auto add = [&](const char* what, Prog p, VmStatus want) { probes.push_back(Probe{what, std::move(p), want}); };

  add("opcode COUNT_", Prog().raw(kBadOpcodeByte), VmStatus::BAD_OPCODE);
  add("opcode 255", Prog().raw(255u), VmStatus::BAD_OPCODE);
  // Registers: a result or source that leaves the file by one register.
  add("MUL_F32 dst 32", Prog().op(Op::MUL_F32, 32, 0, 0), VmStatus::BAD_REGISTER);
  add("MUL_F32 src 32", Prog().op(Op::MUL_F32, 0, 32, 0), VmStatus::BAD_REGISTER);
  add("MUL_F32 src2 255", Prog().op(Op::MUL_F32, 0, 0, 255), VmStatus::BAD_REGISTER);
  add("CONST_F32 dst 32", Prog().op(Op::CONST_F32, 32, 0, 0, {0u}), VmStatus::BAD_REGISTER);
  add("CONST_F32X3 dst 30", Prog().op(Op::CONST_F32X3, 30, 0, 0, {0u, 0u, 0u}), VmStatus::BAD_REGISTER);
  add("MUL_F32X3 dst 30", Prog().op(Op::MUL_F32X3, 30, 0, 0), VmStatus::BAD_REGISTER);
  add("MUL_F32X3_F32 scalar 32", Prog().op(Op::MUL_F32X3_F32, 0, 0, 32), VmStatus::BAD_REGISTER);
  add("ADD_F32X3 src 30", Prog().op(Op::ADD_F32X3, 0, 30, 0), VmStatus::BAD_REGISTER);
  add("LOAD_CTX_P dst 30", Prog().op(Op::LOAD_CTX_P, 30), VmStatus::BAD_REGISTER);
  add("LOAD_CTX_UV dst 30", Prog().op(Op::LOAD_CTX_UV, 30, 0), VmStatus::BAD_REGISTER);
  add("TEX_IMAGE dst 30", Prog().op(Op::TEX_IMAGE, 30, 0, 0, {0u, 0u}), VmStatus::BAD_REGISTER);
  add("TEX_IMAGE uv 30", Prog().op(Op::TEX_IMAGE, 0, 30, 0, {0u, 0u}), VmStatus::BAD_REGISTER);
  add("RGB_LIFT dst 29", Prog().op(Op::RGB_LIFT, 29, 0, 0), VmStatus::BAD_REGISTER);
  add("RGB_LIFT src 30", Prog().op(Op::RGB_LIFT, 0, 30, 0), VmStatus::BAD_REGISTER);
  add("EMIT_LE needs 4 (a = 29)", Prog().op(Op::EMIT_LE, 29), VmStatus::BAD_REGISTER);
  add("STORE_SLOT colour from r30", Prog().op(Op::STORE_SLOT, 30, 0, 0, {uint32_t(ar::RecordSlot::base_color)}),
      VmStatus::BAD_REGISTER);
  add("STORE_SLOT spectrum from r29",
      Prog().op(Op::STORE_SLOT, 29, 0, 0, {uint32_t(ar::RecordSlot::base_color) | ar::kStoreSlotSpecSource}),
      VmStatus::BAD_REGISTER);
  add("STORE_SLOT src 32", Prog().op(Op::STORE_SLOT, 32, 0, 0, {0u}), VmStatus::BAD_REGISTER);
  // Operands the op does not accept.
  add("LOAD_CTX_UV set 2", Prog().op(Op::LOAD_CTX_UV, 0, 2), VmStatus::BAD_OPERAND);
  add("TEX_IMAGE mode 4", Prog().op(Op::TEX_IMAGE, 0, 3, 0, {0u, 4u}), VmStatus::BAD_OPERAND);
  add("RGB_LIFT semantic IOR", Prog().op(Op::RGB_LIFT, 0, 4, 3), VmStatus::BAD_OPERAND);
  add("RGB_LIFT semantic 255", Prog().op(Op::RGB_LIFT, 0, 4, 255), VmStatus::BAD_OPERAND);
  add("STORE_SLOT slot 37", Prog().op(Op::STORE_SLOT, 0, 0, 0, {37u}), VmStatus::BAD_OPERAND);
  add("STORE_SLOT slot 255", Prog().op(Op::STORE_SLOT, 0, 0, 0, {255u}), VmStatus::BAD_OPERAND);
  add("STORE_SLOT stray bit 9", Prog().op(Op::STORE_SLOT, 0, 0, 0, {512u}), VmStatus::BAD_OPERAND);
  add("STORE_SLOT spectrum into a scalar slot", Prog().op(Op::STORE_SLOT, 0, 0, 0, {ar::kStoreSlotSpecSource}),
      VmStatus::BAD_OPERAND);
  add("STORE_SLOT spectrum into a frame slot",
      Prog().op(Op::STORE_SLOT, 0, 0, 0, {uint32_t(ar::RecordSlot::geometry_normal) | ar::kStoreSlotSpecSource}),
      VmStatus::BAD_OPERAND);
  // Control flow.
  add("JMP 0", Prog().op(Op::JMP, 0, 0, 0, {0u}), VmStatus::BAD_JUMP);
  add("JMP -1", Prog().op(Op::JMP, 0, 0, 0, {0xffffffffu}), VmStatus::BAD_JUMP);
  add("JMP INT_MIN", Prog().op(Op::JMP, 0, 0, 0, {0x80000000u}), VmStatus::BAD_JUMP);
  add("UNSUPPORTED", Prog().op(Op::UNSUPPORTED, 0, 0, 0, {7u}), VmStatus::UNSUPPORTED);
#if defined(AR_GATE_HOOKS)
  add("TEST_INJECT_NAN dst 32", Prog().op(Op::TEST_INJECT_NAN, 32), VmStatus::BAD_REGISTER);
#else
  add("TEST_INJECT_NAN without hooks", Prog().op(Op::TEST_INJECT_NAN, 0), VmStatus::BAD_OPCODE);
#endif

  for (Probe& pr : probes) {
    INFO(pr.what);
    // The faulty instruction is followed by a CONST that must not run, and the failure leaves the
    // record and Le alone.
    pr.p.op(Op::CONST_F32, 1, 0, 0, {fb(5.0f)}).end();
    Rig g;
    g.ctx.lambda_t = ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}};
    for (int k = 0; k < 8; ++k) g.regs.r[k] = float(k);
    Rig h = g;
    CHECK(h.run(pr.p) == pr.want);
    CHECK(h.regs.r[1] != 5.0f);
    CHECK(std::memcmp(&h.rec, &g.rec, sizeof g.rec) == 0);
    CHECK(std::memcmp(&h.le, &g.le, sizeof g.le) == 0);
  }

  // Missing outputs and tables are named, not ignored.
  {
    Prog p;
    p.op(Op::STORE_SLOT, 0, 0, 0, {0u}).end();
    Rig g;
    CHECK(ar::run_entry(p.w.data(), 4, &g.regs, g.ctx, g.tables, nullptr, &g.le) == VmStatus::NULL_POINTER);
    Prog e;
    e.op(Op::EMIT_LE, 0).end();
    CHECK(ar::run_entry(e.w.data(), 4, &g.regs, g.ctx, g.tables, &g.rec, nullptr) == VmStatus::NULL_POINTER);
    ar::SceneTables none = {};
    CHECK(ar::run_entry(e.w.data(), 4, &g.regs, g.ctx, none, &g.rec, &g.le) == VmStatus::NULL_POINTER);
    Prog l;
    l.op(Op::RGB_LIFT, 8, 0, 0).end();
    CHECK(ar::run_entry(l.w.data(), 4, &g.regs, g.ctx, none, &g.rec, &g.le) == VmStatus::NULL_POINTER);
    CHECK(ar::run_entry(p.w.data(), 4, &g.regs, g.ctx, none, &g.rec, &g.le) == VmStatus::NULL_POINTER);
    // No image table is a scene without images: magenta. A count without its array is named.
    Prog x;
    x.op(Op::TEX_IMAGE, 8, 0, 0, {0u, 0u}).end();
    CHECK(ar::run_entry(x.w.data(), 4, &g.regs, g.ctx, none, &g.rec, &g.le) == VmStatus::OK);
    CHECK((g.regs.r[8] == 1.0f && g.regs.r[9] == 0.0f && g.regs.r[10] == 1.0f));
    ar::SceneTables dangling = {};
    dangling.image_count = 1;
    CHECK(ar::run_entry(x.w.data(), 4, &g.regs, g.ctx, dangling, &g.rec, &g.le) == VmStatus::NULL_POINTER);
  }

  // The last register of each width is fine: the checks are exact, not conservative.
  {
    Prog p;
    p.op(Op::CONST_F32, 31, 0, 0, {fb(1.0f)})
        .op(Op::CONST_F32X3, 29, 0, 0, {fb(1.0f), fb(2.0f), fb(3.0f)})
        .op(Op::ADD_F32X3, 29, 29, 29)
        .op(Op::RGB_LIFT, 28, 29, 0)
        .op(Op::CONST_F32, 31, 0, 0, {fb(1.0f)})
        .op(Op::EMIT_LE, 28)
        .end();
    Rig g;
    g.ctx.lambda_t = ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}};
    CHECK(g.run(p) == VmStatus::OK);
    // (2, 4, 6) lifts as a reflectance to white, and white emission is the illuminant itself.
    CHECK(g.regs.r[31] == 1.0f);
    CHECK(float(g.le.v[0]) == fixture().obs.d65[90]);
  }

#if defined(AR_GATE_HOOKS)
  {  // The hook writes a NaN into the named register and nothing else.
    Prog p;
    p.op(Op::TEST_INJECT_NAN, 5).end();
    Rig g;
    REQUIRE(g.run(p) == VmStatus::OK);
    CHECK(std::isnan(g.regs.r[5]));
    CHECK(g.regs.r[4] == 0.0f);
    CHECK(g.regs.r[6] == 0.0f);
  }
#endif
}

TEST_CASE("vm/opcode table is wired") {
  // Handler macros exist for every ops.def row (the build proves it); the table and the test's
  // encoding agree on the opcode numbers the programs above use.
  CHECK(uint32_t(Op::END) == 0u);
  CHECK(uint32_t(Op::JMP) == 1u);
  CHECK(uint32_t(Op::CONST_F32) == 2u);
  CHECK(uint32_t(Op::EMIT_LE) == 13u);
  CHECK(uint32_t(Op::COUNT_) == 16u);
}

}  // TEST_SUITE

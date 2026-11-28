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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "material/constant_block.h"
#include "medium/medium_eval.h"
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
    for (int k = 0; k < 3; ++k) table.y_row[k] = obs.y_row[k];  // the host fills it from M_ws
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
  float su = 0.5f;  // the entry's selection uniform (BRANCH_MIX)
  VmStatus run(const Prog& p, uint32_t entry = 4) {
    return ar::run_entry(p.w.data(), entry, &regs, ctx, tables, &rec, &le, &su);
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

// The colour TEX_ENVIRONMENT must return for direction d on the fixture image, computed in double from
// the Cycles formulas (u = (atan2(y, x) - pi) / -2pi, v = (acos(z / |d|) - pi) / -pi), repeating, an sRGB
// byte decoded by the sRGB function after any filtering. False for a Closest lookup within 1e-3 of a texel
// edge, where float and double may pick different texels.
bool env_reference(const float* d, uint32_t mode, double* out) {
  const Fixture& f = fixture();
  const double pi = 3.14159265358979323846;
  const double n = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
  const double u = (std::atan2(double(d[1]), double(d[0])) - pi) / (-2.0 * pi), v = (std::acos(double(d[2]) / n) - pi) / (-pi);
  const int w = int(f.image.width), h = int(f.image.height);
  const auto texel = [&](int x, int y, int k) {
    x = ((x % w) + w) % w, y = ((y % h) + h) % h;
    return double(f.texels[size_t((y * w + x) * 3 + k)]) / 255.0;
  };
  const auto decode = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
  if (mode == 0u) {
    const double x = u * w, y = v * h;
    if (std::fabs(x - std::round(x)) < 1e-3 || std::fabs(y - std::round(y)) < 1e-3) return false;
    for (int k = 0; k < 3; ++k) out[k] = decode(texel(int(std::floor(x)), int(std::floor(y)), k));
    return true;
  }
  const double x = u * w - 0.5, y = v * h - 0.5;
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  const double tx = x - ix, ty = y - iy;
  for (int k = 0; k < 3; ++k)
    out[k] = decode((1 - ty) * (1 - tx) * texel(ix, iy, k) + (1 - ty) * tx * texel(ix + 1, iy, k) +
                    ty * (1 - tx) * texel(ix, iy + 1, k) + ty * tx * texel(ix + 1, iy + 1, k));
  return true;
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

// Independent double evaluation of a Jakob-Hanika sigmoid from its stored float coefficients.
double sigmoid_d(const float* c, double lam) {
  const double x = (double(c[0]) * lam + double(c[1])) * lam + double(c[2]);
  if (std::isinf(x)) return x > 0 ? 1.0 : 0.0;
  return 0.5 + x / (2.0 * std::sqrt(1.0 + x * x));
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
    {  // CHANNEL_F32X3: each channel of r3..r5, one into the source's own first register.
      Prog p;
      p.op(Op::CHANNEL_F32X3, 16, 3, 0, {0u}).op(Op::CHANNEL_F32X3, 17, 3, 0, {1u}).op(Op::CHANNEL_F32X3, 3, 3, 0, {2u}).end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      t.fold("CHANNEL_F32X3", bits(h.regs.r[16]) == bits(ar::op_channel_f32x3(x + 3, 0u)) &&
                                  bits(h.regs.r[17]) == bits(ar::op_channel_f32x3(x + 3, 1u)) &&
                                  bits(h.regs.r[3]) == bits(ar::op_channel_f32x3(x + 3, 2u)));
      t.ref("CHANNEL_F32X3", bits(h.regs.r[16]) == bits(x[3]) && bits(h.regs.r[17]) == bits(x[4]) && bits(h.regs.r[3]) == bits(x[5]));
    }
    {  // COLOR_TO_F32: the luminance of r3..r5, once into a fresh register and once onto its own source.
      Prog p;
      p.op(Op::COLOR_TO_F32, 16, 3).op(Op::COLOR_TO_F32, 3, 3).end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      const float* y = g.tables.rgb2spec->y_row;
      const float want = ar::op_color_to_f32(x + 3, y);
      // Independent: the same dot product in double, rounded once (the handler rounds in float).
      const double ref = double(x[3]) * double(y[0]) + double(x[4]) * double(y[1]) + double(x[5]) * double(y[2]);
      t.fold("COLOR_TO_F32", bits(h.regs.r[16]) == bits(want) && bits(h.regs.r[3]) == bits(want));
      t.ref("COLOR_TO_F32", std::fabs(double(h.regs.r[16]) - ref) <= 5e-7 * (std::fabs(double(x[3])) + std::fabs(double(x[4])) + std::fabs(double(x[5]))));
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
    {  // TEX_ENVIRONMENT in both modes at the direction (r0, r1, r2): the interpreter equals the direct
       // call bit for bit, and the colour equals an independent double-precision lookup (the Cycles
       // mapping, Closest or bilinear on the fixture's 3x2 sRGB image, repeating). Slot 1 is outside the table.
      for (uint32_t mode = 0; mode < 2; ++mode) {
        Prog p;
        p.op(Op::TEX_ENVIRONMENT, 16, 0, 0, {0u, mode}).op(Op::TEX_ENVIRONMENT, 19, 0, 0, {1u, mode}).end();
        Rig h = g;
        REQUIRE(h.run(p) == VmStatus::OK);
        float want[3];
        ar::op_tex_environment(g.tables, 0u, mode, x, want);
        bool fold_ok = true, ref_ok = h.regs.r[19] == 1.0f && bits(h.regs.r[20]) == 0u && h.regs.r[21] == 1.0f;
        for (int k = 0; k < 3; ++k) fold_ok = fold_ok && bits(h.regs.r[16 + k]) == bits(want[k]);
        double ref[3];
        if (env_reference(x, mode, ref))
          for (int k = 0; k < 3; ++k) ref_ok = ref_ok && std::fabs(double(h.regs.r[16 + k]) - ref[k]) <= 2e-6;
        t.fold("TEX_ENVIRONMENT", fold_ok);
        t.ref("TEX_ENVIRONMENT", ref_ok);
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
    {  // BLACKBODY and CAUCHY_ABBE: immediates in, four registers out, through resolve()'s functions.
      const float T = 1500.0f + 10000.0f * unit(i, 40), scale = 0.1f + unit(i, 41);
      const float nd = 1.2f + unit(i, 42), vd = 10.0f + 80.0f * unit(i, 43), ds = unit(i, 44);
      Prog p;
      p.op(Op::BLACKBODY, 16, 0, 0, {fb(T), fb(scale)}).op(Op::CAUCHY_ABBE, 20, 0, 0, {fb(nd), fb(vd), fb(ds)}).end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      const ar::Spec bb = ar::blackbody_spec(T, scale, g.ctx.lambda_t);
      const ar::Spec ca = ar::cauchy_abbe_ior(nd, vd, ds, g.ctx.lambda_t);
      bool bb_fold = true, bb_ref = true, ca_fold = true, ca_ref = true;
      for (int l = 0; l < 4; ++l) {
        bb_fold = bb_fold && bits(h.regs.r[16 + l]) == bits(float(bb.v[l]));
        ca_fold = ca_fold && bits(h.regs.r[20 + l]) == bits(float(ca.v[l]));
        const double lam = double(g.ctx.lambda_t.nm[l]);
        const double want_bb = double(scale) * ar::planck_radiance_t<double>(lam, double(T));
        bb_ref = bb_ref && std::fabs(double(h.regs.r[16 + l]) - want_bb) <= 1e-5 * want_bb;
        const double want_ca = ar::cauchy_abbe_ior_t<double>(double(nd), double(vd), double(ds), lam);
        ca_ref = ca_ref && std::fabs(double(h.regs.r[20 + l]) - want_ca) <= 1e-6 * want_ca;
      }
      t.fold("BLACKBODY", bb_fold);
      t.ref("BLACKBODY", bb_ref);
      t.fold("CAUCHY_ABBE", ca_fold);
      t.ref("CAUCHY_ABBE", ca_ref);
    }
    {  // VOLUME_COEFFS through the volume entry: an absorber and two scatter lobes from random colours
       // and densities. The fold is the direct call of the shared function; the independent value is
       // density * S(colour) for a scatterer and density * (1 - S(colour)) for the absorber, S the
       // sigmoid of the colour's own albedo coefficients, evaluated in double.
      const float d_a = unit(i, 50) * 3.0f, d_s0 = unit(i, 51) * 3.0f, d_s1 = unit(i, 52) * 3.0f;
      const ar::Rgb ca{{ar::Real(x[0]), ar::Real(x[1]), ar::Real(x[2])}}, c0{{ar::Real(x[3]), ar::Real(x[4]), ar::Real(x[5])}},
          c1{{ar::Real(x[6]), ar::Real(x[7]), ar::Real(x[8])}};
      uint32_t fl = 0;
      ar::MediumBlock blk = ar::medium_block_vacuum();
      blk.sigma_a = ar::volume_term_ref(*g.tables.rgb2spec, ca, d_a, ar::VolumeTerm::ABSORPTION, &fl);
      blk.sigma_s[0] = ar::volume_term_ref(*g.tables.rgb2spec, c0, d_s0, ar::VolumeTerm::SCATTER, &fl);
      blk.sigma_s[1] = ar::volume_term_ref(*g.tables.rgb2spec, c1, d_s1, ar::VolumeTerm::SCATTER, &fl);
      blk.g[0] = 0.6f;
      blk.g[1] = -0.25f;
      blk.lobe_count = 2;
      Prog p;
      p.op(Op::VOLUME_COEFFS).end();
      ar::MediumSample got = {}, want = {};
      Rig h = g;
      REQUIRE(ar::run_volume_entry(p.w.data(), 4, &h.regs, g.ctx, g.tables, &blk, &got) == VmStatus::OK);
      REQUIRE(ar::op_volume_coeffs(blk, g.tables.observer, g.ctx.lambda_t, &want) == VmStatus::OK);
      bool fold_ok = same_spec(got.sigma_a, want.sigma_a) && same_spec(got.sigma_s, want.sigma_s) &&
                     same_spec(got.emission, want.emission) && got.phase[0].p0 == 0.6f && got.phase[1].p0 == -0.25f &&
                     same_spec(got.phase[0].weight, want.phase[0].weight) && same_spec(got.phase[1].weight, want.phase[1].weight);
      t.fold("VOLUME_COEFFS", fold_ok);
      const ar::Rgb cols[3] = {ca, c0, c1};
      const float dens[3] = {d_a, d_s0, d_s1};
      bool ref_ok = true;
      for (int k = 0; k < 3; ++k) {
        uint32_t f2 = 0;
        const ar::Upsampled u = ar::albedo_coeffs(*g.tables.rgb2spec, cols[k], &f2);
        const float c[3] = {float(u.c.c[0]), float(u.c.c[1]), float(u.c.c[2])};
        for (int l = 0; l < 4; ++l) {
          const double sg = sigmoid_d(c, double(g.ctx.lambda_t.nm[l]));
          const double w = double(dens[k]) * (k == 0 ? 1.0 - sg : sg);
          const ar::Spec& have = k == 0 ? got.sigma_a : got.phase[k - 1].weight;
          // Derived bound, in units of the density: x = c0 l^2 + c1 l + c2 is two fma roundings,
          // |dx| <= 2 u T with T = |c0| l^2 + |c1| l + |c2|; |S'| <= 1/2; the sigmoid's own
          // roundings and the product add at most 5 u; u is 2^-24 (float) plus 2^-53 (the double
          // reference). An infinite coefficient (a grey 0 or 1) is exact.
          const double lam = double(g.ctx.lambda_t.nm[l]);
          const double tt = std::fabs(double(c[0])) * lam * lam + std::fabs(double(c[1])) * lam + std::fabs(double(c[2]));
          const double bound = std::isinf(tt) ? 0.0 : (5.9604644775390625e-8 + 1.1102230246251565e-16) * (tt + 5.0) * double(dens[k]);
          ref_ok = ref_ok && std::fabs(double(have.v[l]) - w) <= bound;
        }
      }
      t.ref("VOLUME_COEFFS", ref_ok);
    }
    {  // ADD_F32 and SUB_F32, the second into its own first source.
      Prog p;
      p.op(Op::ADD_F32, 16, 0, 1).op(Op::SUB_F32, 2, 2, 3).end();
      Rig h = g;
      REQUIRE(h.run(p) == VmStatus::OK);
      t.fold("ADD_F32", bits(h.regs.r[16]) == bits(ar::op_add_f32(x[0], x[1])));
      t.ref("ADD_F32", bits(h.regs.r[16]) == bits(sum(x[0], x[1])));
      t.fold("SUB_F32", bits(h.regs.r[2]) == bits(ar::op_sub_f32(x[2], x[3])));
      t.ref("SUB_F32", bits(h.regs.r[2]) == bits(float(double(x[2]) - double(x[3]))));
    }
    {  // BRANCH_MIX: the side (r17 = 1 falls through, 2 jumps) and the rescaled selection uniform,
       // for factors in [-0.25, 1.25) so both clamps are hit. The reference is item 31's formula
       // in double with 1 - f held in float, as the register file holds it (exact for f >= 0.5;
       // otherwise u - (1 - f) cancels and the float rounding of 1 - f would be divided by a small
       // f), kept below 1 as pbrt-v4 SampleDiscrete does.
      const float u = unit(i, 45), f = unit(i, 46) * 1.5f - 0.25f;
      Prog p;
      p.op(Op::BRANCH_MIX, 0, 0, 0, {5u})           // words 0-1
          .op(Op::CONST_F32, 17, 0, 0, {fb(1.0f)})   // 2-3: first child
          .end()                                     // 4
          .op(Op::CONST_F32, 17, 0, 0, {fb(2.0f)})   // 5-6: second child
          .end();
      Rig h = g;
      h.regs.r[0] = f, h.su = u;
      REQUIRE(h.run(p) == VmStatus::OK);
      float uf = u;
      const bool second = ar::op_branch_mix(f, &uf);
      t.fold("BRANCH_MIX", h.regs.r[17] == (second ? 2.0f : 1.0f) && bits(h.su) == bits(uf));
      const double fc = std::min(std::max(double(f), 0.0), 1.0), p_first = double(1.0f - float(fc));
      const bool want_second = !(double(u) < p_first);
      const double want_u = std::min(want_second ? (double(u) - p_first) / fc : double(u) / p_first,
                                     double(ar::kBranchOneMinusEpsilon));
      t.ref("BRANCH_MIX", h.regs.r[17] == (want_second ? 2.0f : 1.0f) &&
                              std::fabs(double(h.su) - want_u) <= 2e-7 * want_u && h.su >= 0.0f && h.su < 1.0f);
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
  // Every opcode of the table was exercised, so a renamed key cannot hide a missing check.
  CHECK(t.fold_bad.size() == 20u);
  CHECK(t.ref_bad.size() == 20u);
}

TEST_CASE("vm/div and clamp fold as they interpret") {
  // DIV_F32 and CLAMP_F32 (0005 items 31, 38): the interpreter equals the shared function bit for
  // bit, as the host fold calls it, on 1e4 inputs, and both equal an independent double evaluation.
  // The inputs include zero denominators (IEEE, no special case: the op adds no guard) and a NaN, an
  // infinity and the signed zeros through the clamp, where NaN must pass through. Each op also runs
  // in place, and a CONST after the clamp proves it advanced past its two immediates.
  constexpr uint32_t kN = 10000;
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  volatile float volatile_zero = 0.0f;  // a runtime zero: MSVC's C4723 rejects a divide it can see is by 0
  const float zero = volatile_zero;
  auto same = [](float a, float b) { return (std::isnan(a) && std::isnan(b)) || bits(a) == bits(b) || (a == 0.0f && b == 0.0f); };
  Tally t;
  for (uint32_t i = 0; i < kN; ++i) {
    Rig g;
    float row[16];
    fill(g, row, i);
    const float* x = row;
    const float den = i % 97u == 0u ? ((i & 1u) ? -zero : zero) : x[1];
    const float specials[8] = {nan, inf, -inf, -0.0f, 0.0f, 1.0f, -1.0f, 0.5f};
    const float v = i % 11u < 8u && i % 3u == 0u ? specials[i % 11u] : x[2];
    // The compiler's [0, 1], and random pairs, every fifth one reversed (min(max(x, lo), hi) is hi).
    const bool unit_range = i % 2u == 0u;
    const float lo = unit_range ? 0.0f : x[4], hi = unit_range ? 1.0f : (i % 5u == 0u ? x[5] : x[4] + std::fabs(x[5]));
    g.regs.r[0] = x[0], g.regs.r[1] = den, g.regs.r[2] = v, g.regs.r[3] = x[3];
    Prog p;
    p.op(Op::DIV_F32, 16, 0, 1)
        .op(Op::DIV_F32, 3, 3, 1)
        .op(Op::CLAMP_F32, 17, 2, 0, {fb(lo), fb(hi)})
        .op(Op::CLAMP_F32, 2, 2, 0, {fb(0.0f), fb(1.0f)})
        .op(Op::CONST_F32, 18, 0, 0, {fb(7.0f)})
        .end();
    Rig h = g;
    REQUIRE(h.run(p) == VmStatus::OK);
    const float want_div = ar::op_div_f32(x[0], den), want_div3 = ar::op_div_f32(x[3], den);
    t.fold("DIV_F32", bits(h.regs.r[16]) == bits(want_div) && bits(h.regs.r[3]) == bits(want_div3));
    t.ref("DIV_F32", same(h.regs.r[16], float(double(x[0]) / double(den))) && same(h.regs.r[3], float(double(x[3]) / double(den))));
    auto clamp_ref = [](float a, float l, float u) {
      return std::isnan(a) ? a : float(std::min(std::max(double(a), double(l)), double(u)));
    };
    t.fold("CLAMP_F32", bits(h.regs.r[17]) == bits(ar::op_clamp_f32(v, lo, hi)) && bits(h.regs.r[2]) == bits(ar::op_clamp_f32(v, 0.0f, 1.0f)));
    t.ref("CLAMP_F32", same(h.regs.r[17], clamp_ref(v, lo, hi)) && same(h.regs.r[2], clamp_ref(v, 0.0f, 1.0f)) && h.regs.r[18] == 7.0f);
  }
  for (const auto& kv : t.fold_bad) {
    INFO(kv.first);
    CHECK(kv.second == 0u);
  }
  for (const auto& kv : t.ref_bad) {
    INFO(kv.first);
    CHECK(kv.second == 0u);
  }
  CHECK(t.fold_bad.size() == 2u);
  CHECK(t.ref_bad.size() == 2u);
  // NaN passes through the clamp, and a zero denominator is the IEEE quotient.
  CHECK(std::isnan(ar::op_clamp_f32(nan, 0.0f, 1.0f)));
  CHECK(ar::op_clamp_f32(1.5f, 0.0f, 1.0f) == 1.0f);
  CHECK(ar::op_clamp_f32(-0.5f, 0.0f, 1.0f) == 0.0f);
  CHECK(ar::op_div_f32(1.0f, zero) == inf);
  CHECK(std::isnan(ar::op_div_f32(zero, zero)));
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
  {  // BRANCH_MIX: factor 0 always keeps the first child and factor 1 always takes the second,
     // with u unchanged; 0.25 splits u at 0.75 and rescales each side back onto [0, 1).
    struct Case {
      float f, u;
      bool second;
      float u_out;
    };
    const Case cases[] = {{0.0f, 0.0f, false, 0.0f},   {0.0f, 0.9f, false, 0.9f},  {1.0f, 0.0f, true, 0.0f},
                          {1.0f, 0.9f, true, 0.9f},    {0.25f, 0.5f, false, 0.5f / 0.75f}, {0.25f, 0.875f, true, 0.5f},
                          {-3.0f, 0.5f, false, 0.5f},  {7.0f, 0.5f, true, 0.5f}};
    for (const Case& c : cases) {
      Prog p;
      p.op(Op::BRANCH_MIX, 0, 0, 0, {5u}).op(Op::CONST_F32, 1, 0, 0, {fb(1.0f)}).end().op(Op::CONST_F32, 1, 0, 0, {fb(2.0f)}).end();
      Rig h;
      h.regs.r[0] = c.f, h.su = c.u;
      INFO(c.f << " " << c.u);
      REQUIRE(h.run(p) == VmStatus::OK);
      CHECK(h.regs.r[1] == (c.second ? 2.0f : 1.0f));
      CHECK(h.su == doctest::Approx(c.u_out).epsilon(1e-6));
    }
    // A NaN factor or a uniform outside [0, 1) stops the run instead of picking a side.
    for (const float bad_f : {std::nanf(""), 0.5f})
      for (const float bad_u : {0.5f, -0.25f, 1.0f, std::nanf("")}) {
        if (bad_f == 0.5f && bad_u == 0.5f) continue;
        Prog p;
        p.op(Op::BRANCH_MIX, 0, 0, 0, {2u}).op(Op::CONST_F32, 1, 0, 0, {fb(1.0f)}).end();
        Rig h;
        h.regs.r[0] = bad_f, h.su = bad_u;
        CHECK(h.run(p) == VmStatus::BAD_OPERAND);
        CHECK(h.regs.r[1] == 0.0f);
      }
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
  add("TEX_ENVIRONMENT dst 30", Prog().op(Op::TEX_ENVIRONMENT, 30, 0, 0, {0u, 0u}), VmStatus::BAD_REGISTER);
  add("TEX_ENVIRONMENT direction 30", Prog().op(Op::TEX_ENVIRONMENT, 0, 30, 0, {0u, 0u}), VmStatus::BAD_REGISTER);
  add("RGB_LIFT dst 29", Prog().op(Op::RGB_LIFT, 29, 0, 0), VmStatus::BAD_REGISTER);
  add("RGB_LIFT src 30", Prog().op(Op::RGB_LIFT, 0, 30, 0), VmStatus::BAD_REGISTER);
  add("EMIT_LE needs 4 (a = 29)", Prog().op(Op::EMIT_LE, 29), VmStatus::BAD_REGISTER);
  add("STORE_SLOT colour from r30", Prog().op(Op::STORE_SLOT, 30, 0, 0, {uint32_t(ar::RecordSlot::base_color)}),
      VmStatus::BAD_REGISTER);
  add("STORE_SLOT spectrum from r29",
      Prog().op(Op::STORE_SLOT, 29, 0, 0, {uint32_t(ar::RecordSlot::base_color) | ar::kStoreSlotSpecSource}),
      VmStatus::BAD_REGISTER);
  add("CHANNEL_F32X3 dst 32", Prog().op(Op::CHANNEL_F32X3, 32, 0, 0, {0u}), VmStatus::BAD_REGISTER);
  add("CHANNEL_F32X3 src 30", Prog().op(Op::CHANNEL_F32X3, 0, 30, 0, {0u}), VmStatus::BAD_REGISTER);
  add("STORE_SLOT src 32", Prog().op(Op::STORE_SLOT, 32, 0, 0, {0u}), VmStatus::BAD_REGISTER);
  // Operands the op does not accept.
  add("LOAD_CTX_UV set 2", Prog().op(Op::LOAD_CTX_UV, 0, 2), VmStatus::BAD_OPERAND);
  add("CHANNEL_F32X3 channel 3", Prog().op(Op::CHANNEL_F32X3, 0, 3, 0, {3u}), VmStatus::BAD_OPERAND);
  add("TEX_IMAGE mode 4", Prog().op(Op::TEX_IMAGE, 0, 3, 0, {0u, 4u}), VmStatus::BAD_OPERAND);
  add("TEX_ENVIRONMENT mode EXTEND", Prog().op(Op::TEX_ENVIRONMENT, 0, 3, 0, {0u, 2u}), VmStatus::BAD_OPERAND);  // always repeats
  add("TEX_ENVIRONMENT mode 4", Prog().op(Op::TEX_ENVIRONMENT, 0, 3, 0, {0u, 4u}), VmStatus::BAD_OPERAND);
  add("RGB_LIFT semantic IOR", Prog().op(Op::RGB_LIFT, 0, 4, 3), VmStatus::BAD_OPERAND);
  add("RGB_LIFT semantic 255", Prog().op(Op::RGB_LIFT, 0, 4, 255), VmStatus::BAD_OPERAND);
  add("STORE_SLOT slot 37", Prog().op(Op::STORE_SLOT, 0, 0, 0, {37u}), VmStatus::BAD_OPERAND);
  add("STORE_SLOT slot 255", Prog().op(Op::STORE_SLOT, 0, 0, 0, {255u}), VmStatus::BAD_OPERAND);
  add("STORE_SLOT stray bit 9", Prog().op(Op::STORE_SLOT, 0, 0, 0, {512u}), VmStatus::BAD_OPERAND);
  add("SPECTRUM_TABLE in a program", Prog().op(Op::SPECTRUM_TABLE, 0), VmStatus::BAD_OPERAND);  // the op only folds to a ref
  add("STORE_SLOT spectrum into a scalar slot", Prog().op(Op::STORE_SLOT, 0, 0, 0, {ar::kStoreSlotSpecSource}),
      VmStatus::BAD_OPERAND);
  add("STORE_SLOT spectrum into a frame slot",
      Prog().op(Op::STORE_SLOT, 0, 0, 0, {uint32_t(ar::RecordSlot::geometry_normal) | ar::kStoreSlotSpecSource}),
      VmStatus::BAD_OPERAND);
  // Control flow.
  add("JMP 0", Prog().op(Op::JMP, 0, 0, 0, {0u}), VmStatus::BAD_JUMP);
  add("JMP -1", Prog().op(Op::JMP, 0, 0, 0, {0xffffffffu}), VmStatus::BAD_JUMP);
  add("JMP INT_MIN", Prog().op(Op::JMP, 0, 0, 0, {0x80000000u}), VmStatus::BAD_JUMP);
  add("BRANCH_MIX 0", Prog().op(Op::BRANCH_MIX, 0, 0, 0, {0u}), VmStatus::BAD_JUMP);
  add("BRANCH_MIX -2", Prog().op(Op::BRANCH_MIX, 0, 0, 0, {0xfffffffeu}), VmStatus::BAD_JUMP);
  add("BRANCH_MIX factor 32", Prog().op(Op::BRANCH_MIX, 32, 0, 0, {2u}), VmStatus::BAD_REGISTER);
  add("ADD_F32 dst 32", Prog().op(Op::ADD_F32, 32, 0, 0), VmStatus::BAD_REGISTER);
  add("SUB_F32 src2 32", Prog().op(Op::SUB_F32, 0, 0, 32), VmStatus::BAD_REGISTER);
  add("DIV_F32 src2 32", Prog().op(Op::DIV_F32, 0, 0, 32), VmStatus::BAD_REGISTER);
  add("CLAMP_F32 dst 32", Prog().op(Op::CLAMP_F32, 32, 0, 0, {fb(0.0f), fb(1.0f)}), VmStatus::BAD_REGISTER);
  add("CLAMP_F32 src 32", Prog().op(Op::CLAMP_F32, 0, 32, 0, {fb(0.0f), fb(1.0f)}), VmStatus::BAD_REGISTER);
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
    // A BRANCH_MIX in an entry run without a selection uniform.
    Prog b;
    b.op(Op::BRANCH_MIX, 0, 0, 0, {2u}).end();
    CHECK(ar::run_entry(b.w.data(), 4, &g.regs, g.ctx, g.tables, &g.rec, &g.le) == VmStatus::NULL_POINTER);
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

TEST_CASE("vm/non finite store is refused") {
  // A NaN or Inf in any source register of a scalar, colour, IOR, spectrum or frame store stops the
  // run with NON_FINITE_INPUT and leaves the record untouched (ruling 91). The bad value takes each
  // register of the form in turn, so the check covers the whole width.
  const float bad[2] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
  struct Form {
    const char* name;
    uint32_t imm;
    uint32_t width;
  };
  const Form forms[] = {
      {"scalar", uint32_t(ar::RecordSlot::specular_roughness), 1},
      {"colour", uint32_t(ar::RecordSlot::base_color), 3},
      {"ior", uint32_t(ar::RecordSlot::specular_ior), 1},
      {"spectrum", uint32_t(ar::RecordSlot::base_color) | ar::kStoreSlotSpecSource, 4},
      {"frame", uint32_t(ar::RecordSlot::geometry_normal), 3},
  };
  for (const Form& f : forms)
    for (float v : bad)
      for (uint32_t at = 0; at < f.width; ++at) {  // the bad value in every register of the form
      Prog p;
      for (uint32_t k = 0; k < f.width; ++k) p.op(Op::CONST_F32, 4 + k, 0, 0, {fb(k == at ? v : 0.5f)});
      p.op(Op::STORE_SLOT, 4, 0, 0, {f.imm}).end();
      Rig g;
      g.ctx.lambda_t = ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}};
      Rig h = g;
      INFO(f.name << " " << v << " at " << at);
      CHECK(h.run(p) == VmStatus::NON_FINITE_INPUT);
      CHECK(std::memcmp(&h.rec, &g.rec, sizeof g.rec) == 0);
      // The same program with a finite value stores.
      Prog q;
      for (uint32_t k = 0; k < f.width; ++k) q.op(Op::CONST_F32, 4 + k, 0, 0, {fb(0.5f)});
      q.op(Op::STORE_SLOT, 4, 0, 0, {f.imm}).end();
      Rig ok = g;
      CHECK(ok.run(q) == VmStatus::OK);
    }
}

TEST_CASE("vm/tex environment direction convention") {
  // Cycles' equirectangular mapping, written out as numbers: +x is the middle of the image, +y is a
  // quarter in, -y three quarters, +z the top row (v = 1) and -z the bottom row, whatever the vector's length.
  struct Axis {
    float d[3];
    float u, v;  // u < 0: any
  };
  const Axis axes[] = {{{1, 0, 0}, 0.5f, 0.5f},  {{0, 1, 0}, 0.25f, 0.5f}, {{0, -1, 0}, 0.75f, 0.5f},
                       {{0, 0, 1}, -1.0f, 1.0f}, {{0, 0, -1}, -1.0f, 0.0f}, {{3, 0, 0}, 0.5f, 0.5f},
                       {{1, 1, 0}, 0.375f, 0.5f}, {{1, 0, 1}, 0.5f, 0.75f}};
  for (const Axis& a : axes) {
    float uv[2];
    ar::env_direction_uv(a.d, uv);
    INFO(a.d[0] << " " << a.d[1] << " " << a.d[2]);
    if (a.u >= 0.0f) CHECK(std::fabs(uv[0] - a.u) <= 1e-6f);
    CHECK(std::fabs(uv[1] - a.v) <= 1e-6f);
  }
  // The zero vector maps to (0, 0), as Cycles does, and no direction near a pole gives NaN (a unit
  // vector's z can round above 1, where acos would be NaN).
  const float zero[3] = {0, 0, 0};
  float uv[2] = {7, 7};
  ar::env_direction_uv(zero, uv);
  CHECK((uv[0] == 0.0f && uv[1] == 0.0f));
  uint32_t bad = 0;
  for (uint32_t i = 0; i < 20000; ++i) {
    const float sgn = (i & 1u) ? -1.0f : 1.0f;
    const float d[3] = {(unit(i, 0) - 0.5f) * 2e-3f, (unit(i, 1) - 0.5f) * 2e-3f, sgn * (0.5f + unit(i, 2))};
    ar::env_direction_uv(d, uv);
    bad += !(uv[0] >= 0.0f && uv[0] <= 1.0f && uv[1] >= 0.0f && uv[1] <= 1.0f);
  }
  CHECK(bad == 0u);
}

TEST_CASE("vm/opcode table is wired") {
  // Handler macros exist for every ops.def row (the build proves it); the table and the test's
  // encoding agree on the opcode numbers the programs above use.
  CHECK(uint32_t(Op::END) == 0u);
  CHECK(uint32_t(Op::JMP) == 1u);
  CHECK(uint32_t(Op::CONST_F32) == 2u);
  CHECK(uint32_t(Op::EMIT_LE) == 13u);
  CHECK(uint32_t(Op::TEST_INJECT_NAN) == 15u);
  CHECK(uint32_t(Op::BLACKBODY) == 16u);
  CHECK(uint32_t(Op::CAUCHY_ABBE) == 17u);
  CHECK(uint32_t(Op::CHANNEL_F32X3) == 18u);
  CHECK(uint32_t(Op::VOLUME_COEFFS) == 19u);
  CHECK(uint32_t(Op::BRANCH_MIX) == 20u);
  CHECK(uint32_t(Op::ADD_F32) == 21u);
  CHECK(uint32_t(Op::SUB_F32) == 22u);
  CHECK(uint32_t(Op::SPECTRUM_TABLE) == 23u);
  CHECK(uint32_t(Op::COLOR_TO_F32) == 24u);
  CHECK(uint32_t(Op::TEX_ENVIRONMENT) == 25u);
  CHECK(uint32_t(Op::DIV_F32) == 26u);
  CHECK(uint32_t(Op::CLAMP_F32) == 27u);
  CHECK(uint32_t(Op::COUNT_) == 28u);
}

}  // TEST_SUITE

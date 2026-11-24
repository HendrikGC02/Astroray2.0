// SPDX-License-Identifier: Apache-2.0
// M-1 probe (0005 Risks M-1, ROADMAP Measurements): a gate-only kernel that is never launched or
// embedded. It holds what the full-scope fused shade_surface will hold, so `check full` can read its
// registers, stack and spills in the capped build (0002 D10, ruling 17): the real 304 B
// OpenPBRParams and the one 32-register VmRegs (0005 items 2-3), an interpreter whose switch is
// every ops.def row plus placeholder cases up to the ~100-opcode target, NEE whose deferred light
// runs an emission entry in the same register file, and spike A's fused lobe set (fuzz, coat,
// metal with complex Fresnel, dielectric reflection and transmission with thin film, diffuse)
// standing in for the OpenPBR lobes. The stage body follows stages/shade_surface.h (one
// interpreter site in a two-phase loop, NEE before the BSDF sample) without its test-hook modes.
// Ref: docs/decisions/0005-material-model.md items 2, 3, 30-32, Risks M-1; 0002 D10, Risks 1;
// git show spikes-final:spikes/A-kernel-budget/include/lobes.h (the lobes below, adapted to the
// project's types and record) and include/stages.h (fused NEE then BSDF); stages/shade_surface.h.
#include "launch_config.h"
#include "stages/shade_surface.h"

AR_NS_BEGIN
namespace probe_m1 {

// ---- interpreter: the ops.def switch plus placeholders ------------------------------------------
// Opcodes ops.def does not define yet, numbered up to the ~100 of 0005 Risks M-1. Each is a
// distinct body of one of the shapes the table's rows have (f32, f32x3 and spectrum arithmetic,
// a transcendental, an immediate operand); they stand for future ops' size, not for any node.
// The placeholder cases below are opcodes 22-99, so they start where ops.def ends.
static_assert(uint32_t(Op::COUNT_) == 22, "ops.def changed: renumber the placeholder cases");

template <uint32_t K>
AR_FN void placeholder_op(VmState& vm) {
  float* r = vm.r;
  const float k = 1.0f + float(K) / 128.0f;
  switch (K % 5u) {
    case 0:
      if (!vm_regs_ok(vm, vm.a, 1) || !vm_regs_ok(vm, vm.b, 1) || !vm_regs_ok(vm, vm.c, 1)) return;
      r[vm.a] = ar::fma(r[vm.b], k, r[vm.c]);
      break;
    case 1:
      if (!vm_regs_ok(vm, vm.a, 3) || !vm_regs_ok(vm, vm.b, 3) || !vm_regs_ok(vm, vm.c, 3)) return;
      for (uint32_t i = 0; i < 3u; ++i) r[vm.a + i] = r[vm.b + i] * k + r[vm.c + i];
      break;
    case 2:
      if (!vm_regs_ok(vm, vm.a, 1) || !vm_regs_ok(vm, vm.b, 1) || !vm_regs_ok(vm, vm.c, 1)) return;
      r[vm.a] = ar::exp(-r[vm.b] * k) * ar::log(1.0f + ar::abs(r[vm.c]));
      break;
    case 3:
      if (!vm_regs_ok(vm, vm.a, 4) || !vm_regs_ok(vm, vm.b, 4) || !vm_regs_ok(vm, vm.c, 1)) return;
      for (uint32_t i = 0; i < 4u; ++i) r[vm.a + i] = ar::pow(ar::abs(r[vm.b + i]), k) * r[vm.c];
      break;
    default:
      if (!vm_regs_ok(vm, vm.a, 1) || !vm_regs_ok(vm, vm.b, 1) || !vm_regs_ok(vm, vm.c, 1)) return;
      r[vm.a] = r[vm.b] * bit_cast<float>(vm_imm(vm, 0)) + r[vm.c] * k;
      vm.pc += 1;
      break;
  }
  vm.pc += 1;
}

#define AR_PROBE_CASE(n) case static_cast<Op>(n): placeholder_op<n>(vm); break;
#define AR_PROBE_CASE10(d) AR_PROBE_CASE(d##0) AR_PROBE_CASE(d##1) AR_PROBE_CASE(d##2) AR_PROBE_CASE(d##3) \
  AR_PROBE_CASE(d##4) AR_PROBE_CASE(d##5) AR_PROBE_CASE(d##6) AR_PROBE_CASE(d##7) AR_PROBE_CASE(d##8) AR_PROBE_CASE(d##9)

// vm/interp.h run_entry's loop with the placeholder cases added (same state, same handlers).
AR_FN VmStatus run_entry_100(const uint32_t* prog, uint32_t offset, VmRegs* regs, const ShadeContext& ctx,
                             const SceneTables& tables, OpenPBRParams* rec, Spec* le, float* select_u) {
  VmState vm;
  vm.prog = prog;
  vm.pc = offset;
  vm.r = regs->r;
  vm.ctx = &ctx;
  vm.tables = &tables;
  vm.rec = rec;
  vm.le = le;
  vm.select_u = select_u;
  vm.a = vm.b = vm.c = 0;
  vm.status = VmStatus::OK;
  vm.done = (offset == 0u);
  while (!vm.done && vm.status == VmStatus::OK) {
    const uint32_t word = prog[vm.pc];
    vm.a = (word >> 8) & 0xffu;
    vm.b = (word >> 16) & 0xffu;
    vm.c = word >> 24;
    switch (static_cast<Op>(word & 0xffu)) {
#define AR_OP(NAME, ...) AR_OP_CASE(NAME, handle_##NAME)
#include "vm/ops.def"
#undef AR_OP
      AR_PROBE_CASE(22) AR_PROBE_CASE(23) AR_PROBE_CASE(24) AR_PROBE_CASE(25)
      AR_PROBE_CASE(26) AR_PROBE_CASE(27) AR_PROBE_CASE(28) AR_PROBE_CASE(29)
      AR_PROBE_CASE10(3) AR_PROBE_CASE10(4) AR_PROBE_CASE10(5)
      AR_PROBE_CASE10(6) AR_PROBE_CASE10(7) AR_PROBE_CASE10(8) AR_PROBE_CASE10(9)
      default: vm_fail(vm, VmStatus::BAD_OPCODE); break;
    }
  }
  return vm.status;
}

// ---- spike A's lobes (include/lobes.h), on OpenPBRParams ------------------------------------------
// Physical exactness is not their purpose (spike A's own words): operation count and live state are.
// sin and cos of an angle that is pi times a value use sinpi/cospi, as the production kernels must
// (no double-precision path, portability.h).
inline constexpr Real kPi = Real(kPiDouble);
inline constexpr Real kInvPi = Real(1.0 / kPiDouble);
AR_FN Real sqr(Real x) { return x * x; }
AR_FN Real safe_sqrt(Real x) { return ar::sqrt(ar::max(Real(0), x)); }
AR_FN Real clampr(Real x, Real a, Real b) { return ar::min(ar::max(x, a), b); }
AR_FN Real cos2t(Real3 w) { return w.z * w.z; }
AR_FN Real sin2t(Real3 w) { return ar::max(Real(0), Real(1) - w.z * w.z); }
AR_FN bool same_hemi(Real3 a, Real3 b) { return a.z * b.z > Real(0); }
AR_FN Real3 reflect_about(Real3 wo, Real3 n) { return -wo + n * (Real(2) * dot(wo, n)); }

AR_FN Real2 disk_concentric(Real u0, Real u1) {
  const Real ox = Real(2) * u0 - Real(1), oy = Real(2) * u1 - Real(1);
  if (ox == Real(0) && oy == Real(0)) return Real2{Real(0), Real(0)};
  Real r, th_over_pi;
  if (ar::abs(ox) > ar::abs(oy)) r = ox, th_over_pi = Real(0.25) * (oy / ox);
  else r = oy, th_over_pi = Real(0.5) - Real(0.25) * (ox / oy);
  return Real2{r * cospi(th_over_pi), r * sinpi(th_over_pi)};
}
AR_FN Real3 cosine_hemi(Real u0, Real u1) {
  const Real2 d = disk_concentric(u0, u1);
  return Real3{d.x, d.y, safe_sqrt(Real(1) - d.x * d.x - d.y * d.y)};
}

struct Ggx { Real ax, ay; };
AR_FN Real ggx_D(const Ggx& g, Real3 wm) {
  const Real c2 = cos2t(wm), s2 = sin2t(wm), t2 = s2 / c2;
  if (!(t2 < Real(1e30))) return Real(0);
  const Real cos4 = c2 * c2;
  if (cos4 < Real(1e-16)) return Real(0);
  const Real cp2 = s2 > Real(0) ? wm.x * wm.x / s2 : Real(1);
  const Real e = t2 * (cp2 / sqr(g.ax) + (Real(1) - cp2) / sqr(g.ay));
  return Real(1) / (kPi * g.ax * g.ay * cos4 * sqr(Real(1) + e));
}
AR_FN Real ggx_lambda(const Ggx& g, Real3 w) {
  const Real s2 = sin2t(w), t2 = s2 / cos2t(w);
  if (!(t2 < Real(1e30))) return Real(0);
  const Real cp2 = s2 > Real(0) ? w.x * w.x / s2 : Real(1);
  const Real a2 = cp2 * sqr(g.ax) + (Real(1) - cp2) * sqr(g.ay);
  return (ar::sqrt(Real(1) + a2 * t2) - Real(1)) * Real(0.5);
}
AR_FN Real ggx_G(const Ggx& g, Real3 wo, Real3 wi) { return Real(1) / (Real(1) + ggx_lambda(g, wo) + ggx_lambda(g, wi)); }
AR_FN Real ggx_Dw(const Ggx& g, Real3 w, Real3 wm) {
  return ggx_D(g, wm) * ar::abs(dot(w, wm)) / ((Real(1) + ggx_lambda(g, w)) * ar::abs(w.z));
}
AR_FN Real3 ggx_sample_wm(const Ggx& g, Real3 w, Real u0, Real u1) {
  Real3 wh = normalize(Real3{g.ax * w.x, g.ay * w.y, w.z});
  if (wh.z < Real(0)) wh = -wh;
  const Real3 t1 = wh.z < Real(0.99999) ? normalize(cross(Real3{0, 0, 1}, wh)) : Real3{1, 0, 0};
  const Real3 t2 = cross(wh, t1);
  Real2 p = disk_concentric(u0, u1);
  const Real h = ar::sqrt(Real(1) - p.x * p.x), t = (Real(1) + wh.z) / Real(2);
  p.y = (Real(1) - t) * h + t * p.y;
  const Real pz = safe_sqrt(Real(1) - p.x * p.x - p.y * p.y);
  const Real3 nh = t1 * p.x + t2 * p.y + wh * pz;
  return normalize(Real3{g.ax * nh.x, g.ay * nh.y, ar::max(Real(1e-6), nh.z)});
}

AR_FN Real fr_dielectric(Real cosi, Real eta) {
  cosi = clampr(cosi, Real(-1), Real(1));
  if (cosi < Real(0)) eta = Real(1) / eta, cosi = -cosi;
  const Real s2 = (Real(1) - cosi * cosi) / (eta * eta);
  if (s2 >= Real(1)) return Real(1);
  const Real cost = safe_sqrt(Real(1) - s2);
  const Real rp = (eta * cosi - cost) / (eta * cosi + cost), rs = (cosi - eta * cost) / (cosi + eta * cost);
  return (rp * rp + rs * rs) * Real(0.5);
}
struct Cx { Real re, im; };
AR_FN Cx operator+(Cx a, Cx b) { return Cx{a.re + b.re, a.im + b.im}; }
AR_FN Cx operator-(Cx a, Cx b) { return Cx{a.re - b.re, a.im - b.im}; }
AR_FN Cx operator*(Cx a, Cx b) { return Cx{a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }
AR_FN Cx operator/(Cx a, Cx b) {
  const Real s = Real(1) / (b.re * b.re + b.im * b.im);
  return Cx{(a.re * b.re + a.im * b.im) * s, (a.im * b.re - a.re * b.im) * s};
}
AR_FN Real cnorm(Cx a) { return a.re * a.re + a.im * a.im; }
AR_FN Cx csqrt(Cx z) {
  const Real n = ar::sqrt(cnorm(z));
  if (n == Real(0)) return Cx{0, 0};
  const Real t1 = ar::sqrt(Real(0.5) * (n + ar::abs(z.re))), t2 = Real(0.5) * z.im / t1;
  if (z.re >= Real(0)) return Cx{t1, t2};
  return Cx{ar::abs(t2), z.im >= Real(0) ? t1 : -t1};
}
AR_FN Real fr_complex(Real cosi, Cx eta) {
  cosi = clampr(cosi, Real(0), Real(1));
  const Cx s2t = Cx{Real(1) - cosi * cosi, 0} / (eta * eta);
  const Cx cost = csqrt(Cx{1, 0} - s2t), ci{cosi, 0};
  const Cx rp = (eta * ci - cost) / (eta * ci + cost), rs = (ci - eta * cost) / (ci + eta * cost);
  return (cnorm(rp) + cnorm(rs)) * Real(0.5);
}
// Unpolarised Airy reflectance of a film (nf, d nm) on a substrate ns in air at lam nm.
AR_FN Real fr_thinfilm(Real mu, Real lam, Real d, Real nf, Real ns) {
  mu = clampr(mu, Real(1e-4), Real(1));
  const Real s2 = Real(1) - mu * mu, s2f = s2 / (nf * nf), s2s = s2 / (ns * ns);
  if (s2f >= Real(1) || s2s >= Real(1)) return Real(1);
  const Real cf = ar::sqrt(Real(1) - s2f), cs = ar::sqrt(Real(1) - s2s);
  const Real r12s = (mu - nf * cf) / (mu + nf * cf), r12p = (nf * mu - cf) / (nf * mu + cf);
  const Real r23s = (nf * cf - ns * cs) / (nf * cf + ns * cs), r23p = (ns * cf - nf * cs) / (ns * cf + nf * cs);
  const Real cd = cospi(Real(4) * nf * d * cf / lam);  // cos(delta), delta = 4 pi nf d cf / lam
  const Real Rs = (r12s * r12s + r23s * r23s + Real(2) * r12s * r23s * cd) /
                  (Real(1) + r12s * r12s * r23s * r23s + Real(2) * r12s * r23s * cd);
  const Real Rp = (r12p * r12p + r23p * r23p + Real(2) * r12p * r23p * cd) /
                  (Real(1) + r12p * r12p * r23p * r23p + Real(2) * r12p * r23p * cd);
  return clampr((Rs + Rp) * Real(0.5), Real(0), Real(1));
}
// Karis 2014 EnvBRDFApprox (albedo = F0 A + B); exp2(x) written as exp(x ln 2).
AR_FN Real spec_albedo(Real ior, Real rough, Real mu) {
  const Real r0 = Real(1) - rough, r1 = rough * Real(-0.0275) + Real(0.0425);
  const Real r2 = rough * Real(-0.572) + Real(1.04), r3 = rough * Real(0.022) + Real(-0.04);
  const Real a004 = ar::min(r0 * r0, ar::exp(Real(-9.28) * Real(0.69314718055994531) * mu)) * r0 + r1;
  const Real F0 = sqr((ior - Real(1)) / (ior + Real(1)));
  return clampr(F0 * (Real(-1.04) * a004 + r2) + Real(1.04) * a004 + r3, Real(0), Real(1));
}
// Spike A's placeholder fuzz albedo fit (its words: not calibrated).
AR_FN Real fuzz_albedo(Real wgt, Real favg, Real rough, Real mu) {
  return wgt * favg * (Real(0.2) + Real(0.6) * sqr(Real(1) - mu) * (Real(1) - mu)) * (Real(1) - Real(0.4) * rough);
}

struct BsdfCtx {
  Ggx spec;
  Real ac, fuzz_a;
  Spec cond_n, cond_k;
  Real efo, eco, eso, ef_avg, ec_avg, es_avg;
  Real w_coat, w_metal, tw;
  Real p[5];  // cumulative lobe-selection probabilities: fuzz, coat, metal, dielectric, diffuse
};

AR_FN BsdfCtx prep_ctx(const OpenPBRParams& m, Real muo) {
  BsdfCtx c;
  const Real sr = m.specular_roughness, an = m.specular_roughness_anisotropy;
  const Real a = ar::max(sr * sr, Real(1e-3));
  c.spec.ax = ar::max(a * ar::sqrt(Real(2) / (Real(1) + sqr(Real(1) - an))), Real(1e-3));
  c.spec.ay = ar::max((Real(1) - an) * c.spec.ax, Real(1e-3));
  c.ac = ar::max(sqr(m.coat_roughness), Real(1e-3));
  c.fuzz_a = clampr(m.fuzz_roughness, Real(0.07), Real(1));
  c.tw = m.transmission_weight, c.w_coat = m.coat_weight, c.w_metal = m.base_metalness;
  // Gulbrandsen 2014: conductor n, k from reflectivity (base colour) and edge tint (specular colour).
  for (int i = 0; i < kLanes; ++i) {
    const Real r = clampr(m.base_weight * m.base_color.v[i], Real(0), Real(0.995));
    const Real g = clampr(m.specular_color.v[i], Real(0), Real(1)), sq = ar::sqrt(r);
    const Real n = g * (Real(1) - r) / (Real(1) + r) + (Real(1) - g) * (Real(1) + sq) / (Real(1) - sq + Real(1e-6));
    c.cond_n.v[i] = n;
    c.cond_k.v[i] = ar::sqrt(ar::max((r * sqr(n + Real(1)) - sqr(n - Real(1))) / (Real(1) - r), Real(0)));
  }
  const Real favg = avg(m.fuzz_color), eta = m.specular_ior.v[0];
  c.efo = fuzz_albedo(m.fuzz_weight, favg, m.fuzz_roughness, muo);
  c.ef_avg = fuzz_albedo(m.fuzz_weight, favg, m.fuzz_roughness, Real(0.5));
  c.eco = c.w_coat * spec_albedo(m.coat_ior, m.coat_roughness, muo);
  c.ec_avg = c.w_coat * spec_albedo(m.coat_ior, m.coat_roughness, Real(0.5));
  c.eso = spec_albedo(eta, sr, muo);
  c.es_avg = spec_albedo(eta, sr, Real(0.5));
  const Real above = (Real(1) - c.efo) * (Real(1) - c.eco), base = avg(m.base_color) * m.base_weight;
  Real w[5] = {c.efo, c.eco, c.w_metal * above * base,
               (Real(1) - c.w_metal) * above * (c.eso + c.tw * (Real(1) - c.eso)),
               (Real(1) - c.w_metal) * above * (Real(1) - c.tw) * (Real(1) - c.eso) * base};
  Real tot = w[0] + w[1] + w[2] + w[3] + w[4];
  if (!(tot > Real(1e-6))) w[4] = Real(1), tot = Real(1);
  Real acc = Real(0);
  for (int i = 0; i < 4; ++i) c.p[i] = (acc += w[i] / tot);
  c.p[4] = Real(1);
  return c;
}

// Dielectric Fresnel per lane: the thin film's Airy reflectance when a film is present.
AR_FN Spec diel_fresnel(const OpenPBRParams& m, const Lambda& lam, Real mu) {
  Spec F;
  for (int i = 0; i < kLanes; ++i)
    F.v[i] = m.thin_film_thickness > Real(0)
                 ? fr_thinfilm(ar::abs(mu), lam.nm[i], m.thin_film_thickness * Real(1000), m.thin_film_ior, m.specular_ior.v[i])
                 : fr_dielectric(mu, m.specular_ior.v[i]);
  return F;
}

// bsdf_eval of lobes.h: the five lobes and the albedo-scaling layer weights, local frame.
AR_FN LobeEval bsdf_eval(const OpenPBRParams& m, const BsdfCtx& c, const Lambda& lam, Real3 wo, Real3 wi) {
  LobeEval out;
  out.f = spec_fill(Real(0));
  out.pdf = Real(0);
  Spec ff = spec_fill(Real(0)), fm = ff, fd = ff, fl = ff;
  Real pf = 0, pc = 0, pm = 0, pd = 0, pl = 0, fc = 0;
  const bool up = same_hemi(wo, wi) && wo.z > Real(0);
  if (up && m.fuzz_weight > Real(0)) {  // fuzz: Charlie sheen D, Ashikhmin V
    const Real3 wm = normalize(wo + wi);
    const Real inva = Real(1) / c.fuzz_a, sinh = safe_sqrt(Real(1) - wm.z * wm.z);
    const Real D = (Real(2) + inva) * ar::pow(ar::max(sinh, Real(1e-6)), inva) / (Real(2) * kPi);
    ff = m.fuzz_color * (m.fuzz_weight * D / (Real(4) * (wo.z + wi.z - wo.z * wi.z)));
    pf = wi.z * kInvPi;
  }
  if (up && (c.w_coat > Real(0) || c.w_metal > Real(0))) {  // coat and metal reflection
    const Real3 wm = normalize(wo + wi);
    const Ggx gc{c.ac, c.ac};
    if (c.w_coat > Real(0)) {
      fc = c.w_coat * ggx_D(gc, wm) * ggx_G(gc, wo, wi) * fr_dielectric(dot(wo, wm), m.coat_ior) / (Real(4) * wi.z * wo.z);
      pc = ggx_Dw(gc, wo, wm) / (Real(4) * ar::abs(dot(wo, wm)));
    }
    if (c.w_metal > Real(0)) {
      const Real d = dot(wo, wm), b = ggx_D(c.spec, wm) * ggx_G(c.spec, wo, wi) / (Real(4) * wi.z * wo.z);
      for (int i = 0; i < kLanes; ++i) fm.v[i] = c.w_metal * b * fr_complex(d, Cx{c.cond_n.v[i], c.cond_k.v[i]});
      pm = ggx_Dw(c.spec, wo, wm) / (Real(4) * ar::abs(d));
    }
  }
  if (wi.z != Real(0) && wo.z != Real(0)) {  // dielectric: reflection and rough transmission
    const Real eta = m.specular_ior.v[0], ci = wi.z, co = wo.z;
    const bool refl = ci * co > Real(0);
    const Real etap = refl ? Real(1) : (co > Real(0) ? eta : Real(1) / eta);
    Real3 wm = wi * etap + wo;
    if (dot(wm, wm) > Real(0)) {
      wm = face_forward(normalize(wm), Real3{0, 0, 1});
      if (!(dot(wm, wi) * ci < Real(0) || dot(wm, wo) * co < Real(0))) {
        const Spec F = diel_fresnel(m, lam, dot(wo, wm));
        const Real pr = avg(F), pt = (Real(1) - pr) * c.tw;
        if (pr + pt > Real(0)) {
          const Real D = ggx_D(c.spec, wm), G = ggx_G(c.spec, wo, wi);
          if (refl) {
            fd = F * (D * G / ar::abs(Real(4) * ci * co));
            pd = ggx_Dw(c.spec, wo, wm) / (Real(4) * ar::abs(dot(wo, wm))) * pr / (pr + pt);
          } else {
            const Real den = sqr(dot(wi, wm) + dot(wo, wm) / etap);
            const Real k = D * G * ar::abs(dot(wi, wm) * dot(wo, wm) / (den * ci * co)) / sqr(etap) * c.tw;
            for (int i = 0; i < kLanes; ++i) fd.v[i] = (Real(1) - F.v[i]) * k * m.transmission_color.v[i];
            pd = ggx_Dw(c.spec, wo, wm) * ar::abs(dot(wi, wm)) / den * pt / (pr + pt);
          }
          fd = fd * (Real(1) - c.w_metal);
        }
      }
    }
  }
  if (up) {  // Oren-Nayar diffuse
    const Real s2 = sqr(m.base_diffuse_roughness);
    const Real A = Real(1) - s2 / (Real(2) * (s2 + Real(0.33))), B = Real(0.45) * s2 / (s2 + Real(0.09));
    const Real sti = safe_sqrt(sin2t(wi)), sto = safe_sqrt(sin2t(wo));
    const Real maxc = (sti > Real(1e-4) && sto > Real(1e-4)) ? ar::max(Real(0), (wi.x * wo.x + wi.y * wo.y) / (sti * sto)) : Real(0);
    const bool wi_steeper = ar::abs(wi.z) > ar::abs(wo.z);
    const Real sa = wi_steeper ? sto : sti, tb = wi_steeper ? sti / ar::abs(wi.z) : sto / ar::abs(wo.z);
    fl = m.base_color * ((A + B * maxc * sa * tb) * kInvPi * (Real(1) - c.w_metal) * (Real(1) - c.tw) * m.base_weight);
    pl = ar::abs(wi.z) * kInvPi;
  }
  // layer_scales: fuzz over coat over base, diffuse also under the specular layer.
  const Real mui = ar::abs(wi.z), muo = ar::abs(wo.z);
  const Real efi = fuzz_albedo(m.fuzz_weight, avg(m.fuzz_color), m.fuzz_roughness, mui);
  const Real eci = c.w_coat * spec_albedo(m.coat_ior, m.coat_roughness, mui);
  const Real sF = (Real(1) - c.efo) * (Real(1) - efi) / ar::max(Real(1) - c.ef_avg, Real(1e-3));
  const Real sC = (Real(1) - c.eco) * (Real(1) - eci) / ar::max(Real(1) - c.ec_avg, Real(1e-3));
  const Real len = Real(1) / ar::max(muo, Real(1e-3)) + Real(1) / ar::max(mui, Real(1e-3));
  Spec sB;
  for (int i = 0; i < kLanes; ++i) sB.v[i] = ar::pow(m.coat_color.v[i], c.w_coat * len * Real(0.5)) * sF * sC;
  const Real esi = spec_albedo(m.specular_ior.v[0], m.specular_roughness, mui);
  const Real sS = (Real(1) - c.eso) * (Real(1) - esi) / ar::max(Real(1) - c.es_avg, Real(1e-3));
  out.f = ff + spec_fill(fc * sF) + (fm + fd) * sB + fl * (sB * sS);
  out.pdf = c.p[0] * pf + (c.p[1] - c.p[0]) * pc + (c.p[2] - c.p[1]) * pm + (c.p[3] - c.p[2]) * pd + (c.p[4] - c.p[3]) * pl;
  return out;
}

// bsdf_sample_dir of lobes.h. Its third uniform (reflection or transmission) is the lobe choice
// remapped into [0, 1) inside the chosen lobe's interval (pbrt-v4 SampleDiscrete's uRemapped).
AR_FN bool bsdf_sample_dir(const OpenPBRParams& m, const BsdfCtx& c, const Lambda& lam, Real3 wo, Real ul, Real u0,
                           Real u1, Real3* wi, bool* glossy) {
  // Selected as lobes.h does (a compare chain, no dynamically indexed array).
  const int lobe = ul < c.p[0] ? 0 : ul < c.p[1] ? 1 : ul < c.p[2] ? 2 : ul < c.p[3] ? 3 : 4;
  const Real lo = lobe == 0 ? Real(0) : lobe == 1 ? c.p[0] : lobe == 2 ? c.p[1] : lobe == 3 ? c.p[2] : c.p[3];
  const Real hi = lobe == 0 ? c.p[0] : lobe == 1 ? c.p[1] : lobe == 2 ? c.p[2] : lobe == 3 ? c.p[3] : Real(1);
  const Real u2 = ar::min((ul - lo) / ar::max(hi - lo, Real(1e-6)), Real(0.99999994));
  *glossy = lobe == 1 || lobe == 2 || lobe == 3;
  if (lobe == 0 || lobe == 4) {
    *wi = cosine_hemi(u0, u1);
    if (wo.z < Real(0)) wi->z = -wi->z;
    return wi->z != Real(0);
  }
  const Real3 wm = ggx_sample_wm(lobe == 1 ? Ggx{c.ac, c.ac} : c.spec, wo, u0, u1);
  const Real pr = lobe == 3 ? avg(diel_fresnel(m, lam, dot(wo, wm))) : Real(1);
  const Real pt = (Real(1) - pr) * c.tw;
  if (lobe == 3 && !(pr + pt > Real(0))) return false;
  if (lobe != 3 || u2 < pr / (pr + pt)) {
    *wi = reflect_about(wo, wm);
    return same_hemi(wo, *wi);
  }
  Real cosi = dot(wo, wm), e = m.specular_ior.v[0];
  Real3 n = wm;
  if (cosi < Real(0)) e = Real(1) / e, cosi = -cosi, n = -n;
  const Real s2 = ar::max(Real(0), Real(1) - cosi * cosi) / (e * e);
  if (s2 >= Real(1)) return false;
  *wi = -wo * (Real(1) / e) + n * (cosi / e - safe_sqrt(Real(1) - s2));
  return !same_hemi(wo, *wi) && wi->z != Real(0);
}

// stages/shade_surface.h's stage with run_entry_100 and spike A's lobes, without the NEE test modes.
AR_FN void stage(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  const uint32_t sample = ps_sample_id(kg, slot);
  const SceneData& s = scene_of(kg);
  const ObserverView& obs = kg.tables->observer;
  uint32_t flags = ps_flags(kg, slot);
  const bool arrived_specular = (flags & LAST_BOUNCE_SPECULAR) != 0u;
  const uint32_t inst = ps_hit_inst(kg, slot), prim = ps_hit_prim(kg, slot);
  bool ok = s.material_headers != nullptr && s.material_constants != nullptr && s.material_pass_index != nullptr;
  ShadeContext ctx;
  ctx.ray_type = ps_ray_type(kg, slot);
  {
    const U16x6 b = ps_bounces(kg, slot);
    for (int i = 0; i < 6; ++i) ctx.depth[i] = b.v[i];
    const Spec lt = ps_lambda_t(kg, slot);
    for (int i = 0; i < kLanes; ++i) ctx.lambda_t.nm[i] = lt.v[i];
  }
  const uint32_t total = ctx.depth[0];
  OpenPBRParams rec;
  VmRegs regs;  // the kernel's one register file: surface entry, then the light's emission entry
  Spec le = spec_fill(Real(0));
  Real3 P{}, Ng{}, N{}, wo{};
  uint32_t nee_state = 0, sh = 0, g_inst = inst, g_prim = prim, mat = 0;
  Real2 g_bary = ps_hit_uv(kg, slot);
  Real3 g_dir = ps_ray_d(kg, slot);
  // The lobe-choice uniform through the entry's selection uniform, as stages/shade_surface.h.
  float uc = float(camera_path_draw(kg, sample, event_dim(ps_event_index(kg, slot), EV_LOBE_CHOICE)));
  for (uint32_t phase = 0;; ++phase) {
    if (!shade_context_geometry(kg, g_inst, g_prim, g_bary, g_dir, &ctx)) {
      sample_mark_invalid(kg, sample);
      path_end(kg, slot, Term::INVALID);
      return;
    }
    mat = hit_material_id(kg, g_inst, g_prim);
    if (ok) {
      const MaterialHeader* hp = s.material_headers + mat;
      const uint32_t mflags = load_ro(&hp->flags);
      const ConstantBlock& block = s.material_constants[load_ro(&hp->const_offset)];
      if (phase == 0u) resolve_constant_record(block, ctx, obs, &rec);
      le = resolve_slot<SpectrumSemantic::ILLUMINANT>(block.emission_color, ctx.lambda_t, obs) * Real(block.emission_luminance);
      const uint32_t* prog = s.material_programs + load_ro(&hp->program_offset);
      const bool program = phase == 0u ? (mflags & CONSTANT_SURFACE) == 0u : (mflags & CONSTANT_EMISSION) == 0u;
      const uint32_t entry = program ? load_ro(prog + (phase == 0u ? 0u : 2u)) : 0u;
      ok = run_entry_100(prog, entry, &regs, ctx, *kg.tables, phase == 0u ? &rec : nullptr, &le,
                         phase == 0u ? &uc : nullptr) == VmStatus::OK;
    }
    if (phase != 0u) break;
    P = ctx.P, Ng = ctx.Ng, N = ctx.N, wo = ctx.wo;
    flags = ctx.front_face != 0u ? (flags | FRONT_FACE) : (flags & ~FRONT_FACE);
    if (s.material_pass_index != nullptr && s.material_headers != nullptr && total == 0u) {
      const uint32_t index = kg.session.samples_done + sample % kg.session.samples_per_pass;
      film_primary_aov(kg, sample, index, &flags, P, N, ctx.uv[0].x, ctx.uv[0].y, load_ro(s.material_pass_index + mat));
    }
    Spec contribution = spec_fill(Real(0));
    const bool weighted = total > 0u && !arrived_specular;
    if (ok && max_lane(le) > Real(0)) {
      Real p_light = Real(0);
      if (weighted) {
        const uint32_t light = light_of_hit(kg, inst, prim);
        ok = light != kLightIndexInvalid;
        if (ok && light != kNoLight) {
          const LightSampleContext prev = prev_light_context(kg, slot);
          p_light = light_sampler_pmf(*kg.light_sampler, prev, light) * tri_pdf_li(kg.lights[light], prev, -wo, LightHit{P, Ng});
        }
      }
      contribution = le * ps_beta(kg, slot) *
                     (radiance_scale(kg) * mis_emitter_scale(ps_r_u(kg, slot), weighted ? ps_r_l(kg, slot) : Spec{}, p_light, weighted));
    }
    if (!ok || !is_finite(contribution)) {
      sample_mark_invalid(kg, sample);
      ps_flags(kg, slot) = flags;
      path_end(kg, slot, Term::INVALID);
      return;
    }
    sample_add(kg, sample, contribution);
    ps_flags(kg, slot) = flags;
    if (total >= kg.session.max_bounces[0]) {
      path_end(kg, slot, Term::MAX_BOUNCE);
      return;
    }
    // NEE (stages.h nee_part: NEE reads beta before the BSDF update).
    const uint32_t e = ps_event_index(kg, slot);
    const LightSampleContext lctx = light_context_point(offset_ray_origin(P, Ng, wo), Ng, N);
    const uint32_t bits = camera_path_draw_bits(kg, sample, event_dim(e, EV_LIGHT_PICK));
    const uint32_t q_bits = camera_path_draw_bits(kg, sample, DIM_CAM_ALIAS_Q) ^ uint32_t(mix_bits(uint64_t(e)));
    const SampledLight sl = light_sampler_sample(*kg.light_sampler, lctx, bits, bits_to_unit(q_bits));
    if (!(sl.pmf > Real(0))) break;
    const LightRecord& lr = kg.lights[sl.light];
    const Real2 ul{camera_path_draw(kg, sample, event_dim(e, EV_LIGHT_U)), camera_path_draw(kg, sample, event_dim(e, EV_LIGHT_V))};
    const LightLiSample ls = light_sample_li(lr, lctx, ul, ctx.lambda_t, obs);
    if (!(ls.pdf > Real(0))) break;
    const Frame fr = frame_from_normal_tangent(N, rec.geometry_tangent);
    const Real3 wo_l = to_local(fr, wo);
    const LobeEval ev = bsdf_eval(rec, prep_ctx(rec, ar::abs(wo_l.z)), ctx.lambda_t, wo_l, to_local(fr, ls.wi));
    Spec nee = ps_beta(kg, slot) * ev.f * (abs_dot(ls.wi, N) * mis_nee_scale(ps_r_u(kg, slot), sl.pmf * ls.pdf, ev.pdf, ls.delta));
    if (!ls.deferred_le) nee *= ls.Le;
    if (!is_finite(nee)) {
      sample_mark_invalid(kg, sample);
      path_end(kg, slot, Term::INVALID);
      return;
    }
    if (!(max_lane(nee) > Real(0))) break;
    const bool triangle = LightType(lr.type) == LightType::TRIANGLE;
    ShadowSegment seg;
    if (ls.infinite) seg = ShadowSegment{offset_ray_origin(P, Ng, ls.wi), ls.wi, Real(kShadowUnbounded)};
    else if (triangle) seg = shadow_segment(P, Ng, ls.p_light, tri_normal(lr));
    else seg = shadow_segment_to_lamp(P, Ng, ls.p_light);
    sh = load_ro(kg.shadow_queue + step.shadow_base + item);
    sh_ray_o(kg, sh) = seg.origin;
    sh_ray_d(kg, sh) = seg.dir;
    sh_tmax(kg, sh) = seg.tmax;
    sh_contribution(kg, sh) = nee;
    sh_target(kg, sh) = sample;
    if (kg.session.features.light_groups) sh_light_group(kg, sh) = load_ro(&lr.light_group);
    nee_state = ls.deferred_le ? 2u : 1u;
    if (!ls.deferred_le) break;
    // Phase 1: the light's emission entry at the sampled point, in the same register file.
    g_inst = tri_instance(lr), g_prim = tri_index(lr);
    g_bary = triangle_barycentrics(tri_vertex(lr, 0), tri_vertex(lr, 1), tri_vertex(lr, 2), ls.p_light);
    g_dir = ls.wi;
  }
  if (nee_state == 2u) {
    const Spec nee = sh_contribution(kg, sh) * le * radiance_scale(kg);
    if (!ok || !is_finite(nee)) {
      sample_mark_invalid(kg, sample);
      path_end(kg, slot, Term::INVALID);
      return;
    }
    sh_contribution(kg, sh) = nee;
    if (!(max_lane(nee) > Real(0))) nee_state = 0u;
  }
  if (nee_state != 0u) sh_next_stage(kg, sh) = static_cast<uint8_t>(Stage::INTERSECT_SHADOW);

  // BSDF sample (stages.h bsdf_part), then shade_surface.h's continuation.
  const uint32_t e = ps_event_index(kg, slot);
  const Spec beta = ps_beta(kg, slot), r_u = ps_r_u(kg, slot);
  const Frame fr = frame_from_normal_tangent(N, rec.geometry_tangent);
  const Real3 wo_l = to_local(fr, wo);
  const BsdfCtx c = prep_ctx(rec, ar::abs(wo_l.z));
  Real3 wi_l{};
  bool glossy = false;
  const bool sampled = bsdf_sample_dir(rec, c, ctx.lambda_t, wo_l, Real(uc),
                                       camera_path_draw(kg, sample, event_dim(e, EV_BSDF_U)),
                                       camera_path_draw(kg, sample, event_dim(e, EV_BSDF_V)), &wi_l, &glossy);
  const LobeEval bs = sampled ? bsdf_eval(rec, c, ctx.lambda_t, wo_l, wi_l) : LobeEval{spec_fill(Real(0)), Real(0)};
  const Real3 wi = to_world(fr, wi_l);
  if (!(bs.pdf > Real(0)) || (wi_l.z * wo_l.z > Real(0) && !(dot(wi, Ng) > Real(0)))) {
    path_end(kg, slot, Term::ZERO_THROUGHPUT);
    return;
  }
  Spec beta_next = beta * (bs.f * (abs_dot(wi, N) / bs.pdf));
  if (!is_finite(beta_next)) {
    sample_mark_invalid(kg, sample);
    path_end(kg, slot, Term::INVALID);
    return;
  }
  if (!(max_lane(beta_next) > Real(0))) {
    path_end(kg, slot, Term::ZERO_THROUGHPUT);
    return;
  }
  if (!roulette(&beta_next, ps_eta_scale(kg, slot), r_u, total + 1u, kg.session.min_bounces,
                camera_path_draw(kg, sample, event_dim(e, EV_ROULETTE)))) {
    path_end(kg, slot, Term::RUSSIAN_ROULETTE);
    return;
  }
  ps_beta(kg, slot) = beta_next;
  ps_r_l(kg, slot) = r_u / bs.pdf;
  ps_flags(kg, slot) = flags & ~LAST_BOUNCE_SPECULAR;
  U16x6 b = ps_bounces(kg, slot);
  b.v[0] = uint16_t(b.v[0] + 1u);
  b.v[glossy ? 2 : 1] = uint16_t(b.v[glossy ? 2 : 1] + 1u);
  ps_bounces(kg, slot) = b;
  ps_event_index(kg, slot) = uint16_t(e + 1u);
  ps_prev_ng(kg, slot) = oct_encode(Ng);
  ps_prev_ns(kg, slot) = oct_encode(N);
  ps_prev_dist(kg, slot) = Real(0);
  ps_ray_o(kg, slot) = offset_ray_origin(P, Ng, wi);
  ps_ray_d(kg, slot) = wi;
  ps_ray_type(kg, slot) = glossy ? kRayTypeGlossy : kRayTypeDiffuse;
  ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::INTERSECT_CLOSEST);
}
}  // namespace probe_m1
AR_NS_END

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_ar_probe_m1.threads, ar::kLaunch_ar_probe_m1.min_blocks)
    ar_probe_m1(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const ar::StepEntry step = g.step_table->stage[static_cast<uint32_t>(ar::Stage::SHADE_SURFACE)];
  for (uint32_t item = blockIdx.x * blockDim.x + threadIdx.x; item < step.count; item += gridDim.x * blockDim.x)
    ar::probe_m1::stage(g, item, step);
}

// Layered lobe stack: fuzz (sheen) / coat (GGX dielectric) / base = {metal GGX,
// dielectric GGX refl+rough transmission with optional thin film, Oren-Nayar diffuse}.
// Layering by albedo scaling (below-layer scale = (1-E(wo))(1-E(wi))/(1-Eavg)).
//
// Sources (formulas taken from, cited as approximations; physical exactness is not the
// goal of this spike, operation count and live state are):
//  - Trowbridge-Reitz / VNDF sampling, dielectric BxDF, FrDielectric, FrComplex:
//    pbrt-v4 (Pharr, Jakob, Humphreys), microfacet.h / bxdfs.h, Apache-2.0-compatible description;
//    VNDF: Heitz 2018 "Sampling the GGX distribution of visible normals" (JCGT 7(4)).
//  - Anisotropic roughness mapping alpha=r^2 and layer ordering: OpenPBR Surface spec 1.1.
//  - Artist-friendly conductor n,k from reflectivity + edge tint: Gulbrandsen 2014 (JCGT 3(4)).
//  - Directional-albedo fit: Karis 2014, "Physically Based Shading on Mobile" (EnvBRDFApprox).
//  - Charlie sheen D, Ashikhmin visibility: Estevez & Kulla 2017 "Production Friendly Microfacet Sheen BRDF".
//  - Oren-Nayar qualitative form: pbrt-v3 sec. 8.2.2.
//  - Thin film Airy reflectance (unpolarised, per wavelength): Born & Wolf; Belcour & Barla 2017.
#pragma once
#include "material.h"

namespace kl {

// ---------------------------------------------------------------- helpers
FN Real cos2theta(V3 w) { return w.z * w.z; }
FN Real sin2theta(V3 w) { return max_((Real)0, (Real)1 - w.z * w.z); }
FN Real tan2theta(V3 w) { return sin2theta(w) / cos2theta(w); }
FN bool same_hemi(V3 a, V3 b) { return a.z * b.z > 0; }
FN V3 reflect_(V3 wo, V3 n) { return -wo + n * ((Real)2 * dot(wo, n)); }

FN void sample_disk_concentric(Real u0, Real u1, Real& dx, Real& dy) {
  Real ox = (Real)2 * u0 - 1, oy = (Real)2 * u1 - 1;
  if (ox == 0 && oy == 0) { dx = 0; dy = 0; return; }
  Real r, th;
  if (abs_(ox) > abs_(oy)) { r = ox; th = (PI / 4) * (oy / ox); }
  else { r = oy; th = PI / 2 - (PI / 4) * (ox / oy); }
  dx = r * cos_(th); dy = r * sin_(th);
}
FN V3 sample_cosine_hemi(Real u0, Real u1) {
  Real dx, dy; sample_disk_concentric(u0, u1, dx, dy);
  return v3(dx, dy, safe_sqrt((Real)1 - dx * dx - dy * dy));
}

// ---------------------------------------------------------------- GGX (pbrt-v4)
struct Ggx { Real ax, ay; };
FN Real ggx_D(const Ggx& g, V3 wm) {
  Real t2 = tan2theta(wm);
  if (!(t2 < (Real)1e30)) return 0;
  Real cos4 = sqr(cos2theta(wm));
  if (cos4 < (Real)1e-16) return 0;
  Real s2 = sin2theta(wm);
  Real cp2 = s2 > 0 ? wm.x * wm.x / s2 : (Real)1, sp2 = (Real)1 - cp2;
  Real e = t2 * (cp2 / sqr(g.ax) + sp2 / sqr(g.ay));
  return (Real)1 / (PI * g.ax * g.ay * cos4 * sqr((Real)1 + e));
}
FN Real ggx_lambda(const Ggx& g, V3 w) {
  Real t2 = tan2theta(w);
  if (!(t2 < (Real)1e30)) return 0;
  Real s2 = sin2theta(w);
  Real cp2 = s2 > 0 ? w.x * w.x / s2 : (Real)1, sp2 = (Real)1 - cp2;
  Real a2 = cp2 * sqr(g.ax) + sp2 * sqr(g.ay);
  return (sqrt_((Real)1 + a2 * t2) - 1) * (Real)0.5;
}
FN Real ggx_G1(const Ggx& g, V3 w) { return (Real)1 / ((Real)1 + ggx_lambda(g, w)); }
FN Real ggx_G(const Ggx& g, V3 wo, V3 wi) { return (Real)1 / ((Real)1 + ggx_lambda(g, wo) + ggx_lambda(g, wi)); }
FN Real ggx_Dw(const Ggx& g, V3 w, V3 wm) {
  return ggx_G1(g, w) / abs_(w.z) * ggx_D(g, wm) * abs_(dot(w, wm));
}
FN V3 ggx_sample_wm(const Ggx& g, V3 w, Real u0, Real u1) {
  V3 wh = normalize(v3(g.ax * w.x, g.ay * w.y, w.z));
  if (wh.z < 0) wh = -wh;
  V3 T1 = wh.z < (Real)0.99999 ? normalize(cross(v3(0, 0, 1), wh)) : v3(1, 0, 0);
  V3 T2 = cross(wh, T1);
  Real px, py; sample_disk_concentric(u0, u1, px, py);
  // polar-disk variant in pbrt; concentric used here (same cost class)
  Real h = sqrt_((Real)1 - px * px);
  py = lerp_(((Real)1 + wh.z) / 2, h, py);
  Real pz = safe_sqrt((Real)1 - px * px - py * py);
  V3 nh = T1 * px + T2 * py + wh * pz;
  return normalize(v3(g.ax * nh.x, g.ay * nh.y, max_((Real)1e-6, nh.z)));
}

// ---------------------------------------------------------------- Fresnel
FN Real fr_dielectric(Real cosi, Real eta) {
  cosi = clamp_(cosi, (Real)-1, (Real)1);
  if (cosi < 0) { eta = (Real)1 / eta; cosi = -cosi; }
  Real sin2t = ((Real)1 - cosi * cosi) / (eta * eta);
  if (sin2t >= 1) return 1;
  Real cost = safe_sqrt((Real)1 - sin2t);
  Real rp = (eta * cosi - cost) / (eta * cosi + cost);
  Real rs = (cosi - eta * cost) / (cosi + eta * cost);
  return (rp * rp + rs * rs) * (Real)0.5;
}
struct Cx { Real re, im; };
FN Cx cx(Real a, Real b) { Cx c; c.re = a; c.im = b; return c; }
FN Cx operator+(Cx a, Cx b) { return cx(a.re + b.re, a.im + b.im); }
FN Cx operator-(Cx a, Cx b) { return cx(a.re - b.re, a.im - b.im); }
FN Cx operator*(Cx a, Cx b) { return cx(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re); }
FN Cx operator/(Cx a, Cx b) {
  Real s = (Real)1 / (b.re * b.re + b.im * b.im);
  return cx((a.re * b.re + a.im * b.im) * s, (a.im * b.re - a.re * b.im) * s);
}
FN Real cnorm(Cx a) { return a.re * a.re + a.im * a.im; }
FN Cx csqrt(Cx z) {
  Real n = sqrt_(cnorm(z));
  if (n == 0) return cx(0, 0);
  Real t1 = sqrt_((Real)0.5 * (n + abs_(z.re)));
  Real t2 = (Real)0.5 * z.im / t1;
  if (z.re >= 0) return cx(t1, t2);
  return cx(abs_(t2), z.im >= 0 ? t1 : -t1);
}
FN Real fr_complex(Real cosi, Cx eta) {
  cosi = clamp_(cosi, (Real)0, (Real)1);
  Real sin2i = (Real)1 - cosi * cosi;
  Cx sin2t = cx(sin2i, 0) / (eta * eta);
  Cx cost = csqrt(cx(1, 0) - sin2t);
  Cx ci = cx(cosi, 0);
  Cx rp = (eta * ci - cost) / (eta * ci + cost);
  Cx rs = (ci - eta * cost) / (ci + eta * cost);
  return (cnorm(rp) + cnorm(rs)) * (Real)0.5;
}
// Unpolarised Airy reflectance of a film (nf, d nm) on a substrate ns in air, wavelength lam nm.
FN Real fr_thinfilm(Real mu, Real lam, Real d, Real nf, Real ns) {
  mu = clamp_(mu, (Real)1e-4, (Real)1);
  Real s2 = (Real)1 - mu * mu;
  Real s2f = s2 / (nf * nf);
  Real s2s = s2 / (ns * ns);
  if (s2f >= 1 || s2s >= 1) return 1;
  Real cf = sqrt_((Real)1 - s2f), cs = sqrt_((Real)1 - s2s);
  Real r12s = (mu - nf * cf) / (mu + nf * cf);
  Real r12p = (nf * mu - cf) / (nf * mu + cf);
  Real r23s = (nf * cf - ns * cs) / (nf * cf + ns * cs);
  Real r23p = (ns * cf - nf * cs) / (ns * cf + nf * cs);
  Real delta = (Real)4 * PI * nf * d * cf / lam;
  Real cd = cos_(delta);
  Real Rs = (r12s * r12s + r23s * r23s + (Real)2 * r12s * r23s * cd) /
            ((Real)1 + r12s * r12s * r23s * r23s + (Real)2 * r12s * r23s * cd);
  Real Rp = (r12p * r12p + r23p * r23p + (Real)2 * r12p * r23p * cd) /
            ((Real)1 + r12p * r12p * r23p * r23p + (Real)2 * r12p * r23p * cd);
  return clamp_((Rs + Rp) * (Real)0.5, (Real)0, (Real)1);
}

// Karis 2014 EnvBRDFApprox: returns (A,B) so that albedo = F0*A + B (F90 = 1).
FN void env_brdf_ab(Real rough, Real nov, Real& A, Real& B) {
  Real r0 = rough * (Real)-1 + (Real)1, r1 = rough * (Real)-0.0275 + (Real)0.0425;
  Real r2 = rough * (Real)-0.572 + (Real)1.04, r3 = rough * (Real)0.022 + (Real)-0.04;
  Real a004 = min_(r0 * r0, exp2_((Real)-9.28 * nov)) * r0 + r1;
  A = (Real)-1.04 * a004 + r2;
  B = (Real)1.04 * a004 + r3;
}

// ---------------------------------------------------------------- per-hit lobe context
template <int N> struct BsdfCtx {
  Ggx spec;       // specular (anisotropic) alphas
  Real ac;        // coat alpha
  Real fuzzA;     // charlie alpha
  Spec<N> condN, condK; // conductor complex IOR per wavelength
  Spec<N> lam;
  Real Efo, Eco, Eso, Ef_avg, Ec_avg, Es_avg; // directional albedos (fuzz, coat, spec) at wo and averaged
  Real wCoat, wMetal;
  Real tw;
  Real p[5];      // cumulative lobe-selection probabilities
};

enum { LOBE_FUZZ = 0, LOBE_COAT, LOBE_METAL, LOBE_DIEL, LOBE_DIFF };

FN Real fuzz_albedo(Real wgt, Real favg, Real rough, Real mu) {
  // placeholder analytic fit (cost class of a LUT-free albedo); not calibrated
  return wgt * favg * ((Real)0.2 + (Real)0.6 * sqr((Real)1 - mu) * ((Real)1 - mu)) * ((Real)1 - (Real)0.4 * rough);
}
FN Real spec_albedo(Real ior, Real rough, Real mu) {
  Real F0 = sqr((ior - 1) / (ior + 1)), A, B;
  env_brdf_ab(rough, mu, A, B);
  return clamp_(F0 * A + B, (Real)0, (Real)1);
}

template <int N>
LOBE_FN void prep_ctx(MAT_ARG(MatRec<N>), const Spec<N>& lam, Real muo, BsdfCtx<N>& c) {
  MAT_USE(MatRec<N>);
  c.lam = lam;
  Real sr = m.s[S_SPEC_ROUGH], an = m.s[S_SPEC_ANISO];
  Real a = max_(sr * sr, (Real)1e-3);
  c.spec.ax = max_(a * sqrt_((Real)2 / ((Real)1 + sqr((Real)1 - an))), (Real)1e-3);
  c.spec.ay = max_(((Real)1 - an) * c.spec.ax, (Real)1e-3);
  c.ac = max_(sqr(m.s[S_COAT_ROUGH]), (Real)1e-3);
  c.fuzzA = clamp_(m.s[S_FUZZ_ROUGH], (Real)0.07, (Real)1);
  c.tw = m.s[S_TRANS_WEIGHT];
  c.wCoat = m.s[S_COAT_WEIGHT];
  c.wMetal = m.s[S_METALNESS];
  // Gulbrandsen conductor n,k from reflectivity (base colour) and edge tint (specular colour)
  for (int i = 0; i < N; ++i) {
    Real r = clamp_(m.s[S_BASE_WEIGHT] * m.sp[SP_BASE].v[i], (Real)0.0, (Real)0.995);
    Real g = clamp_(m.sp[SP_SPEC].v[i], (Real)0, (Real)1);
    Real sr_ = sqrt_(r);
    Real n = g * ((Real)1 - r) / ((Real)1 + r) + ((Real)1 - g) * ((Real)1 + sr_) / ((Real)1 - sr_ + (Real)1e-6);
    Real k2 = (r * sqr(n + 1) - sqr(n - 1)) / ((Real)1 - r);
    c.condN.v[i] = n;
    c.condK.v[i] = sqrt_(max_(k2, (Real)0));
  }
  Real favg = savg(m.sp[SP_FUZZ]);
  c.Efo = fuzz_albedo(m.s[S_FUZZ_WEIGHT], favg, m.s[S_FUZZ_ROUGH], muo);
  c.Ef_avg = fuzz_albedo(m.s[S_FUZZ_WEIGHT], favg, m.s[S_FUZZ_ROUGH], (Real)0.5);
  c.Eco = c.wCoat * spec_albedo(m.s[S_COAT_IOR], m.s[S_COAT_ROUGH], muo);
  c.Ec_avg = c.wCoat * spec_albedo(m.s[S_COAT_IOR], m.s[S_COAT_ROUGH], (Real)0.5);
  c.Eso = spec_albedo(m.s[S_SPEC_IOR], sr, muo);
  c.Es_avg = spec_albedo(m.s[S_SPEC_IOR], sr, (Real)0.5);

  // lobe selection weights from albedos at wo
  Real above = ((Real)1 - c.Efo) * ((Real)1 - c.Eco);
  Real metalA = c.wMetal * above * savg(m.sp[SP_BASE]) * m.s[S_BASE_WEIGHT];
  Real dielA = ((Real)1 - c.wMetal) * above * (c.Eso + c.tw * ((Real)1 - c.Eso));
  Real diffA = ((Real)1 - c.wMetal) * above * ((Real)1 - c.tw) * ((Real)1 - c.Eso) * savg(m.sp[SP_BASE]) * m.s[S_BASE_WEIGHT];
  Real w0 = c.Efo, w1 = c.Eco, w2 = metalA, w3 = dielA, w4 = diffA;
  Real tot = w0 + w1 + w2 + w3 + w4;
  if (!(tot > (Real)1e-6)) { w4 = 1; tot = 1; }
  Real it = (Real)1 / tot;
  c.p[0] = w0 * it; c.p[1] = c.p[0] + w1 * it; c.p[2] = c.p[1] + w2 * it;
  c.p[3] = c.p[2] + w3 * it; c.p[4] = (Real)1;
}

// ---------------------------------------------------------------- lobes
template <int N> struct LobeOut { Spec<N> f; Real pdf; };

// Fuzz (charlie sheen), same-hemisphere only.
template <int N>
LOBE_FN void lobe_fuzz(MAT_ARG(MatRec<N>), const BsdfCtx<N>& c, V3 wo, V3 wi, Spec<N>& f, Real& pdf) {
  MAT_USE(MatRec<N>);
  f = sset<N>(0); pdf = 0;
  if (!same_hemi(wo, wi) || wo.z < 0 || m.s[S_FUZZ_WEIGHT] <= 0) return;
  V3 wm = normalize(wo + wi);
  Real sinh = safe_sqrt((Real)1 - wm.z * wm.z);
  Real inva = (Real)1 / c.fuzzA;
  Real D = ((Real)2 + inva) * pow_(max_(sinh, (Real)1e-6), inva) / ((Real)2 * PI);
  Real V = (Real)1 / ((Real)4 * (wo.z + wi.z - wo.z * wi.z));
  f = m.sp[SP_FUZZ] * (m.s[S_FUZZ_WEIGHT] * D * V);
  pdf = wi.z * INV_PI;
}

// Coat: scalar-Fresnel GGX dielectric reflection.
template <int N>
LOBE_FN void lobe_coat(MAT_ARG(MatRec<N>), const BsdfCtx<N>& c, V3 wo, V3 wi, Real& f, Real& pdf) {
  MAT_USE(MatRec<N>);
  f = 0; pdf = 0;
  if (!same_hemi(wo, wi) || wo.z < 0 || c.wCoat <= 0) return;
  Ggx g; g.ax = c.ac; g.ay = c.ac;
  V3 wm = wo + wi;
  if (length2(wm) == 0) return;
  wm = normalize(wm);
  Real F = fr_dielectric(dot(wo, wm), m.s[S_COAT_IOR]);
  f = c.wCoat * ggx_D(g, wm) * ggx_G(g, wo, wi) * F / ((Real)4 * wi.z * wo.z);
  pdf = ggx_Dw(g, wo, wm) / ((Real)4 * abs_(dot(wo, wm)));
}

// Metal: spectral complex Fresnel GGX reflection (anisotropic).
template <int N>
LOBE_FN void lobe_metal(MAT_ARG(MatRec<N>), const BsdfCtx<N>& c, V3 wo, V3 wi, Spec<N>& f, Real& pdf) {
  MAT_USE(MatRec<N>);
  (void)m;
  f = sset<N>(0); pdf = 0;
  if (!same_hemi(wo, wi) || wo.z < 0 || c.wMetal <= 0) return;
  V3 wm = wo + wi;
  if (length2(wm) == 0) return;
  wm = normalize(wm);
  Real d = dot(wo, wm);
  Real base = ggx_D(c.spec, wm) * ggx_G(c.spec, wo, wi) / ((Real)4 * wi.z * wo.z);
  for (int i = 0; i < N; ++i)
    f.v[i] = c.wMetal * base * fr_complex(d, cx(c.condN.v[i], c.condK.v[i]));
  pdf = ggx_Dw(c.spec, wo, wm) / ((Real)4 * abs_(d));
}

// Dielectric specular: reflection (per-wavelength Fresnel, optional thin film) +
// rough transmission (pbrt-v4 DielectricBxDF generalised half vector).
template <int N>
LOBE_FN Spec<N> diel_fresnel(MAT_ARG(MatRec<N>), const BsdfCtx<N>& c, Real mu) {
  MAT_USE(MatRec<N>);
  Spec<N> F;
  if (m.s[S_FILM_THICK] > 0) {
    for (int i = 0; i < N; ++i)
      F.v[i] = fr_thinfilm(abs_(mu), c.lam.v[i], m.s[S_FILM_THICK], m.s[S_FILM_IOR], m.s[S_SPEC_IOR]);
  } else {
    F = sset<N>(fr_dielectric(mu, m.s[S_SPEC_IOR]));
  }
  return F;
}

template <int N>
LOBE_FN void lobe_diel(MAT_ARG(MatRec<N>), const BsdfCtx<N>& c, V3 wo, V3 wi, Spec<N>& f, Real& pdf) {
  MAT_USE(MatRec<N>);
  f = sset<N>(0); pdf = 0;
  Real eta = m.s[S_SPEC_IOR];
  Real ci = wi.z, co = wo.z;
  if (ci == 0 || co == 0) return;
  bool refl = ci * co > 0;
  Real etap = 1;
  if (!refl) etap = co > 0 ? eta : (Real)1 / eta;
  V3 wm = wi * etap + wo;
  if (length2(wm) == 0) return;
  wm = face_forward(normalize(wm), v3(0, 0, 1));
  if (dot(wm, wi) * ci < 0 || dot(wm, wo) * co < 0) return;
  Spec<N> F = diel_fresnel<N>(MAT_PASS(m), c, dot(wo, wm));
  Real Favg = savg(F);
  Real pr = Favg, pt = ((Real)1 - Favg) * c.tw;
  if (pr + pt <= 0) return;
  Real D = ggx_D(c.spec, wm), G = ggx_G(c.spec, wo, wi);
  if (refl) {
    f = F * (D * G / abs_((Real)4 * ci * co));
    pdf = ggx_Dw(c.spec, wo, wm) / ((Real)4 * abs_(dot(wo, wm))) * pr / (pr + pt);
  } else {
    Real denom = sqr(dot(wi, wm) + dot(wo, wm) / etap) * ci * co;
    Real k = D * G * abs_(dot(wi, wm) * dot(wo, wm) / denom) / sqr(etap) * c.tw;
    for (int i = 0; i < N; ++i)
      f.v[i] = ((Real)1 - F.v[i]) * k * m.sp[SP_TRANS].v[i];
    Real dwm = abs_(dot(wi, wm)) / sqr(dot(wi, wm) + dot(wo, wm) / etap);
    pdf = ggx_Dw(c.spec, wo, wm) * dwm * pt / (pr + pt);
  }
  Real wd = (Real)1 - c.wMetal;
  f = f * wd;
}

// Oren-Nayar diffuse.
template <int N>
LOBE_FN void lobe_diffuse(MAT_ARG(MatRec<N>), const BsdfCtx<N>& c, V3 wo, V3 wi, Spec<N>& f, Real& pdf) {
  MAT_USE(MatRec<N>);
  f = sset<N>(0); pdf = 0;
  if (!same_hemi(wo, wi) || wo.z < 0) return;
  Real sigma = m.s[S_BASE_ROUGH];
  Real s2 = sigma * sigma;
  Real A = (Real)1 - s2 / ((Real)2 * (s2 + (Real)0.33));
  Real B = (Real)0.45 * s2 / (s2 + (Real)0.09);
  Real sti = safe_sqrt(sin2theta(wi)), sto = safe_sqrt(sin2theta(wo));
  Real maxc = 0;
  if (sti > (Real)1e-4 && sto > (Real)1e-4) {
    Real cp = (wi.x * wo.x + wi.y * wo.y) / (sti * sto);
    maxc = max_((Real)0, cp);
  }
  Real sa, tb;
  if (abs_(wi.z) > abs_(wo.z)) { sa = sto; tb = sti / abs_(wi.z); }
  else { sa = sti; tb = sto / abs_(wo.z); }
  Real k = (A + B * maxc * sa * tb) * INV_PI;
  Real w = ((Real)1 - c.wMetal) * ((Real)1 - c.tw) * m.s[S_BASE_WEIGHT];
  f = m.sp[SP_BASE] * (k * w);
  pdf = abs_(wi.z) * INV_PI;
}

// ---------------------------------------------------------------- stack eval / pdf
// Below-layer scale from the layers above (fuzz, coat tint), albedo-scaling form.
template <int N>
LOBE_FN void layer_scales(MAT_ARG(MatRec<N>), const BsdfCtx<N>& c, V3 wo, V3 wi,
                          Real& sCoatBelow, Spec<N>& sBase, Real& sSpecBelow) {
  MAT_USE(MatRec<N>);
  Real mui = abs_(wi.z), muo = abs_(wo.z);
  Real Efi = fuzz_albedo(m.s[S_FUZZ_WEIGHT], savg(m.sp[SP_FUZZ]), m.s[S_FUZZ_ROUGH], mui);
  Real Eci = c.wCoat * spec_albedo(m.s[S_COAT_IOR], m.s[S_COAT_ROUGH], mui);
  Real sF = ((Real)1 - c.Efo) * ((Real)1 - Efi) / max_((Real)1 - c.Ef_avg, (Real)1e-3);
  Real sC = ((Real)1 - c.Eco) * ((Real)1 - Eci) / max_((Real)1 - c.Ec_avg, (Real)1e-3);
  sCoatBelow = sF;
  // coat absorption tint along the in/out path (cosine-corrected slab length)
  Real len = (Real)1 / max_(muo, (Real)1e-3) + (Real)1 / max_(mui, (Real)1e-3);
  Spec<N> tint = spow<N>(m.sp[SP_COAT], c.wCoat * len * (Real)0.5);
  sBase = tint * (sF * sC);
  Real Esi = spec_albedo(m.s[S_SPEC_IOR], m.s[S_SPEC_ROUGH], mui);
  sSpecBelow = ((Real)1 - c.Eso) * ((Real)1 - Esi) / max_((Real)1 - c.Es_avg, (Real)1e-3);
}

template <int N>
LOBE_FN void bsdf_eval(MAT_ARG(MatRec<N>), const BsdfCtx<N>& c, V3 wo, V3 wi, Spec<N>& f, Real& pdf) {
  MAT_USE(MatRec<N>);
  Spec<N> ff, fm, fd, fl; Real pf, pm, pd, pl, fc, pc;
  lobe_fuzz<N>(MAT_PASS(m), c, wo, wi, ff, pf);
  lobe_coat<N>(MAT_PASS(m), c, wo, wi, fc, pc);
  lobe_metal<N>(MAT_PASS(m), c, wo, wi, fm, pm);
  lobe_diel<N>(MAT_PASS(m), c, wo, wi, fd, pd);
  lobe_diffuse<N>(MAT_PASS(m), c, wo, wi, fl, pl);
  Real sF; Spec<N> sB; Real sS;
  layer_scales<N>(MAT_PASS(m), c, wo, wi, sF, sB, sS);
  // fuzz on top; coat sits below fuzz; base below coat; diffuse additionally below specular
  f = ff + sset<N>(fc) * sF + (fm + fd) * sB + fl * (sB * sS);
  pdf = c.p[0] * pf + (c.p[1] - c.p[0]) * pc + (c.p[2] - c.p[1]) * pm + (c.p[3] - c.p[2]) * pd + (c.p[4] - c.p[3]) * pl;
}

// Pick a lobe by u, sample a direction from it. Returns false if no valid direction.
template <int N>
LOBE_FN bool bsdf_sample_dir(MAT_ARG(MatRec<N>), const BsdfCtx<N>& c, V3 wo, Real ul, Real u0, Real u1, Real u2,
                             V3& wi, int& lobe) {
  MAT_USE(MatRec<N>);
  lobe = ul < c.p[0] ? LOBE_FUZZ : ul < c.p[1] ? LOBE_COAT : ul < c.p[2] ? LOBE_METAL : ul < c.p[3] ? LOBE_DIEL : LOBE_DIFF;
  if (lobe == LOBE_FUZZ || lobe == LOBE_DIFF) {
    wi = sample_cosine_hemi(u0, u1);
    if (wo.z < 0) wi.z = -wi.z;
    return wi.z != 0;
  }
  if (lobe == LOBE_COAT) {
    Ggx g; g.ax = c.ac; g.ay = c.ac;
    V3 wm = ggx_sample_wm(g, wo, u0, u1);
    wi = reflect_(wo, wm);
    return same_hemi(wo, wi);
  }
  V3 wm = ggx_sample_wm(c.spec, wo, u0, u1);
  if (lobe == LOBE_METAL) {
    wi = reflect_(wo, wm);
    return same_hemi(wo, wi);
  }
  // dielectric: choose reflection vs transmission by Fresnel
  Spec<N> F = diel_fresnel<N>(MAT_PASS(m), c, dot(wo, wm));
  Real Favg = savg(F);
  Real pr = Favg, pt = ((Real)1 - Favg) * c.tw;
  if (pr + pt <= 0) return false;
  if (u2 < pr / (pr + pt)) {
    wi = reflect_(wo, wm);
    return same_hemi(wo, wi);
  }
  Real eta = m.s[S_SPEC_IOR];
  Real cosi = dot(wo, wm);
  V3 n = wm;
  Real e = eta;
  if (cosi < 0) { e = (Real)1 / e; cosi = -cosi; n = -n; }
  Real sin2i = max_((Real)0, (Real)1 - cosi * cosi);
  Real sin2t = sin2i / (e * e);
  if (sin2t >= 1) return false;
  Real cost = safe_sqrt((Real)1 - sin2t);
  wi = -wo * ((Real)1 / e) + n * (cosi / e - cost);
  return !same_hemi(wo, wi) && wi.z != 0;
}
} // namespace kl

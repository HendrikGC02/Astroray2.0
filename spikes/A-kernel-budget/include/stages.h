// Stage functions (written once; GPU kernels and CPU loops are thin wrappers over these).
// Stages: bsdf sample + state update, NEE + shadow-record emission, parameter (bytecode VM) evaluation.
#pragma once
#include "lobes.h"

namespace kl {

// Light table (float storage). type 0 = sphere (cone sampling), 1 = one-sided rectangle.
template <int N> struct LightRec {
  float type, radius, area, pad0;
  float pos[3], pad1;
  float e1[3], pad2;
  float e2[3], pad3;
  float Le[N];
};
// Alias table over lights built on the host from power (Vose); pmf stored for MIS.
template <int N> struct LightTable {
  const LightRec<N>* lights;
  const float* prob;
  const int* alias;
  const float* pmf;
  int count;
};

// Path state, structure of arrays. Pointers are device pointers (or host pointers on CPU).
template <int N> struct PathIO {
  const float *px, *py, *pz, *dx, *dy, *dz, *nx, *ny, *nz, *tx, *ty, *tz, *u, *v;
  const uint32_t *matId, *pixel, *sampleIdx, *depth;
  const float* lam[N];
  float* tp[N];    // throughput (in/out)
  float* Lacc[N];  // accumulated radiance (in/out)
  float *ndx, *ndy, *ndz, *nox, *noy, *noz; // next ray
  float* prevPdf;
  uint32_t* flags;
  // sample/eval diagnostics (also written in production variants; cheap)
  float* oF[N];
  float *oPdf, *oWx, *oWy, *oWz;
  int* oLobe;
  // NEE / shadow-record outputs
  float *sox, *soy, *soz, *sdx, *sdy, *sdz, *stmax, *spdf;
  float* scontrib[N];
  int* slight;
};

FN Frame make_frame(V3 n, V3 t) {
  Frame f;
  f.n = normalize(n);
  f.t = normalize(t - f.n * dot(f.n, t));
  f.b = cross(f.n, f.t);
  return f;
}
FN Frame frame_from_z(V3 z) {
  Frame f; f.n = z;
  Real sg = z.z >= 0 ? (Real)1 : (Real)-1;
  Real a = (Real)-1 / (sg + z.z), b = z.x * z.y * a;
  f.t = v3((Real)1 + sg * z.x * z.x * a, sg * b, -sg * z.x);
  f.b = v3(b, sg + z.y * z.y * a, -z.y);
  return f;
}

template <int N> struct Hit {
  V3 p, d;
  Frame fr;
  V3 wo;
  Spec<N> lam, tp;
  uint32_t pixel, sample, depth, mat;
  Real u, v;
};

template <int N> FN void load_hit(const PathIO<N>& io, int i, Hit<N>& h) {
  h.p = v3((Real)io.px[i], (Real)io.py[i], (Real)io.pz[i]);
  h.d = v3((Real)io.dx[i], (Real)io.dy[i], (Real)io.dz[i]);
  h.fr = make_frame(v3((Real)io.nx[i], (Real)io.ny[i], (Real)io.nz[i]),
                    v3((Real)io.tx[i], (Real)io.ty[i], (Real)io.tz[i]));
  h.wo = to_local(h.fr, -h.d);
  for (int k = 0; k < N; ++k) { h.lam.v[k] = (Real)io.lam[k][i]; h.tp.v[k] = (Real)io.tp[k][i]; }
  h.pixel = io.pixel[i]; h.sample = io.sampleIdx[i]; h.depth = io.depth[i]; h.mat = io.matId[i];
  h.u = (Real)io.u[i]; h.v = (Real)io.v[i];
}

enum { DIM_BSDF = 0, DIM_NEE = 16 };
enum { FLAG_DEAD = 1, FLAG_SPECULAR = 2 };

// ------------------------------------------------------------ BSDF sample + state update
template <int N>
FN void bsdf_part(MAT_ARG(MatRec<N>), const BsdfCtx<N>& c, const Hit<N>& h, PathIO<N>& io, int i) {
  MAT_USE(MatRec<N>);
  Rng r = make_rng(h.pixel, h.sample);
  r.dim = DIM_BSDF + h.depth * 4;
  // emission (MIS against prevPdf omitted: representative arithmetic only)
  if (m.s[S_EMISSION_LUM] > 0) {
    for (int k = 0; k < N; ++k) io.Lacc[k][i] = (float)((Real)io.Lacc[k][i] + h.tp.v[k] * m.sp[SP_EMISSION].v[k] * m.s[S_EMISSION_LUM]);
  }
  Real uo = r.next();
  Real ul = r.next(), u0 = r.next(), u1 = r.next(), u2 = r.next(), urr = r.next();
  uint32_t fl = 0;
  V3 wiw = h.d;
  Spec<N> tp = h.tp;
  Spec<N> fout = sset<N>(0); Real pdfout = 0; int lobe = -1; V3 wiL = v3(0, 0, 1);
  if (uo >= m.s[S_OPACITY]) {
    // alpha pass-through: ray continues unchanged
    wiw = h.d; fl |= FLAG_SPECULAR;
  } else {
    V3 wi; bool ok = bsdf_sample_dir<N>(MAT_PASS(m), c, h.wo, ul, u0, u1, u2, wi, lobe);
    Spec<N> f; Real pdf = 0;
    if (ok) bsdf_eval<N>(MAT_PASS(m), c, h.wo, wi, f, pdf);
    if (!ok || !(pdf > 0)) {
      fl |= FLAG_DEAD;
    } else {
      Real cosi = abs_(wi.z);
      Real sc = cosi / pdf;
      for (int k = 0; k < N; ++k) tp.v[k] *= f.v[k] * sc;
      Real mx = smax(tp);
      Real q = max_((Real)0, (Real)1 - mx);
      if (h.depth >= 3) {
        if (urr < q) fl |= FLAG_DEAD;
        else tp = tp * ((Real)1 / ((Real)1 - q));
      }
      fout = f; pdfout = pdf; wiL = wi;
      wiw = to_world(h.fr, wi);
    }
  }
  Real side = dot(wiw, h.fr.n) >= 0 ? (Real)1 : (Real)-1;
  V3 org = h.p + h.fr.n * (side * (Real)1e-4);
  io.ndx[i] = (float)wiw.x; io.ndy[i] = (float)wiw.y; io.ndz[i] = (float)wiw.z;
  io.nox[i] = (float)org.x; io.noy[i] = (float)org.y; io.noz[i] = (float)org.z;
  io.prevPdf[i] = (float)pdfout;
  io.flags[i] = fl;
  for (int k = 0; k < N; ++k) { io.tp[k][i] = (float)tp.v[k]; io.oF[k][i] = (float)fout.v[k]; }
  io.oPdf[i] = (float)pdfout; io.oLobe[i] = lobe;
  io.oWx[i] = (float)wiL.x; io.oWy[i] = (float)wiL.y; io.oWz[i] = (float)wiL.z;
}

// ------------------------------------------------------------ NEE + shadow-record emission
template <int N>
FN void nee_part(MAT_ARG(MatRec<N>), const BsdfCtx<N>& c, const Hit<N>& h, const LightTable<N> lt,
                      PathIO<N>& io, int i) {
  MAT_USE(MatRec<N>);
  Rng r = make_rng(h.pixel, h.sample);
  r.dim = DIM_NEE + h.depth * 4;
  Real ul = r.next(), us0 = r.next(), us1 = r.next();
  // light selection through the alias table
  Real x = ul * (Real)lt.count;
  int k = (int)x; if (k > lt.count - 1) k = lt.count - 1;
  Real ur = x - (Real)k;
  int sel = ur < (Real)lt.prob[k] ? k : lt.alias[k];
  Real pmf = (Real)lt.pmf[sel];
  const LightRec<N>& L = lt.lights[sel];
  V3 lp = v3((Real)L.pos[0], (Real)L.pos[1], (Real)L.pos[2]);
  V3 wiw = v3(0, 0, 1); Real dist = 1, pdfs = 0; bool valid = true;
  if (L.type < (Real)0.5) {
    Real rad = (Real)L.radius;
    V3 wc = lp - h.p; Real dc2 = length2(wc);
    if (dc2 <= rad * rad * (Real)1.0001) valid = false;
    else {
      Real sin2max = rad * rad / dc2;
      Real sinmax = sqrt_(sin2max);
      Real cosmax = safe_sqrt((Real)1 - sin2max);
      Real omc = (Real)1 - cosmax;
      Real cost = (cosmax - 1) * us0 + 1;
      Real sin2t = (Real)1 - cost * cost;
      if (sin2max < (Real)0.00068523) { sin2t = sin2max * us0; cost = sqrt_((Real)1 - sin2t); omc = sin2max * (Real)0.5; }
      Real cosa = sin2t / sinmax + cost * safe_sqrt((Real)1 - sin2t / sin2max);
      Real sina = safe_sqrt((Real)1 - cosa * cosa);
      Real phi = us1 * (Real)2 * PI;
      V3 w = v3(sina * cos_(phi), sina * sin_(phi), cosa);
      Frame sf = frame_from_z(normalize(wc));
      V3 nl = to_world(sf, -w);
      V3 pt = lp + nl * rad;
      V3 dv = pt - h.p; Real d2 = length2(dv);
      dist = sqrt_(d2); wiw = dv * ((Real)1 / dist);
      pdfs = (Real)1 / ((Real)2 * PI * omc);
    }
  } else {
    V3 e1 = v3((Real)L.e1[0], (Real)L.e1[1], (Real)L.e1[2]);
    V3 e2 = v3((Real)L.e2[0], (Real)L.e2[1], (Real)L.e2[2]);
    V3 pt = lp + e1 * (us0 - (Real)0.5) + e2 * (us1 - (Real)0.5);
    V3 nl = normalize(cross(e1, e2));
    V3 dv = pt - h.p; Real d2 = length2(dv);
    dist = sqrt_(d2); wiw = dv * ((Real)1 / dist);
    Real cl = dot(nl, -wiw);
    if (cl <= 0) valid = false;
    else pdfs = d2 / (cl * (Real)L.area);
  }
  Spec<N> contrib = sset<N>(0); Real pl = pmf * pdfs;
  if (valid && pl > 0) {
    V3 wi = to_local(h.fr, wiw);
    if (wi.z != 0) {
      Spec<N> f; Real pb;
      bsdf_eval<N>(MAT_PASS(m), c, h.wo, wi, f, pb);
      Real w = pl * pl / (pl * pl + pb * pb);
      Real sc = abs_(wi.z) * w / pl;
      for (int q = 0; q < N; ++q) contrib.v[q] = h.tp.v[q] * f.v[q] * (Real)L.Le[q] * sc;
    }
  }
  Real side = dot(wiw, h.fr.n) >= 0 ? (Real)1 : (Real)-1;
  V3 org = h.p + h.fr.n * (side * (Real)1e-4);
  io.sox[i] = (float)org.x; io.soy[i] = (float)org.y; io.soz[i] = (float)org.z;
  io.sdx[i] = (float)wiw.x; io.sdy[i] = (float)wiw.y; io.sdz[i] = (float)wiw.z;
  io.stmax[i] = (float)(dist * ((Real)1 - (Real)1e-3));
  io.spdf[i] = (float)pl; io.slight[i] = valid ? sel : -1;
  for (int q = 0; q < N; ++q) io.scontrib[q][i] = (float)contrib.v[q];
}

// ------------------------------------------------------------ stage entry points
template <int N>
FN void stage_bsdf(const float* rec, PathIO<N>& io, int i) {
  Hit<N> h; load_hit<N>(io, i, h);
  if (h.wo.z <= 0) { io.flags[i] = FLAG_DEAD; return; }
  MatRec<N> m = unpack<N>(rec);
  BsdfCtx<N> c; prep_ctx<N>(MAT_PASS(m), h.lam, h.wo.z, c);
  bsdf_part<N>(MAT_PASS(m), c, h, io, i);
}
template <int N>
FN void stage_nee(const float* rec, const LightTable<N> lt, PathIO<N>& io, int i) {
  Hit<N> h; load_hit<N>(io, i, h);
  if (h.wo.z <= 0) { io.slight[i] = -1; return; }
  MatRec<N> m = unpack<N>(rec);
  BsdfCtx<N> c; prep_ctx<N>(MAT_PASS(m), h.lam, h.wo.z, c);
  nee_part<N>(MAT_PASS(m), c, h, lt, io, i);
}
template <int N>
FN void stage_fused(const float* rec, const LightTable<N> lt, PathIO<N>& io, int i) {
  Hit<N> h; load_hit<N>(io, i, h);
  if (h.wo.z <= 0) { io.flags[i] = FLAG_DEAD; io.slight[i] = -1; return; }
  MatRec<N> m = unpack<N>(rec);
  BsdfCtx<N> c; prep_ctx<N>(MAT_PASS(m), h.lam, h.wo.z, c);
  nee_part<N>(MAT_PASS(m), c, h, lt, io, i);   // NEE reads tp before the BSDF update
  bsdf_part<N>(MAT_PASS(m), c, h, io, i);
}
// Agreement-only: BSDF value+pdf for a given (wo, wi) pair from the hit inputs.
template <int N>
FN void stage_eval(const float* rec, PathIO<N>& io, const float* wix, const float* wiy, const float* wiz, int i) {
  Hit<N> h; load_hit<N>(io, i, h);
  if (h.wo.z <= 0) { io.oPdf[i] = 0; return; }
  MatRec<N> m = unpack<N>(rec);
  BsdfCtx<N> c; prep_ctx<N>(MAT_PASS(m), h.lam, h.wo.z, c);
  V3 wi = normalize(to_local(h.fr, v3((Real)wix[i], (Real)wiy[i], (Real)wiz[i])));
  Spec<N> f; Real pdf;
  bsdf_eval<N>(MAT_PASS(m), c, h.wo, wi, f, pdf);
  for (int k = 0; k < N; ++k) io.oF[k][i] = (float)f.v[k];
  io.oPdf[i] = (float)pdf;
}

// ------------------------------------------------------------ parameter evaluation: tiny bytecode VM
enum { OP_NOP = 0, OP_CONST, OP_TEX, OP_MIX, OP_MUL, OP_ADD, OP_RAMP, OP_SETP, OP_SETS };
struct Op { uint8_t code, dst, a, b; float imm0, imm1; };
enum { VM_OPS = 16, VM_TEX = 64, VM_RAMPS = 4, VM_STOPS = 4 };

FN Real vm_tex(const float* tex, Real u, Real v) {
  Real x = u - (Real)floor(u), y = v - (Real)floor(v);
  x *= (Real)(VM_TEX - 1); y *= (Real)(VM_TEX - 1);
  int ix = (int)x, iy = (int)y; Real fx = x - (Real)ix, fy = y - (Real)iy;
  int ix1 = ix + 1 < VM_TEX ? ix + 1 : ix, iy1 = iy + 1 < VM_TEX ? iy + 1 : iy;
  Real a = lerp_(fx, (Real)tex[iy * VM_TEX + ix], (Real)tex[iy * VM_TEX + ix1]);
  Real b = lerp_(fx, (Real)tex[iy1 * VM_TEX + ix], (Real)tex[iy1 * VM_TEX + ix1]);
  return lerp_(fy, a, b);
}

template <int N>
FN void stage_param(const float* baseRec, const Op* prog, const float* tex, const float* ramps,
                         Real u, Real v, float* outRec) {
  Real sc[S_COUNT];
  Spec<N> sp[SP_COUNT];
  for (int i = 0; i < S_COUNT; ++i) sc[i] = (Real)baseRec[i];
  for (int k = 0; k < SP_COUNT; ++k)
    for (int i = 0; i < N; ++i) sp[k].v[i] = (Real)baseRec[S_COUNT + k * N + i];
  Real r[16];
  Spec<N> s[4];
  for (int i = 0; i < 16; ++i) r[i] = 0;
  for (int i = 0; i < 4; ++i) s[i] = sset<N>(0);
  for (int k = 0; k < VM_OPS; ++k) {
    Op op = prog[k];
    switch (op.code) {
      case OP_CONST: r[op.dst & 15] = (Real)op.imm0; break;
      case OP_TEX: r[op.dst & 15] = vm_tex(tex, u * (Real)op.imm0 + (Real)op.imm1, v * (Real)op.imm0); break;
      case OP_MIX: r[op.dst & 15] = lerp_((Real)op.imm0, r[op.a & 15], r[op.b & 15]); break;
      case OP_MUL: r[op.dst & 15] = r[op.a & 15] * r[op.b & 15]; break;
      case OP_ADD: r[op.dst & 15] = r[op.a & 15] + r[op.b & 15]; break;
      case OP_RAMP: {
        Real t = clamp_(r[op.a & 15], (Real)0, (Real)0.9999) * (Real)(VM_STOPS - 1);
        int s0 = (int)t; Real ft = t - (Real)s0;
        const float* base = ramps + ((int)op.imm1 & (VM_RAMPS - 1)) * VM_STOPS * N;
        for (int i = 0; i < N; ++i)
          s[op.dst & 3].v[i] = lerp_(ft, (Real)base[s0 * N + i], (Real)base[(s0 + 1) * N + i]);
        break;
      }
      case OP_SETP: sc[(int)op.imm1 % S_COUNT] = r[op.a & 15] * (Real)op.imm0; break;
      case OP_SETS: sp[(int)op.imm1 % SP_COUNT] = slerp<N>((Real)op.imm0, sp[(int)op.imm1 % SP_COUNT], s[op.a & 3]); break;
      default: break;
    }
  }
  for (int i = 0; i < S_COUNT; ++i) outRec[i] = (float)sc[i];
  for (int k = 0; k < SP_COUNT; ++k)
    for (int i = 0; i < N; ++i) outRec[S_COUNT + k * N + i] = (float)sp[k].v[i];
}
} // namespace kl

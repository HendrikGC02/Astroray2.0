// Host-only deterministic test-scene generator (plain float math, shared by the GPU and CPU harnesses so both
// executables see bit-identical inputs). Not kernel code; STL allowed here.
#pragma once
#include <vector>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "stages.h"

namespace hs {
inline float hr(uint32_t a, uint32_t b, uint32_t c) { return (float)(kl::rng_u32(a, b, c) >> 8) * (1.0f / 16777216.0f); }
struct F3 { float x, y, z; };
inline F3 unit_dir(float u, float v) {
  float z = 1 - 2 * u, r = sqrtf(fmaxf(0, 1 - z * z)), p = 6.2831853f * v;
  F3 d = {r * cosf(p), r * sinf(p), z}; return d;
}
inline F3 cross3(F3 a, F3 b) { F3 r = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; return r; }
inline float dot3(F3 a, F3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline F3 norm3(F3 a) { float l = 1.0f / sqrtf(dot3(a, a)); F3 r = {a.x * l, a.y * l, a.z * l}; return r; }

enum { NUM_MATS = 64, NUM_LIGHTS = 16 };

template <int N> struct HostScene {
  std::vector<float> table;                 // NUM_MATS * FLOATS
  std::vector<kl::Op> progs;                // NUM_MATS * VM_OPS
  std::vector<float> tex, ramps;
  std::vector<kl::LightRec<N>> lights;
  std::vector<float> lprob, lpmf; std::vector<int> lalias;
  enum { FLOATS = kl::MatLayout<N>::FLOATS };

  static void spectrum(float* out, uint32_t seed, float lo, float hi, float tilt) {
    for (int i = 0; i < N; ++i) {
      float t = N > 1 ? (float)i / (float)(N - 1) : 0.f;
      float v = lo + (hi - lo) * hr(seed, 1, 0) + tilt * (t - 0.5f) + 0.05f * (hr(seed, 2, i) - 0.5f);
      out[i] = fminf(0.98f, fmaxf(0.02f, v));
    }
  }
  void build() {
    table.assign(NUM_MATS * FLOATS, 0.f);
    for (int m = 0; m < NUM_MATS; ++m) {
      float* r = &table[m * FLOATS];
      uint32_t sd = 1000 + m;
      float jit = hr(sd, 9, 0);
      r[kl::S_BASE_WEIGHT] = 1; r[kl::S_BASE_ROUGH] = 0.3f; r[kl::S_METALNESS] = 0;
      r[kl::S_SPEC_WEIGHT] = 1; r[kl::S_SPEC_ROUGH] = 0.3f; r[kl::S_SPEC_IOR] = 1.5f; r[kl::S_SPEC_ANISO] = 0;
      r[kl::S_TRANS_WEIGHT] = 0; r[kl::S_COAT_WEIGHT] = 0; r[kl::S_COAT_ROUGH] = 0.1f; r[kl::S_COAT_IOR] = 1.5f;
      r[kl::S_FUZZ_WEIGHT] = 0; r[kl::S_FUZZ_ROUGH] = 0.5f; r[kl::S_EMISSION_LUM] = 0;
      r[kl::S_FILM_THICK] = 0; r[kl::S_FILM_IOR] = 1.4f; r[kl::S_OPACITY] = 1;
      float* sp = r + kl::S_COUNT;
      spectrum(sp + kl::SP_BASE * N, sd, 0.2f, 0.8f, 0.3f);
      spectrum(sp + kl::SP_SPEC * N, sd + 1, 0.6f, 1.0f, 0.f);
      spectrum(sp + kl::SP_TRANS * N, sd + 2, 0.7f, 1.0f, 0.f);
      spectrum(sp + kl::SP_COAT * N, sd + 3, 0.8f, 1.0f, 0.f);
      spectrum(sp + kl::SP_FUZZ * N, sd + 4, 0.4f, 0.9f, 0.2f);
      spectrum(sp + kl::SP_EMISSION * N, sd + 5, 0.5f, 1.0f, 0.f);
      switch (m % 8) {
        case 0: r[kl::S_METALNESS] = 1; r[kl::S_SPEC_ROUGH] = 0.1f + 0.3f * jit; r[kl::S_SPEC_ANISO] = 0.5f * jit;
                for (int i = 0; i < N; ++i) sp[kl::SP_BASE * N + i] = 0.55f + 0.4f * (float)i / (float)N; break; // gold-ish
        case 1: r[kl::S_METALNESS] = 1; r[kl::S_SPEC_ROUGH] = 0.4f + 0.4f * jit; break;
        case 2: r[kl::S_TRANS_WEIGHT] = 1; r[kl::S_SPEC_ROUGH] = 0.03f + 0.1f * jit; r[kl::S_SPEC_IOR] = 1.45f + 0.1f * jit; break;
        case 3: r[kl::S_TRANS_WEIGHT] = 1; r[kl::S_SPEC_ROUGH] = 0.3f + 0.3f * jit; r[kl::S_SPEC_IOR] = 1.5f; break;
        case 4: r[kl::S_COAT_WEIGHT] = 1; r[kl::S_COAT_ROUGH] = 0.05f + 0.2f * jit; r[kl::S_SPEC_ROUGH] = 0.4f; break;
        case 5: r[kl::S_FUZZ_WEIGHT] = 1; r[kl::S_FUZZ_ROUGH] = 0.3f + 0.5f * jit; r[kl::S_BASE_ROUGH] = 0.6f; break;
        case 6: r[kl::S_FILM_THICK] = 250.f + 400.f * jit; r[kl::S_SPEC_ROUGH] = 0.1f + 0.2f * jit; r[kl::S_COAT_WEIGHT] = 0.5f; break;
        case 7: r[kl::S_EMISSION_LUM] = 0.5f; r[kl::S_OPACITY] = 0.9f; r[kl::S_SPEC_ROUGH] = 0.6f; r[kl::S_BASE_ROUGH] = 0.8f; break;
      }
    }
    // programs: same 16-op shape with material-dependent constants
    progs.assign(NUM_MATS * kl::VM_OPS, kl::Op());
    for (int m = 0; m < NUM_MATS; ++m) {
      kl::Op* p = &progs[m * kl::VM_OPS];
      float j = hr(2000 + m, 0, 0);
      int k = 0;
      auto op = [&](int code, int dst, int a, int b, float i0, float i1) { kl::Op o; o.code = (uint8_t)code; o.dst = (uint8_t)dst; o.a = (uint8_t)a; o.b = (uint8_t)b; o.imm0 = i0; o.imm1 = i1; p[k++] = o; };
      op(kl::OP_TEX, 0, 0, 0, 2.f + 6.f * j, 0.f);
      op(kl::OP_CONST, 1, 0, 0, 0.5f, 0);
      op(kl::OP_MIX, 2, 0, 1, 0.3f + 0.4f * j, 0);
      op(kl::OP_MUL, 3, 2, 0, 1.f, 0);
      op(kl::OP_TEX, 4, 0, 0, 5.f, 0.37f);
      op(kl::OP_ADD, 5, 3, 4, 0, 0);
      op(kl::OP_CONST, 6, 0, 0, 0.5f, 0);
      op(kl::OP_MUL, 7, 5, 6, 0, 0);
      op(kl::OP_RAMP, 0, 7, 0, 0, (float)(m & 3));
      op(kl::OP_SETS, 0, 0, 0, 0.5f, (float)kl::SP_BASE);
      op(kl::OP_MIX, 8, 4, 3, 0.5f, 0);
      op(kl::OP_SETP, 0, 8, 0, 0.15f, (float)kl::S_BASE_ROUGH);
      op(kl::OP_CONST, 9, 0, 0, 0.9f, 0);
      op(kl::OP_MUL, 10, 9, 7, 0, 0);
      op(kl::OP_SETP, 0, 10, 0, 0.05f, (float)kl::S_SPEC_ROUGH);
      op(kl::OP_NOP, 0, 0, 0, 0, 0);
    }
    tex.resize(kl::VM_TEX * kl::VM_TEX);
    for (int i = 0; i < kl::VM_TEX * kl::VM_TEX; ++i) tex[i] = hr(77, i, 0);
    ramps.resize(kl::VM_RAMPS * kl::VM_STOPS * N);
    for (int i = 0; i < (int)ramps.size(); ++i) ramps[i] = 0.1f + 0.8f * hr(88, i, 0);
    // lights: 8 spheres + 8 rectangles around the origin
    lights.resize(NUM_LIGHTS);
    std::vector<float> power(NUM_LIGHTS);
    for (int l = 0; l < NUM_LIGHTS; ++l) {
      kl::LightRec<N>& L = lights[l];
      memset(&L, 0, sizeof(L));
      F3 d = unit_dir(hr(3000 + l, 0, 0), hr(3000 + l, 1, 0));
      float dist = 5.f + 3.f * hr(3000 + l, 2, 0);
      L.pos[0] = d.x * dist; L.pos[1] = d.y * dist; L.pos[2] = d.z * dist;
      float pe = 0;
      for (int i = 0; i < N; ++i) { L.Le[i] = 5.f + 40.f * hr(3100 + l, i, 0); pe += L.Le[i] / N; }
      if (l < 8) { L.type = 0; L.radius = 0.4f + 0.6f * hr(3000 + l, 3, 0); L.area = 4 * 3.14159265f * L.radius * L.radius; }
      else {
        L.type = 1;
        F3 up = fabsf(d.z) < 0.9f ? F3{0, 0, 1} : F3{1, 0, 0};
        F3 e1 = norm3(cross3(d, up)); F3 e2 = norm3(cross3(d, e1));
        float s = 1.0f + 1.5f * hr(3000 + l, 4, 0);
        // normal = cross(e1,e2) must point toward the origin (-d)
        F3 n = cross3(e1, e2); if (dot3(n, d) > 0) { e1.x = -e1.x; e1.y = -e1.y; e1.z = -e1.z; }
        L.e1[0] = e1.x * s; L.e1[1] = e1.y * s; L.e1[2] = e1.z * s;
        L.e2[0] = e2.x * s; L.e2[1] = e2.y * s; L.e2[2] = e2.z * s;
        L.area = s * s;
      }
      power[l] = pe * L.area / (dist * dist);
    }
    // Vose alias table
    float tot = 0; for (float p : power) tot += p;
    lpmf.resize(NUM_LIGHTS); lprob.assign(NUM_LIGHTS, 1.f); lalias.assign(NUM_LIGHTS, 0);
    std::vector<float> sc(NUM_LIGHTS); std::vector<int> sm, lg;
    for (int i = 0; i < NUM_LIGHTS; ++i) { lpmf[i] = power[i] / tot; sc[i] = lpmf[i] * NUM_LIGHTS; (sc[i] < 1 ? sm : lg).push_back(i); }
    while (!sm.empty() && !lg.empty()) {
      int s = sm.back(); sm.pop_back(); int g = lg.back(); lg.pop_back();
      lprob[s] = sc[s]; lalias[s] = g; sc[g] = sc[g] + sc[s] - 1.f;
      (sc[g] < 1 ? sm : lg).push_back(g);
    }
    for (int i : sm) { lprob[i] = 1; lalias[i] = i; }
    for (int i : lg) { lprob[i] = 1; lalias[i] = i; }
  }
};

// Per-path inputs (always float).
struct Inputs {
  int n = 0, width = 0;
  std::vector<float> px, py, pz, dx, dy, dz, nx, ny, nz, tx, ty, tz, u, v, wix, wiy, wiz;
  std::vector<uint32_t> matId, pixel, sampleIdx, depth;
  std::vector<float> lam[8], tp[8];
  void build(int count, int N, uint32_t seed = 1) {
    n = count; width = N;
    px.resize(n); py.resize(n); pz.resize(n); dx.resize(n); dy.resize(n); dz.resize(n); nx.resize(n); ny.resize(n); nz.resize(n);
    tx.resize(n); ty.resize(n); tz.resize(n); u.resize(n); v.resize(n); wix.resize(n); wiy.resize(n); wiz.resize(n);
    matId.resize(n); pixel.resize(n); sampleIdx.resize(n); depth.resize(n);
    for (int k = 0; k < N; ++k) { lam[k].resize(n); tp[k].resize(n); }
    for (int i = 0; i < n; ++i) {
      uint32_t id = (uint32_t)i + seed * 0x10001u;
      px[i] = 2.f * hr(id, 0, 0) - 1.f; py[i] = 2.f * hr(id, 0, 1) - 1.f; pz[i] = 2.f * hr(id, 0, 2) - 1.f;
      F3 d = unit_dir(hr(id, 1, 0), hr(id, 1, 1));
      F3 nn = unit_dir(hr(id, 2, 0), hr(id, 2, 1));
      if (dot3(nn, d) > 0) { nn.x = -nn.x; nn.y = -nn.y; nn.z = -nn.z; } // surface faces the incoming ray
      F3 t = unit_dir(hr(id, 3, 0), hr(id, 3, 1));
      F3 w = unit_dir(hr(id, 4, 0), hr(id, 4, 1));
      dx[i] = d.x; dy[i] = d.y; dz[i] = d.z; nx[i] = nn.x; ny[i] = nn.y; nz[i] = nn.z;
      tx[i] = t.x; ty[i] = t.y; tz[i] = t.z; wix[i] = w.x; wiy[i] = w.y; wiz[i] = w.z;
      u[i] = hr(id, 5, 0); v[i] = hr(id, 5, 1);
      matId[i] = (uint32_t)(kl::rng_u32(id, 6, 0) % NUM_MATS);
      pixel[i] = id; sampleIdx[i] = (uint32_t)(i & 63); depth[i] = (uint32_t)(kl::rng_u32(id, 7, 0) % 6);
      float l0 = hr(id, 8, 0);
      for (int k = 0; k < N; ++k) {
        float f = l0 + (float)k / (float)N; f -= floorf(f);
        lam[k][i] = 360.f + 470.f * f;
        tp[k][i] = 0.3f + 0.7f * hr(id, 9, k);
      }
    }
  }
};

// Output arrays in a fixed order; written as float32 so all executables share one comparison format.
template <int N> struct Outputs {
  std::vector<float> eF[N], ePdf;                                             // eval stage
  std::vector<float> sWx, sWy, sWz, sF[N], sPdf, sLobe, sNdx, sNdy, sNdz, sTp[N]; // sample stage
  std::vector<float> nC[N], nDx, nDy, nDz, nPdf, nLight, nTmax;               // nee stage
  std::vector<float> auxMat, auxCoso;                                         // input classes (same in every exe)
  void fill_aux(const Inputs& in) {
    auxMat.resize(in.n); auxCoso.resize(in.n);
    for (int i = 0; i < in.n; ++i) { auxMat[i] = (float)(in.matId[i] % 8); auxCoso[i] = -(in.dx[i] * in.nx[i] + in.dy[i] * in.ny[i] + in.dz[i] * in.nz[i]); }
  }
  void resize(int n) {
    for (int k = 0; k < N; ++k) { eF[k].assign(n, 0); sF[k].assign(n, 0); sTp[k].assign(n, 0); nC[k].assign(n, 0); }
    ePdf.assign(n, 0); sWx.assign(n, 0); sWy.assign(n, 0); sWz.assign(n, 0); sPdf.assign(n, 0); sLobe.assign(n, 0);
    sNdx.assign(n, 0); sNdy.assign(n, 0); sNdz.assign(n, 0); nDx.assign(n, 0); nDy.assign(n, 0); nDz.assign(n, 0);
    nPdf.assign(n, 0); nLight.assign(n, 0); nTmax.assign(n, 0);
  }
  // names + pointers, same order for write and compare
  void collect(std::vector<const std::vector<float>*>& v, std::vector<const char*>& names) const {
    static const char* en[] = {"eval.f0", "eval.f1", "eval.f2", "eval.f3", "eval.f4", "eval.f5", "eval.f6", "eval.f7"};
    for (int k = 0; k < N; ++k) { v.push_back(&eF[k]); names.push_back("eval.f"); }
    v.push_back(&ePdf); names.push_back("eval.pdf");
    v.push_back(&sWx); names.push_back("sample.wi.x"); v.push_back(&sWy); names.push_back("sample.wi.y"); v.push_back(&sWz); names.push_back("sample.wi.z");
    for (int k = 0; k < N; ++k) { v.push_back(&sF[k]); names.push_back("sample.f"); }
    v.push_back(&sPdf); names.push_back("sample.pdf");
    v.push_back(&sLobe); names.push_back("sample.lobe");
    v.push_back(&sNdx); names.push_back("sample.nextdir.x"); v.push_back(&sNdy); names.push_back("sample.nextdir.y"); v.push_back(&sNdz); names.push_back("sample.nextdir.z");
    for (int k = 0; k < N; ++k) { v.push_back(&sTp[k]); names.push_back("sample.throughput"); }
    for (int k = 0; k < N; ++k) { v.push_back(&nC[k]); names.push_back("nee.contrib"); }
    v.push_back(&nDx); names.push_back("nee.dir.x"); v.push_back(&nDy); names.push_back("nee.dir.y"); v.push_back(&nDz); names.push_back("nee.dir.z");
    v.push_back(&nPdf); names.push_back("nee.pdf_light"); v.push_back(&nLight); names.push_back("nee.light"); v.push_back(&nTmax); names.push_back("nee.tmax");
    v.push_back(&auxMat); names.push_back("aux.matclass"); v.push_back(&auxCoso); names.push_back("aux.coso");
    (void)en;
  }
  bool write(const char* path, int n) const {
    std::vector<const std::vector<float>*> v; std::vector<const char*> nm; collect(v, nm);
    FILE* f = fopen(path, "wb"); if (!f) return false;
    int cnt = (int)v.size(); fwrite(&n, 4, 1, f); fwrite(&cnt, 4, 1, f);
    for (size_t q = 0; q < v.size(); ++q) { char nb[32] = {0}; strncpy(nb, nm[q], 31); fwrite(nb, 1, 32, f); fwrite(v[q]->data(), 4, n, f); }
    fclose(f); return true;
  }
};
} // namespace hs

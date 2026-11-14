// Plain-C view of device buffers shared by the harness (cl-compiled) and the kernel TUs (nvcc-compiled).
#pragma once
#include "stages.h"

struct GpuScene {
  int n, N;
  const float *px, *py, *pz, *dx, *dy, *dz, *nx, *ny, *nz, *tx, *ty, *tz, *u, *v, *wix, *wiy, *wiz;
  const uint32_t *matId, *pixel, *sampleIdx, *depth;
  const float* lam[8];
  float *tp[8], *Lacc[8];
  float *ndx, *ndy, *ndz, *nox, *noy, *noz, *prevPdf;
  uint32_t* flags;
  float* oF[8];
  float *oPdf, *oWx, *oWy, *oWz;
  int* oLobe;
  float *sox, *soy, *soz, *sdx, *sdy, *sdz, *stmax, *spdf;
  float* scontrib[8];
  int* slight;
  // materials and lights (packed for this N)
  const float* table;     // NUM_MATS records
  float* recs;            // per-path records (output of param stage, input of split stages)
  const void* lights; const float *lprob, *lpmf; const int* lalias; int lcount;
  const kl::Op* progs; const float *tex, *ramps;
};

template <int N> kl::PathIO<N> make_io(const GpuScene& g) {
  kl::PathIO<N> io;
  io.px = g.px; io.py = g.py; io.pz = g.pz; io.dx = g.dx; io.dy = g.dy; io.dz = g.dz;
  io.nx = g.nx; io.ny = g.ny; io.nz = g.nz; io.tx = g.tx; io.ty = g.ty; io.tz = g.tz; io.u = g.u; io.v = g.v;
  io.matId = g.matId; io.pixel = g.pixel; io.sampleIdx = g.sampleIdx; io.depth = g.depth;
  for (int k = 0; k < N; ++k) { io.lam[k] = g.lam[k]; io.tp[k] = g.tp[k]; io.Lacc[k] = g.Lacc[k]; io.oF[k] = g.oF[k]; io.scontrib[k] = g.scontrib[k]; }
  io.ndx = g.ndx; io.ndy = g.ndy; io.ndz = g.ndz; io.nox = g.nox; io.noy = g.noy; io.noz = g.noz;
  io.prevPdf = g.prevPdf; io.flags = g.flags; io.oPdf = g.oPdf; io.oWx = g.oWx; io.oWy = g.oWy; io.oWz = g.oWz; io.oLobe = g.oLobe;
  io.sox = g.sox; io.soy = g.soy; io.soz = g.soz; io.sdx = g.sdx; io.sdy = g.sdy; io.sdz = g.sdz; io.stmax = g.stmax; io.spdf = g.spdf;
  io.slight = g.slight;
  return io;
}
template <int N> kl::LightTable<N> make_lights(const GpuScene& g) {
  kl::LightTable<N> t;
  t.lights = (const kl::LightRec<N>*)g.lights; t.prob = g.lprob; t.alias = g.lalias; t.pmf = g.lpmf; t.count = g.lcount;
  return t;
}

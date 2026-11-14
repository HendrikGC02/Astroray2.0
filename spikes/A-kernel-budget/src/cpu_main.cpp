// CPU target: the same stage headers compiled by MSVC. Build with /DKL_REAL_DOUBLE for the double variant.
// Usage: cpu.exe out.bin [M] [--time]   (--time: single-thread evaluations/s per stage)
#include <chrono>
#include <string>
#include "scene.h"
using namespace kl;
static const int NW = 4;

struct CpuBufs {
  std::vector<float> Lacc[NW], oF[NW], scontrib[NW], tp[NW];
  std::vector<float> ndx, ndy, ndz, nox, noy, noz, prevPdf, oPdf, oWx, oWy, oWz, sox, soy, soz, sdx, sdy, sdz, stmax, spdf;
  std::vector<uint32_t> flags; std::vector<int> oLobe, slight;
  void init(int n, const hs::Inputs& in) {
    for (int k = 0; k < NW; ++k) { Lacc[k].assign(n, 0); oF[k].assign(n, 0); scontrib[k].assign(n, 0); tp[k] = in.tp[k]; }
    for (auto* v : {&ndx, &ndy, &ndz, &nox, &noy, &noz, &prevPdf, &oPdf, &oWx, &oWy, &oWz, &sox, &soy, &soz, &sdx, &sdy, &sdz, &stmax, &spdf}) v->assign(n, 0);
    flags.assign(n, 0); oLobe.assign(n, 0); slight.assign(n, 0);
  }
  void resetTp(const hs::Inputs& in) { for (int k = 0; k < NW; ++k) tp[k] = in.tp[k]; }
};

static PathIO<NW> make_io(hs::Inputs& in, CpuBufs& b) {
  PathIO<NW> io;
  io.px = in.px.data(); io.py = in.py.data(); io.pz = in.pz.data(); io.dx = in.dx.data(); io.dy = in.dy.data(); io.dz = in.dz.data();
  io.nx = in.nx.data(); io.ny = in.ny.data(); io.nz = in.nz.data(); io.tx = in.tx.data(); io.ty = in.ty.data(); io.tz = in.tz.data();
  io.u = in.u.data(); io.v = in.v.data(); io.matId = in.matId.data(); io.pixel = in.pixel.data(); io.sampleIdx = in.sampleIdx.data(); io.depth = in.depth.data();
  for (int k = 0; k < NW; ++k) { io.lam[k] = in.lam[k].data(); io.tp[k] = b.tp[k].data(); io.Lacc[k] = b.Lacc[k].data(); io.oF[k] = b.oF[k].data(); io.scontrib[k] = b.scontrib[k].data(); }
  io.ndx = b.ndx.data(); io.ndy = b.ndy.data(); io.ndz = b.ndz.data(); io.nox = b.nox.data(); io.noy = b.noy.data(); io.noz = b.noz.data();
  io.prevPdf = b.prevPdf.data(); io.flags = b.flags.data(); io.oPdf = b.oPdf.data(); io.oWx = b.oWx.data(); io.oWy = b.oWy.data(); io.oWz = b.oWz.data();
  io.oLobe = b.oLobe.data(); io.sox = b.sox.data(); io.soy = b.soy.data(); io.soz = b.soz.data(); io.sdx = b.sdx.data(); io.sdy = b.sdy.data(); io.sdz = b.sdz.data();
  io.stmax = b.stmax.data(); io.spdf = b.spdf.data(); io.slight = b.slight.data();
  return io;
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "cpu.bin";
  int M = argc > 2 && argv[2][0] != '-' ? atoi(argv[2]) : 1000000;
  bool timing = false; for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--time") timing = true;
  hs::HostScene<NW>* hsn = new hs::HostScene<NW>(); hsn->build();
  hs::Inputs* in = new hs::Inputs(); in->build(M, NW, 7);
  CpuBufs* b = new CpuBufs(); b->init(M, *in);
  PathIO<NW> io = make_io(*in, *b);
  LightTable<NW> lt; lt.lights = hsn->lights.data(); lt.prob = hsn->lprob.data(); lt.alias = hsn->lalias.data(); lt.pmf = hsn->lpmf.data(); lt.count = hs::NUM_LIGHTS;
  enum { F = hs::HostScene<NW>::FLOATS };
  const float* table = hsn->table.data();
  hs::Outputs<NW>* o = new hs::Outputs<NW>(); o->resize(M);
  auto rec = [&](int i) { return table + (size_t)in->matId[i] * F; };
  typedef std::chrono::steady_clock Clk;
  auto secs = [](Clk::time_point a, Clk::time_point c) { return std::chrono::duration<double>(c - a).count(); };

  b->resetTp(*in);
  auto t0 = Clk::now();
  for (int i = 0; i < M; ++i) stage_eval<NW>(rec(i), io, in->wix.data(), in->wiy.data(), in->wiz.data(), i);
  auto t1 = Clk::now();
  for (int k = 0; k < NW; ++k) o->eF[k] = b->oF[k];
  o->ePdf = b->oPdf;
  b->resetTp(*in);
  for (int i = 0; i < M; ++i) stage_bsdf<NW>(rec(i), io, i);
  auto t2 = Clk::now();
  o->sWx = b->oWx; o->sWy = b->oWy; o->sWz = b->oWz;
  for (int k = 0; k < NW; ++k) { o->sF[k] = b->oF[k]; o->sTp[k] = b->tp[k]; }
  o->sPdf = b->oPdf; for (int i = 0; i < M; ++i) o->sLobe[i] = (float)b->oLobe[i];
  o->sNdx = b->ndx; o->sNdy = b->ndy; o->sNdz = b->ndz;
  b->resetTp(*in);
  for (int i = 0; i < M; ++i) stage_nee<NW>(rec(i), lt, io, i);
  auto t3 = Clk::now();
  for (int k = 0; k < NW; ++k) o->nC[k] = b->scontrib[k];
  o->nDx = b->sdx; o->nDy = b->sdy; o->nDz = b->sdz; o->nPdf = b->spdf; for (int i = 0; i < M; ++i) o->nLight[i] = (float)b->slight[i];
  o->nTmax = b->stmax;
  o->fill_aux(*in);
  o->write(path, M);
  printf("wrote %s (M=%d, Real=%s)\n", path, M, sizeof(Real) == 8 ? "double" : "float");
  if (timing) {
    // first pass above includes cache warm-up; time a second pass per stage
    b->resetTp(*in);
    auto a0 = Clk::now(); for (int i = 0; i < M; ++i) stage_eval<NW>(rec(i), io, in->wix.data(), in->wiy.data(), in->wiz.data(), i);
    auto a1 = Clk::now(); b->resetTp(*in); for (int i = 0; i < M; ++i) stage_bsdf<NW>(rec(i), io, i);
    auto a2 = Clk::now(); b->resetTp(*in); for (int i = 0; i < M; ++i) stage_nee<NW>(rec(i), lt, io, i);
    auto a3 = Clk::now();
    printf("TIME,Real=%s,eval(prep+eval+pdf),%.3f s,%.0f evals/s\n", sizeof(Real) == 8 ? "double" : "float", secs(a0, a1), M / secs(a0, a1));
    printf("TIME,Real=%s,sample(prep+sample+eval+update),%.3f s,%.0f evals/s\n", sizeof(Real) == 8 ? "double" : "float", secs(a1, a2), M / secs(a1, a2));
    printf("TIME,Real=%s,nee(prep+light+eval+shadowrec),%.3f s,%.0f evals/s\n", sizeof(Real) == 8 ? "double" : "float", secs(a2, a3), M / secs(a2, a3));
  }
  (void)t0; (void)t1; (void)t2; (void)t3;
  return 0;
}

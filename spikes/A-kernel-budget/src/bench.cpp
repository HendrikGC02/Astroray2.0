// Runtime benchmark: paths/s per kernel variant. Compiled by cl, linked against all kernel TUs.
// Usage: bench.exe [--n 1048576] [--warm 5] [--iters 20] [--burn 100] [variant ...]
// Records: "table" = material records straight from the table (what the fused kernel reads);
//          "vm"    = records produced by the bytecode parameter kernel (changes roughness/base colour per path).
#include <algorithm>
#include <cmath>
#include <string>
#include "gpu_upload.h"
#include "variants.inc"

#define DECL(n, w)                                                       \
  extern "C" void n##_fused(const GpuScene*, cudaStream_t);              \
  extern "C" void n##_bsdf(const GpuScene*, cudaStream_t);               \
  extern "C" void n##_nee(const GpuScene*, cudaStream_t);
VARIANT_LIST(DECL)
#define DECLP(n, w) extern "C" void n##_param(const GpuScene*, cudaStream_t);
PARAM_LIST(DECLP)
#define DECLF(n, w) extern "C" void n##_fvm(const GpuScene*, cudaStream_t);
FVM_LIST(DECLF)

typedef void (*Launch)(const GpuScene*, cudaStream_t);
struct Var { const char* name; int width; Launch fused, bsdf, nee; };
#define ENT(n, w) {#n, w, n##_fused, n##_bsdf, n##_nee},
static Var vars[] = {VARIANT_LIST(ENT)};
struct PVar { const char* name; int width; Launch fn; };
#define ENTP(n, w) {#n, w, n##_param},
static PVar pvars[] = {PARAM_LIST(ENTP)};
#define ENTF(n, w) {#n, w, n##_fvm},
static PVar fvars[] = {FVM_LIST(ENTF)};

static int g_warm = 5, g_iters = 20, g_burn = 100, g_n = 1 << 20;
static std::vector<std::string> g_sel;
static bool selected(const char* n) {
  if (g_sel.empty()) return true;
  for (auto& s : g_sel) if (s == n) return true;
  return false;
}

template <int N> struct Timer {
  GpuHandle<N>& h;
  cudaEvent_t a, b;
  Timer(GpuHandle<N>& hh) : h(hh) { CK(cudaEventCreate(&a)); CK(cudaEventCreate(&b)); }
  std::vector<float> run(Launch f, bool resetTp = true) {
    std::vector<float> t;
    for (int i = 0; i < g_warm + g_iters; ++i) {
      if (resetTp) h.resetTp();
      CK(cudaDeviceSynchronize());
      CK(cudaEventRecord(a)); f(&h.g, 0); CK(cudaEventRecord(b)); CK(cudaEventSynchronize(b));
      CK(cudaGetLastError());
      float ms; CK(cudaEventElapsedTime(&ms, a, b));
      if (i >= g_warm) t.push_back(ms);
    }
    std::sort(t.begin(), t.end());
    return t;
  }
  void report(const char* name, const char* kernel, const char* recs, const std::vector<float>& t) {
    printf("RESULT,%s,%s,%s,%.4f,%.4f,%.2f\n", name, kernel, recs, t.front(), t[t.size() / 2], g_n / (t[t.size() / 2] * 1e-3) / 1e6);
    fflush(stdout);
  }
};

template <int N> void checksum(GpuHandle<N>& h, const char* name, const char* what) {
  std::vector<float> pdf(h.g.n), c0(h.g.n);
  CK(cudaMemcpy(pdf.data(), h.g.oPdf, (size_t)h.g.n * 4, cudaMemcpyDeviceToHost));
  CK(cudaMemcpy(c0.data(), h.g.scontrib[0], (size_t)h.g.n * 4, cudaMemcpyDeviceToHost));
  double sp = 0, sc = 0; long bad = 0, zero = 0;
  for (int i = 0; i < h.g.n; ++i) {
    if (!std::isfinite(pdf[i]) || !std::isfinite(c0[i])) { ++bad; continue; }
    sp += std::min(pdf[i], 1e3f); sc += c0[i]; if (pdf[i] == 0) ++zero;
  }
  printf("CHECK,%s,%s,sum_pdf_clamped=%.6e,sum_nee0=%.6e,nonfinite=%ld,zero_pdf=%ld\n", name, what, sp, sc, bad, zero);
}

template <int N> void run_width() {
  bool any = false;
  for (auto& v : vars) if (v.width == N && selected(v.name)) any = true;
  for (auto& v : fvars) if (v.width == N && selected(v.name)) any = true;
  if (!any) return;
  hs::HostScene<N>* hsn = new hs::HostScene<N>(); hsn->build();
  hs::Inputs* in = new hs::Inputs(); in->build(g_n, N);
  GpuHandle<N> h; h.init(*hsn, *in);
  Timer<N> T(h);
  enum { F = hs::HostScene<N>::FLOATS };
  // identity records (table[matId]) for the split kernels
  float* recsVm = h.g.recs;
  std::vector<float> idr((size_t)g_n * F);
  for (int i = 0; i < g_n; ++i) memcpy(&idr[(size_t)i * F], &hsn->table[(size_t)in->matId[i] * F], F * 4);
  float* recsTable = h.A.up(idr);
  idr.clear(); idr.shrink_to_fit();
  Launch param = nullptr; const char* pname = "";
  for (auto& p : pvars) if (p.width == N) { param = p.fn; pname = p.name; }
  printf("INFO,width=%d,n=%d,rec_floats=%d\n", N, g_n, (int)F);
  for (int i = 0; i < g_burn; ++i) { h.resetTp(); vars[0].fused(&h.g, 0); }
  CK(cudaDeviceSynchronize());
  T.report(pname, "param", "-", T.run(param, false));
  for (auto& v : fvars) {
    if (v.width != N || !selected(v.name)) continue;
    T.report(v.name, "fused+vm", "vm", T.run(v.fn));
    h.resetTp(); v.fn(&h.g, 0); CK(cudaDeviceSynchronize()); checksum<N>(h, v.name, "fused+vm");
  }
  for (auto& v : vars) {
    if (v.width != N || !selected(v.name)) continue;
    T.report(v.name, "fused", "table", T.run(v.fused));
    h.resetTp(); v.fused(&h.g, 0); CK(cudaDeviceSynchronize()); checksum<N>(h, v.name, "fused");
    for (int pass = 0; pass < 2; ++pass) {
      h.g.recs = pass == 0 ? recsTable : recsVm;
      const char* rn = pass == 0 ? "table" : "vm";
      if (pass == 1) { param(&h.g, 0); CK(cudaDeviceSynchronize()); }
      T.report(v.name, "bsdf", rn, T.run(v.bsdf));
      T.report(v.name, "nee", rn, T.run(v.nee));
      h.resetTp(); v.nee(&h.g, 0); v.bsdf(&h.g, 0); CK(cudaDeviceSynchronize());
      checksum<N>(h, v.name, pass == 0 ? "split(table)" : "split(vm)");
    }
    h.g.recs = recsVm;
  }
  h.destroy(); delete hsn; delete in;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--n") g_n = atoi(argv[++i]);
    else if (a == "--warm") g_warm = atoi(argv[++i]);
    else if (a == "--iters") g_iters = atoi(argv[++i]);
    else if (a == "--burn") g_burn = atoi(argv[++i]);
    else g_sel.push_back(a);
  }
  cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, 0));
  printf("INFO,device=%s,cc=%d.%d,sms=%d\n", p.name, p.major, p.minor, p.multiProcessorCount);
  printf("COLUMNS,variant,kernel,records,min_ms,median_ms,Mpaths_per_s_at_median\n");
  run_width<4>();
  run_width<8>();
  return 0;
}

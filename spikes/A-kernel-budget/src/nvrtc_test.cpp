// NVRTC edit-loop measurement: compile the fused kernel source at runtime, load it, launch it.
// Usage: nvrtc_test.exe [reps]
#include <chrono>
#include <algorithm>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>
#include <nvrtc.h>
#include <cuda.h>
#include "gpu_upload.h"
typedef std::chrono::steady_clock Clk;
static double ms(Clk::time_point a, Clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
#define NV(x) do { nvrtcResult r_ = (x); if (r_ != NVRTC_SUCCESS) { fprintf(stderr, "NVRTC %s at %s:%d\n", nvrtcGetErrorString(r_), __FILE__, __LINE__); exit(3); } } while (0)
#define CU(x) do { CUresult r_ = (x); if (r_ != CUDA_SUCCESS) { const char* s_; cuGetErrorString(r_, &s_); fprintf(stderr, "CU %s at %s:%d\n", s_, __FILE__, __LINE__); exit(4); } } while (0)

static std::string readAll(const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

// Compile to PTX (arch=compute_120) or CUBIN (arch=sm_120). Returns compile ms; log printed once.
static double compile(const std::string& src, bool cubin, std::vector<char>& out, const std::vector<std::string>& extra, bool showLog) {
  auto t0 = Clk::now();
  nvrtcProgram prog; NV(nvrtcCreateProgram(&prog, src.c_str(), "rtc_kernel.cu", 0, nullptr, nullptr));
  std::vector<std::string> opt = {"-I", "include", "--std=c++17", cubin ? "--gpu-architecture=sm_120" : "--gpu-architecture=compute_120"};
  for (auto& e : extra) opt.push_back(e);
  std::vector<const char*> o; for (auto& s : opt) o.push_back(s.c_str());
  // "-I include" must be one option for NVRTC: rebuild
  std::vector<std::string> opt2; for (size_t i = 0; i < opt.size(); ++i) { if (opt[i] == "-I") { opt2.push_back("--include-path=" + opt[i + 1]); ++i; } else opt2.push_back(opt[i]); }
  std::vector<const char*> o2; for (auto& s : opt2) o2.push_back(s.c_str());
  nvrtcResult r = nvrtcCompileProgram(prog, (int)o2.size(), o2.data());
  size_t ls; nvrtcGetProgramLogSize(prog, &ls);
  if (ls > 1 && (r != NVRTC_SUCCESS || showLog)) { std::string l(ls, 0); nvrtcGetProgramLog(prog, &l[0]); fprintf(stderr, "NVRTC log: %s\n", l.c_str()); }
  if (r != NVRTC_SUCCESS) { fprintf(stderr, "compile failed: %s\n", nvrtcGetErrorString(r)); exit(5); }
  size_t sz; if (cubin) NV(nvrtcGetCUBINSize(prog, &sz)); else NV(nvrtcGetPTXSize(prog, &sz));
  out.resize(sz); if (cubin) NV(nvrtcGetCUBIN(prog, out.data())); else NV(nvrtcGetPTX(prog, out.data()));
  nvrtcDestroyProgram(&prog);
  return ms(t0, Clk::now());
}

int main(int argc, char** argv) {
  int reps = argc > 1 ? atoi(argv[1]) : 3;
  int ver[2]; nvrtcVersion(&ver[0], &ver[1]); printf("INFO,nvrtc=%d.%d\n", ver[0], ver[1]);
  const int N = 4, n = 1 << 20;
  hs::HostScene<N>* hsn = new hs::HostScene<N>(); hsn->build();
  hs::Inputs* in = new hs::Inputs(); in->build(n, N);
  GpuHandle<N> h; h.init(*hsn, *in);
  CU(cuInit(0)); CUcontext ctx; CU(cuCtxGetCurrent(&ctx));
  std::string src = readAll("src/rtc_kernel.cu");
  struct Mode { const char* name; bool cubin; std::vector<std::string> extra; };
  std::vector<Mode> modes = {{"ptx(compute_120)", false, {}}, {"cubin(sm_120)", true, {}}, {"cubin(sm_120) --Ofast-compile=max", true, {"--Ofast-compile=max"}},
                             {"cubin(sm_120) --split-compile=0", true, {"--split-compile=0"}}, {"cubin(sm_120) --pch", true, {"--pch"}}};
  for (auto& m : modes) {
    std::vector<char> img; std::vector<double> ct, lt;
    for (int r = 0; r < reps; ++r) {
      ct.push_back(compile(src, m.cubin, img, m.extra, r == 0));
      auto t0 = Clk::now(); CUmodule mod; CU(cuModuleLoadDataEx(&mod, img.data(), 0, nullptr, nullptr)); lt.push_back(ms(t0, Clk::now()));
      if (r == reps - 1) {
        CUfunction fn; CU(cuModuleGetFunction(&fn, mod, "k_fused_rtc"));
        int regs, lmem; cuFuncGetAttribute(&regs, CU_FUNC_ATTRIBUTE_NUM_REGS, fn); cuFuncGetAttribute(&lmem, CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES, fn);
        kl::PathIO<N> io = make_io<N>(h.g); kl::LightTable<N> lts = make_lights<N>(h.g);
        const float* table = h.g.table; int nn = n; void* args[] = {&io, &table, &lts, &nn};
        h.resetTp();
        auto l0 = Clk::now(); CU(cuLaunchKernel(fn, (n + 255) / 256, 1, 1, 256, 1, 1, 0, 0, args, nullptr)); CK(cudaDeviceSynchronize()); double first = ms(l0, Clk::now());
        std::vector<float> ev; for (int i = 0; i < 20; ++i) { h.resetTp(); CK(cudaDeviceSynchronize()); auto a = Clk::now(); CU(cuLaunchKernel(fn, (n + 255) / 256, 1, 1, 256, 1, 1, 0, 0, args, nullptr)); CK(cudaDeviceSynchronize()); ev.push_back((float)ms(a, Clk::now())); }
        std::sort(ev.begin(), ev.end());
        std::vector<float> pdf(n); CK(cudaMemcpy(pdf.data(), h.g.oPdf, (size_t)n * 4, cudaMemcpyDeviceToHost));
        double sp = 0; for (int i = 0; i < n; ++i) if (std::isfinite(pdf[i])) sp += std::min(pdf[i], 1e3f);
        printf("KERNEL,%s,regs=%d,local_bytes=%d,first_launch_ms=%.3f,min_ms=%.4f,median_ms=%.4f,sum_pdf_clamped=%.6e\n", m.name, regs, lmem, first, ev.front(), ev[ev.size() / 2], sp);
      }
      cuModuleUnload(mod);
    }
    auto mm = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return std::make_pair(v.front(), v[v.size() / 2]); };
    auto c = mm(ct), l = mm(lt);
    printf("NVRTC,%s,image_bytes=%zu,compile_ms_min=%.1f,compile_ms_median=%.1f,load_ms_min=%.1f,load_ms_median=%.1f\n", m.name, img.size(), c.first, c.second, l.first, l.second);
    fflush(stdout);
  }
  return 0;
}

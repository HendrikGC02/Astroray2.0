// Loads the fused kernel as an OptiX module from PTX and from OptiX-IR, builds a raygen-only pipeline, launches it.
// Usage: optix_test.exe kernel.ptx kernel.optixir [reps]
#define NOMINMAX
#include <chrono>
#include <algorithm>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>
#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>
#include <cuda.h>
#include "gpu_upload.h"
#include "optix_params.h"
typedef std::chrono::steady_clock Clk;
static double ms(Clk::time_point a, Clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
#define OK(x) do { OptixResult r_ = (x); if (r_ != OPTIX_SUCCESS) { fprintf(stderr, "OptiX error %d (%s) at %s:%d log=%s\n", (int)r_, optixGetErrorName(r_), __FILE__, __LINE__, g_log); exit(6); } } while (0)
static char g_log[8192]; static size_t g_logSize;
static std::string readAll(const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void cbLog(unsigned, const char* tag, const char* msg, void*) { fprintf(stderr, "[optix %s] %s\n", tag, msg); }
struct Rec { alignas(OPTIX_SBT_RECORD_ALIGNMENT) char header[OPTIX_SBT_RECORD_HEADER_SIZE]; };

int main(int argc, char** argv) {
  if (argc < 3) return 1;
  int reps = argc > 3 ? atoi(argv[3]) : 3;
  const int N = 4, n = 1 << 20;
  hs::HostScene<N>* hsn = new hs::HostScene<N>(); hsn->build();
  hs::Inputs* in = new hs::Inputs(); in->build(n, N);
  GpuHandle<N> h; h.init(*hsn, *in);
  CK(cudaFree(0)); CUcontext cu; cuCtxGetCurrent(&cu);
  auto t0 = Clk::now(); OK(optixInit()); double tinit = ms(t0, Clk::now());
  OptixDeviceContextOptions co = {}; co.logCallbackFunction = cbLog; co.logCallbackLevel = 2;
  OptixDeviceContext ctx; t0 = Clk::now(); OK(optixDeviceContextCreate(cu, &co, &ctx)); double tctx = ms(t0, Clk::now());
  bool nocache = false; for (int a_ = 1; a_ < argc; ++a_) if (std::string(argv[a_]) == "--nocache") nocache = true;
  if (nocache) OK(optixDeviceContextSetCacheEnabled(ctx, 0));
  printf("INFO,optixInit_ms=%.1f,contextCreate_ms=%.1f,cache=%s\n", tinit, tctx, nocache ? "disabled" : "enabled");
  OptixParams hp; hp.io = make_io<N>(h.g); hp.table = h.g.table; hp.lt = make_lights<N>(h.g); hp.n = n;
  CUdeviceptr dparams; CK(cudaMalloc((void**)&dparams, sizeof(hp))); CK(cudaMemcpy((void*)dparams, &hp, sizeof(hp), cudaMemcpyHostToDevice));
  const char* kinds[2] = {"PTX", "OptiX-IR"};
  for (int k = 0; k < 2; ++k) {
    std::string img = readAll(argv[1 + k]);
    std::vector<double> tm, tp, tpl;
    OptixModule mod = nullptr; OptixPipeline pipe = nullptr; OptixProgramGroup pg = nullptr;
    for (int r = 0; r < reps; ++r) {
      OptixModuleCompileOptions mco = {}; mco.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT; mco.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;
      OptixPipelineCompileOptions pco = {}; pco.usesMotionBlur = 0; pco.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
      pco.numPayloadValues = 2; pco.numAttributeValues = 2; pco.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE; pco.pipelineLaunchParamsVariableName = "params";
      g_logSize = sizeof(g_log);
      if (mod) { optixModuleDestroy(mod); }
      auto a = Clk::now(); OK(optixModuleCreate(ctx, &mco, &pco, img.data(), img.size(), g_log, &g_logSize, &mod)); tm.push_back(ms(a, Clk::now()));
      OptixProgramGroupOptions po = {}; OptixProgramGroupDesc pd = {}; pd.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN; pd.raygen.module = mod; pd.raygen.entryFunctionName = "__raygen__fused";
      g_logSize = sizeof(g_log); if (pg) optixProgramGroupDestroy(pg);
      a = Clk::now(); OK(optixProgramGroupCreate(ctx, &pd, 1, &po, g_log, &g_logSize, &pg)); tp.push_back(ms(a, Clk::now()));
      OptixPipelineLinkOptions plo = {}; plo.maxTraceDepth = 1; g_logSize = sizeof(g_log); if (pipe) optixPipelineDestroy(pipe);
      a = Clk::now(); OK(optixPipelineCreate(ctx, &pco, &plo, &pg, 1, g_log, &g_logSize, &pipe)); tpl.push_back(ms(a, Clk::now()));
    }
    OK(optixPipelineSetStackSize(pipe, 0, 0, 8192, 1));
    Rec rec; OK(optixSbtRecordPackHeader(pg, &rec)); CUdeviceptr drec; CK(cudaMalloc((void**)&drec, sizeof(rec))); CK(cudaMemcpy((void*)drec, &rec, sizeof(rec), cudaMemcpyHostToDevice));
    OptixShaderBindingTable sbt = {}; sbt.raygenRecord = drec;
    std::vector<float> ev;
    for (int i = 0; i < 25; ++i) {
      h.resetTp(); CK(cudaDeviceSynchronize());
      auto a = Clk::now(); OK(optixLaunch(pipe, 0, dparams, sizeof(hp), &sbt, n, 1, 1)); CK(cudaDeviceSynchronize()); if (i >= 5) ev.push_back((float)ms(a, Clk::now()));
    }
    std::sort(ev.begin(), ev.end());
    std::vector<float> pdf(n); CK(cudaMemcpy(pdf.data(), h.g.oPdf, (size_t)n * 4, cudaMemcpyDeviceToHost));
    double sp = 0; for (int i = 0; i < n; ++i) if (std::isfinite(pdf[i])) sp += std::min(pdf[i], 1e3f);
    auto mm = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return std::make_pair(v.front(), v[v.size() / 2]); };
    printf("OPTIX,%s,input_bytes=%zu,moduleCreate_ms_min=%.1f,median=%.1f,programGroup_ms_min=%.1f,pipelineCreate_ms_min=%.1f,launch_min_ms=%.4f,launch_median_ms=%.4f,sum_pdf_clamped=%.6e\n",
           kinds[k], img.size(), mm(tm).first, mm(tm).second, mm(tp).first, mm(tpl).first, ev.front(), ev[ev.size() / 2], sp);
    fflush(stdout);
  }
  return 0;
}

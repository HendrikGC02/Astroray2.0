// Same stage code as src/k_fused.cu, as an NVRTC / OptiX-module input (no CUDA runtime headers).
#include "stages.h"
using namespace kl;
#ifndef KL_WIDTH
#define KL_WIDTH 4
#endif
static const int NW = KL_WIDTH;
extern "C" __global__ void k_fused_rtc(PathIO<NW> io, const float* table, LightTable<NW> lt, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const float* rec = table + (size_t)io.matId[i] * MatLayout<NW>::FLOATS;
  stage_fused<NW>(rec, lt, io, i);
}

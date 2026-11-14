// Same stage code as k_fused.cu as an OptiX raygen program (no ray traced; measures module compile/load only).
#include <optix_device.h>
#include "optix_params.h"
using namespace kl;
extern "C" { __constant__ OptixParams params; }
extern "C" __global__ void __raygen__fused() {
  int i = (int)optixGetLaunchIndex().x;
  if (i >= params.n) return;
  PathIO<4> io = params.io;
  const float* rec = params.table + (size_t)io.matId[i] * MatLayout<4>::FLOATS;
  stage_fused<4>(rec, params.lt, io, i);
}

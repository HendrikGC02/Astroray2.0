#include "kcommon.h"
// V1 fused: material-record unpack + BSDF sample + NEE + state update in one kernel.
static __global__ void KL_LB_ATTR k_fused(PathIO<NW> io, const float* table, LightTable<NW> lt, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const float* rec = table + (size_t)io.matId[i] * MatLayout<NW>::FLOATS;
  stage_fused<NW>(rec, lt, io, i);
}
extern "C" void KL_CAT(KL_VARIANT, _fused)(const GpuScene* g, cudaStream_t s) {
  k_fused<<<(g->n + 255) / 256, 256, 0, s>>>(make_io<NW>(*g), g->table, make_lights<NW>(*g), g->n);
}

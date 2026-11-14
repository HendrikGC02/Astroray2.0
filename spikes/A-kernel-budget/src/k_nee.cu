#include "kcommon.h"
// V2(ii) split: NEE + shadow-record emission.
static __global__ void KL_LB_ATTR k_nee(PathIO<NW> io, const float* recs, LightTable<NW> lt, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  stage_nee<NW>(recs + (size_t)i * MatLayout<NW>::FLOATS, lt, io, i);
}
extern "C" void KL_CAT(KL_VARIANT, _nee)(const GpuScene* g, cudaStream_t s) {
  k_nee<<<(g->n + 255) / 256, 256, 0, s>>>(make_io<NW>(*g), g->recs, make_lights<NW>(*g), g->n);
}

#include "kcommon.h"
// V2(i) split: BSDF sample/eval + state update, record comes from the per-path parameter buffer.
static __global__ void KL_LB_ATTR k_bsdf(PathIO<NW> io, const float* recs, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  stage_bsdf<NW>(recs + (size_t)i * MatLayout<NW>::FLOATS, io, i);
}
extern "C" void KL_CAT(KL_VARIANT, _bsdf)(const GpuScene* g, cudaStream_t s) {
  k_bsdf<<<(g->n + 255) / 256, 256, 0, s>>>(make_io<NW>(*g), g->recs, g->n);
}

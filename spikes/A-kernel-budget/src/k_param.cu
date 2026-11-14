#include "kcommon.h"
// V2(iii) parameter evaluation: tiny bytecode VM fills the per-path material record.
static __global__ void KL_LB_ATTR k_param(const uint32_t* matId, const float* u, const float* v, const float* table, const Op* progs,
                                   const float* tex, const float* ramps, float* recs, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  uint32_t m = matId[i];
  stage_param<NW>(table + (size_t)m * MatLayout<NW>::FLOATS, progs + m * VM_OPS, tex, ramps, (Real)u[i], (Real)v[i],
                  recs + (size_t)i * MatLayout<NW>::FLOATS);
}
extern "C" void KL_CAT(KL_VARIANT, _param)(const GpuScene* g, cudaStream_t s) {
  k_param<<<(g->n + 255) / 256, 256, 0, s>>>(g->matId, g->u, g->v, g->table, g->progs, g->tex, g->ramps, g->recs, g->n);
}

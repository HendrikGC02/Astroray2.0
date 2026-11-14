#include "kcommon.h"
// V1b fused + parameter VM: the VM fills the record in-kernel, then BSDF sample + NEE in the same kernel.
static __global__ void KL_LB_ATTR k_fvm(PathIO<NW> io, const float* table, const Op* progs, const float* tex, const float* ramps,
                                        LightTable<NW> lt, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  uint32_t m = io.matId[i];
  float rec[MatLayout<NW>::FLOATS];
  stage_param<NW>(table + (size_t)m * MatLayout<NW>::FLOATS, progs + m * VM_OPS, tex, ramps, (Real)io.u[i], (Real)io.v[i], rec);
  stage_fused<NW>(rec, lt, io, i);
}
extern "C" void KL_CAT(KL_VARIANT, _fvm)(const GpuScene* g, cudaStream_t s) {
  k_fvm<<<(g->n + 255) / 256, 256, 0, s>>>(make_io<NW>(*g), g->table, g->progs, g->tex, g->ramps, make_lights<NW>(*g), g->n);
}

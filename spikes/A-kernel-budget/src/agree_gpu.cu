// GPU side of the agreement test: runs eval / sample / NEE stages (float, width 4) over M inputs and dumps outputs.
// Built twice: default (-fmad=true) and -fmad=false.   Usage: agree_gpu.exe out.bin [M]
#include "gpu_upload.h"
using namespace kl;
static const int NW = 4;

static __global__ void k_eval(PathIO<NW> io, const float* table, const float* wx, const float* wy, const float* wz, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x; if (i >= n) return;
  stage_eval<NW>(table + (size_t)io.matId[i] * MatLayout<NW>::FLOATS, io, wx, wy, wz, i);
}
static __global__ void k_bsdf(PathIO<NW> io, const float* table, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x; if (i >= n) return;
  stage_bsdf<NW>(table + (size_t)io.matId[i] * MatLayout<NW>::FLOATS, io, i);
}
static __global__ void k_nee(PathIO<NW> io, const float* table, LightTable<NW> lt, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x; if (i >= n) return;
  stage_nee<NW>(table + (size_t)io.matId[i] * MatLayout<NW>::FLOATS, lt, io, i);
}

template <class T> static void down(std::vector<float>& dst, const T* d, int n) {
  std::vector<T> tmp(n); CK(cudaMemcpy(tmp.data(), d, (size_t)n * sizeof(T), cudaMemcpyDeviceToHost));
  dst.resize(n); for (int i = 0; i < n; ++i) dst[i] = (float)tmp[i];
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "gpu.bin";
  int M = argc > 2 ? atoi(argv[2]) : 1000000;
  hs::HostScene<NW>* hsn = new hs::HostScene<NW>(); hsn->build();
  hs::Inputs* in = new hs::Inputs(); in->build(M, NW, 7);
  GpuHandle<NW> h; h.init(*hsn, *in);
  PathIO<NW> io = make_io<NW>(h.g); LightTable<NW> lt = make_lights<NW>(h.g);
  int blocks = (M + 255) / 256;
  hs::Outputs<NW>* o = new hs::Outputs<NW>(); o->resize(M);
  h.resetTp(); k_eval<<<blocks, 256>>>(io, h.g.table, h.g.wix, h.g.wiy, h.g.wiz, M); CK(cudaDeviceSynchronize());
  for (int k = 0; k < NW; ++k) down(o->eF[k], h.g.oF[k], M);
  down(o->ePdf, h.g.oPdf, M);
  h.resetTp(); k_bsdf<<<blocks, 256>>>(io, h.g.table, M); CK(cudaDeviceSynchronize());
  down(o->sWx, h.g.oWx, M); down(o->sWy, h.g.oWy, M); down(o->sWz, h.g.oWz, M);
  for (int k = 0; k < NW; ++k) { down(o->sF[k], h.g.oF[k], M); down(o->sTp[k], h.g.tp[k], M); }
  down(o->sPdf, h.g.oPdf, M); down(o->sLobe, h.g.oLobe, M);
  down(o->sNdx, h.g.ndx, M); down(o->sNdy, h.g.ndy, M); down(o->sNdz, h.g.ndz, M);
  h.resetTp(); k_nee<<<blocks, 256>>>(io, h.g.table, lt, M); CK(cudaDeviceSynchronize());
  for (int k = 0; k < NW; ++k) down(o->nC[k], h.g.scontrib[k], M);
  down(o->nDx, h.g.sdx, M); down(o->nDy, h.g.sdy, M); down(o->nDz, h.g.sdz, M);
  down(o->nPdf, h.g.spdf, M); down(o->nLight, h.g.slight, M); down(o->nTmax, h.g.stmax, M);
  o->fill_aux(*in);
  o->write(path, M);
  printf("wrote %s (M=%d)\n", path, M);
  return 0;
}

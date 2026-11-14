// Host-only: upload a HostScene + Inputs to the GPU and fill a GpuScene. Used by bench.cpp and agree_gpu.cu.
#pragma once
#include <cuda_runtime.h>
#include <vector>
#include <stdlib.h>
#include "gpu_scene.h"
#include "scene.h"

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { fprintf(stderr, "CUDA error %s at %s:%d\n", cudaGetErrorString(e_), __FILE__, __LINE__); exit(2); } } while (0)

struct DevAlloc {
  std::vector<void*> ptrs;
  template <class T> T* up(const std::vector<T>& v) {
    T* d; CK(cudaMalloc(&d, v.size() * sizeof(T))); CK(cudaMemcpy(d, v.data(), v.size() * sizeof(T), cudaMemcpyHostToDevice)); ptrs.push_back(d); return d;
  }
  template <class T> T* zero(size_t n) { T* d; CK(cudaMalloc(&d, n * sizeof(T))); CK(cudaMemset(d, 0, n * sizeof(T))); ptrs.push_back(d); return d; }
  void freeAll() { for (void* p : ptrs) cudaFree(p); ptrs.clear(); }
};

template <int N> struct GpuHandle {
  GpuScene g; DevAlloc A;
  std::vector<float*> tp0; // pristine throughput copies for resetting between launches
  hs::HostScene<N>* hostScene; hs::Inputs* in;
  void init(hs::HostScene<N>& hsn, hs::Inputs& inp) {
    hostScene = &hsn; in = &inp;
    int n = inp.n; memset(&g, 0, sizeof(g)); g.n = n; g.N = N;
    g.px = A.up(inp.px); g.py = A.up(inp.py); g.pz = A.up(inp.pz); g.dx = A.up(inp.dx); g.dy = A.up(inp.dy); g.dz = A.up(inp.dz);
    g.nx = A.up(inp.nx); g.ny = A.up(inp.ny); g.nz = A.up(inp.nz); g.tx = A.up(inp.tx); g.ty = A.up(inp.ty); g.tz = A.up(inp.tz);
    g.u = A.up(inp.u); g.v = A.up(inp.v); g.wix = A.up(inp.wix); g.wiy = A.up(inp.wiy); g.wiz = A.up(inp.wiz);
    g.matId = A.up(inp.matId); g.pixel = A.up(inp.pixel); g.sampleIdx = A.up(inp.sampleIdx); g.depth = A.up(inp.depth);
    for (int k = 0; k < N; ++k) {
      g.lam[k] = A.up(inp.lam[k]); g.tp[k] = A.up(inp.tp[k]); tp0.push_back(A.up(inp.tp[k]));
      g.Lacc[k] = A.zero<float>(n); g.oF[k] = A.zero<float>(n); g.scontrib[k] = A.zero<float>(n);
    }
    g.ndx = A.zero<float>(n); g.ndy = A.zero<float>(n); g.ndz = A.zero<float>(n);
    g.nox = A.zero<float>(n); g.noy = A.zero<float>(n); g.noz = A.zero<float>(n); g.prevPdf = A.zero<float>(n);
    g.flags = A.zero<uint32_t>(n);
    g.oPdf = A.zero<float>(n); g.oWx = A.zero<float>(n); g.oWy = A.zero<float>(n); g.oWz = A.zero<float>(n); g.oLobe = A.zero<int>(n);
    g.sox = A.zero<float>(n); g.soy = A.zero<float>(n); g.soz = A.zero<float>(n);
    g.sdx = A.zero<float>(n); g.sdy = A.zero<float>(n); g.sdz = A.zero<float>(n);
    g.stmax = A.zero<float>(n); g.spdf = A.zero<float>(n); g.slight = A.zero<int>(n);
    g.table = A.up(hsn.table);
    g.recs = A.zero<float>((size_t)n * hs::HostScene<N>::FLOATS);
    g.lights = A.up(hsn.lights); g.lprob = A.up(hsn.lprob); g.lpmf = A.up(hsn.lpmf); g.lalias = A.up(hsn.lalias);
    g.lcount = hs::NUM_LIGHTS;
    g.progs = A.up(hsn.progs); g.tex = A.up(hsn.tex); g.ramps = A.up(hsn.ramps);
  }
  void resetTp() { for (int k = 0; k < N; ++k) CK(cudaMemcpy(g.tp[k], tp0[k], (size_t)g.n * 4, cudaMemcpyDeviceToDevice)); }
  void destroy() { A.freeAll(); tp0.clear(); }
};

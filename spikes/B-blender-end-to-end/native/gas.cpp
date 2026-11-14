// Optional spike-B part: OptiX 9.1 triangle GAS per mesh + TLAS over the scene's objects. Throwaway.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cuda_runtime.h>
#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>
#include <cstdio>
#include <vector>
#include "sb.h"

#define EXPORT extern "C" __declspec(dllexport)
static OptixDeviceContext g_ctx = nullptr;
static char g_err[512];

static double qms() { return sb_now_ns() * 1e-6; }
#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { snprintf(g_err, sizeof g_err, "%s:%d cuda %s", __FILE__, __LINE__, cudaGetErrorString(e_)); return -1; } } while (0)
#define OK(x) do { OptixResult r_ = (x); if (r_ != OPTIX_SUCCESS) { snprintf(g_err, sizeof g_err, "%s:%d optix %d", __FILE__, __LINE__, (int)r_); return -2; } } while (0)

EXPORT const char* sb_gas_error() { return g_err; }
EXPORT int sb_gas_init() {
  CK(cudaFree(0));
  OK(optixInit());
  OptixDeviceContextOptions o = {};
  OK(optixDeviceContextCreate(0, &o, &g_ctx));
  return 0;
}

static size_t al(size_t v, size_t a) { return (v + a - 1) / a * a; }

// out[0]=h2d ms, [1]=GAS build ms (all meshes, one sync), [2]=compaction ms, [3]=TLAS ms, [4]=total ms,
// [5]=GAS bytes before compaction, [6]=GAS bytes after, [7]=TLAS bytes, [8]=GAS count, [9]=instance count
EXPORT int sb_gas_build(void* sp, double* out) {
  SbScene* s = (SbScene*)sp;
  double t_start = qms();
  cudaStream_t st = 0;
  // 1. pack-free upload: two arenas, one async copy per mesh
  size_t vb = 0, ib = 0; std::vector<int> ids;
  std::vector<size_t> voff, ioff;
  for (size_t i = 0; i < s->meshes.size(); i++) if (s->meshes[i].pos) { ids.push_back((int)i); voff.push_back(vb); ioff.push_back(ib); vb += al(s->meshes[i].nv * 12, 256); ib += al(s->meshes[i].nt * 12, 256); }
  char *dV, *dI; CK(cudaMalloc(&dV, vb)); CK(cudaMalloc(&dI, ib));
  for (size_t k = 0; k < ids.size(); k++) {
    SbMesh& m = s->meshes[ids[k]];
    CK(cudaMemcpyAsync(dV + voff[k], m.pos, m.nv * 12, cudaMemcpyHostToDevice, st));
    CK(cudaMemcpyAsync(dI + ioff[k], m.tri, m.nt * 12, cudaMemcpyHostToDevice, st));
  }
  CK(cudaStreamSynchronize(st));
  double t_h2d = qms();
  // 2. memory usage
  OptixAccelBuildOptions ao = {}; ao.buildFlags = OPTIX_BUILD_FLAG_ALLOW_COMPACTION | OPTIX_BUILD_FLAG_PREFER_FAST_TRACE; ao.operation = OPTIX_BUILD_OPERATION_BUILD;
  static const unsigned flag = OPTIX_GEOMETRY_FLAG_NONE;
  std::vector<OptixBuildInput> bis(ids.size()); std::vector<CUdeviceptr> dverts(ids.size());
  std::vector<size_t> outoff(ids.size()); size_t outb = 0, tmpmax = 0;
  for (size_t k = 0; k < ids.size(); k++) {
    SbMesh& m = s->meshes[ids[k]];
    dverts[k] = (CUdeviceptr)(dV + voff[k]);
    OptixBuildInput& bi = bis[k]; memset(&bi, 0, sizeof bi); bi.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    auto& ta = bi.triangleArray; ta.vertexBuffers = &dverts[k]; ta.numVertices = (unsigned)m.nv; ta.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3; ta.vertexStrideInBytes = 12;
    ta.indexBuffer = (CUdeviceptr)(dI + ioff[k]); ta.numIndexTriplets = (unsigned)m.nt; ta.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3; ta.indexStrideInBytes = 12;
    ta.flags = &flag; ta.numSbtRecords = 1;
    OptixAccelBufferSizes sz; OK(optixAccelComputeMemoryUsage(g_ctx, &ao, &bi, 1, &sz));
    outoff[k] = outb; outb += al(sz.outputSizeInBytes, 128); if (sz.tempSizeInBytes > tmpmax) tmpmax = sz.tempSizeInBytes;
  }
  char *dOut, *dTmp; size_t* dCompSz; CK(cudaMalloc(&dOut, outb)); CK(cudaMalloc(&dTmp, al(tmpmax, 128))); CK(cudaMalloc(&dCompSz, ids.size() * 8 + 8));
  // 3. build all, one sync
  std::vector<OptixTraversableHandle> hgas(ids.size());
  for (size_t k = 0; k < ids.size(); k++) {
    OptixAccelEmitDesc em = {}; em.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE; em.result = (CUdeviceptr)(dCompSz + k);
    OK(optixAccelBuild(g_ctx, st, &ao, &bis[k], 1, (CUdeviceptr)dTmp, tmpmax, (CUdeviceptr)(dOut + outoff[k]), 1ull << 40, &hgas[k], &em, 1));
  }
  CK(cudaStreamSynchronize(st));
  double t_build = qms();
  // 4. compaction
  std::vector<size_t> csz(ids.size()); CK(cudaMemcpy(csz.data(), dCompSz, ids.size() * 8, cudaMemcpyDeviceToHost));
  size_t cb = 0; std::vector<size_t> coff(ids.size()); for (size_t k = 0; k < ids.size(); k++) { coff[k] = cb; cb += al(csz[k], 128); }
  char* dC; CK(cudaMalloc(&dC, cb));
  std::vector<OptixTraversableHandle> cgas(ids.size());
  for (size_t k = 0; k < ids.size(); k++) OK(optixAccelCompact(g_ctx, st, hgas[k], (CUdeviceptr)(dC + coff[k]), csz[k], &cgas[k]));
  CK(cudaStreamSynchronize(st));
  double t_comp = qms();
  // 5. TLAS over objects
  std::vector<int> idpos(s->meshes.size(), -1); for (size_t k = 0; k < ids.size(); k++) idpos[ids[k]] = (int)k;
  std::vector<OptixInstance> inst; inst.reserve((size_t)s->nobj);
  for (size_t i = 0; i < (size_t)s->nobj; i++) {
    if (idpos[s->obj_mesh[i]] < 0) continue;   // empty mesh (e.g. a geometry-nodes host object): no geometry to instance
    inst.emplace_back(); OptixInstance& o = inst.back(); memset(&o, 0, sizeof o);
    memcpy(o.transform, s->obj_xf + i * 16, 12 * 4);  // row-major 3x4 = first 12 floats of row-major 4x4
    o.instanceId = (unsigned)i; o.visibilityMask = 255; o.flags = OPTIX_INSTANCE_FLAG_NONE; o.traversableHandle = cgas[idpos[s->obj_mesh[i]]];
  }
  size_t ni = inst.size();
  char* dInst; CK(cudaMalloc(&dInst, ni * sizeof(OptixInstance))); CK(cudaMemcpy(dInst, inst.data(), ni * sizeof(OptixInstance), cudaMemcpyHostToDevice));
  OptixBuildInput ti = {}; ti.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES; ti.instanceArray.instances = (CUdeviceptr)dInst; ti.instanceArray.numInstances = (unsigned)ni;
  OptixAccelBuildOptions to = {}; to.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE; to.operation = OPTIX_BUILD_OPERATION_BUILD;
  OptixAccelBufferSizes tsz; OK(optixAccelComputeMemoryUsage(g_ctx, &to, &ti, 1, &tsz));
  char *dTT, *dTO; CK(cudaMalloc(&dTT, tsz.tempSizeInBytes)); CK(cudaMalloc(&dTO, tsz.outputSizeInBytes));
  OptixTraversableHandle htlas; OK(optixAccelBuild(g_ctx, st, &to, &ti, 1, (CUdeviceptr)dTT, tsz.tempSizeInBytes, (CUdeviceptr)dTO, tsz.outputSizeInBytes, &htlas, nullptr, 0));
  CK(cudaStreamSynchronize(st));
  double t_end = qms();
  out[0] = t_h2d - t_start; out[1] = t_build - t_h2d; out[2] = t_comp - t_build; out[3] = t_end - t_comp; out[4] = t_end - t_start;
  out[5] = (double)outb; out[6] = (double)cb; out[7] = (double)tsz.outputSizeInBytes; out[8] = (double)ids.size(); out[9] = (double)ni;
  cudaFree(dV); cudaFree(dI); cudaFree(dOut); cudaFree(dTmp); cudaFree(dCompSz); cudaFree(dC); cudaFree(dInst); cudaFree(dTT); cudaFree(dTO);
  return 0;
}

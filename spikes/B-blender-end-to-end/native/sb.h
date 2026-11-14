// Shared types for the spike-B native test module. Throwaway.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

struct SbMesh {
  float* pos = nullptr;      int64_t nv = 0;     // float3 per vertex
  int32_t* tri = nullptr;    int64_t nt = 0;     // 3 vertex indices per triangle
  float* nrm = nullptr;      int64_t nn = 0;     // float3 per corner (or per vertex)
  float* uv = nullptr;       int64_t nuv = 0;    // float2 per corner
  int32_t* tloops = nullptr;                     // 3 corner indices per triangle
  int32_t* tmat = nullptr;                       // material index per triangle
};

struct SbScene {
  std::vector<SbMesh> meshes;
  int32_t* obj_mesh = nullptr;  int64_t nobj = 0;
  float* obj_xf = nullptr;                       // 16 floats per object, row-major 4x4
  std::vector<std::vector<float>> materials;
};

extern "C" __declspec(dllexport) double sb_now_ns();

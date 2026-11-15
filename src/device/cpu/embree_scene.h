// SPDX-License-Identifier: Apache-2.0
// Embree acceleration structures of the CPU device: one RTCDevice per CPU device, one
// triangle scene per geometry key and one instance scene per generation. Embree headers
// stay in embree_scene.cpp and embree_trace.cpp (0001 D5.2); this header is opaque.
// Embree traverses in single precision whatever Real is (0002 C3).
// Ref: docs/decisions/0002-kernel-architecture.md D11 (robust traversal, instance mask);
// 0007 d11 (one structure per geometry key, the instance scene is always rebuilt);
// Embree 4 API documentation (RenderKit/embree, Apache-2.0).
#pragma once
#include <cstdint>
#include <memory>

#include "host/ledger.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/thread_pool.h"

namespace astroray::host {

// Created with Embree's internal tasking, never process-global. Must outlive its scenes.
class EmbreeDevice {
 public:
  EmbreeDevice();  // throws std::runtime_error with Embree's message
  ~EmbreeDevice();
  EmbreeDevice(const EmbreeDevice&) = delete;
  EmbreeDevice& operator=(const EmbreeDevice&) = delete;

 private:
  struct Impl;
  friend class EmbreeScene;
  std::unique_ptr<Impl> impl_;
};

// The built structures of one instance table. Immutable after construction.
class EmbreeScene {
 public:
  // Builds one robust triangle scene per distinct geometry key of `table` (on `pool`), using
  // each mesh's representative motion step, then the instance scene carrying each instance's
  // transform and its 8-bit mask. Records the structures' host memory in `ledger` under
  // ACCELERATION. Throws std::runtime_error naming the instance when a geometry key has no
  // mesh in `meshes`, and with Embree's text when Embree reports an error.
  EmbreeScene(EmbreeDevice& device, const MeshStore& meshes, const InstanceTable& table, Ledger& ledger,
              ThreadPool& pool);
  ~EmbreeScene();
  EmbreeScene(const EmbreeScene&) = delete;
  EmbreeScene& operator=(const EmbreeScene&) = delete;

  uint32_t instance_count() const;
  uint64_t acceleration_bytes() const;  // as recorded in the ledger
  // The instance scene as an RTCScene, for embree_trace.cpp.
  void* handle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace astroray::host

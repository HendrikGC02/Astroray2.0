// SPDX-License-Identifier: Apache-2.0
// The seam between the scene's table sets (host/scene/scene_tables.h) and the traversal
// structures of a backend (Embree on the CPU, OptiX on CUDA), which only src/device/ may name
// (0001 D5.2). A scene owns one AccelBuilder; each scene snapshot gets one immutable SnapshotAccel
// from it, held by the snapshot's table set and so freed by its last holder (RULINGS 70). Mesh
// geometry structures are shared between consecutive snapshots by the identity of the mesh record
// they were built from, so an edit that moves an instance rebuilds only the instance structure.
// Ref: docs/decisions/0002-kernel-architecture.md D11, D13; 0007 d9, d11; RULINGS 70.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "host/ledger.h"
#include "host/scene/instances.h"
#include "kernel/light/light_record.h"

namespace astroray::host {

struct MeshRecord;
struct MeshCopy;

// One mesh of a snapshot: the host record (identity of the geometry) and its device arrays (null
// on the CPU, where Embree reads the record).
struct AccelMesh {
  std::shared_ptr<const MeshRecord> record;
  std::shared_ptr<const MeshCopy> copy;
};

// What one snapshot's structures are built from. `instances` already carries one record per lamp
// (geom key 0, a light index); `lights` is the snapshot's light records, empty without lamps.
struct AccelInput {
  const InstanceTable* instances = nullptr;  // null: an empty scene
  const std::vector<ar::LightRecord>* lights = nullptr;
  std::map<ar_key, AccelMesh> meshes;        // every key the instances reference
  std::shared_ptr<Ledger> host_ledger;       // where host-memory structures are recorded
};

// The structures of one snapshot. Immutable; frees what it holds, shared parts excepted, on
// destruction.
class SnapshotAccel {
 public:
  virtual ~SnapshotAccel() = default;
  // What SceneData::accel holds: the EmbreeScene on the CPU, the TLAS handle value on CUDA.
  virtual const void* kernel_accel() const = 0;
  uint32_t gas_built = 0, gas_reused = 0;  // mesh structures built for / carried over into this one
  double build_ms = 0;                     // wall time of the build
  uint64_t bytes = 0;                      // device memory it holds, shared parts included (0 on the CPU)
};

struct AccelCounters {
  uint64_t gas_builds = 0;   // mesh geometry structures built, lamp unit shapes included
  uint64_t gas_reused = 0;   // carried over by identity
  uint64_t tlas_builds = 0;  // instance structures built
};

class AccelBuilder {
 public:
  virtual ~AccelBuilder() = default;
  // Builds the structures of `in`, reusing the mesh structures of `prev` (the previous snapshot's,
  // may be null) whose record is the same object. An empty input gives an empty valid structure.
  // Throws std::runtime_error naming the instance or key on bad input and with the backend's
  // text on a backend failure; nothing is kept then.
  virtual std::shared_ptr<const SnapshotAccel> build(const AccelInput& in, const SnapshotAccel* prev) = 0;
  virtual AccelCounters counters() const = 0;
};

// The CPU's builder (defined in src/device/cpu/embree_scene.cpp): owns the scene's Embree device.
std::unique_ptr<AccelBuilder> make_cpu_accel_builder();

class Device;
// The CUDA scene's builder on `device` (the scene's CudaDevice, ordinal `device_index`); runs on
// the scene's init thread. Defined in src/device/cuda/optix_accel.cpp, which exists only in
// ASTRORAY_GPU builds. `cache_dir` is ar_scene_desc.cache_dir, the OptiX disk-cache directory of
// the scene's context (0007 d45); empty turns the disk cache off, so no compilation is cached to
// the default location.
// Throws DeviceError when OptiX cannot start.
std::unique_ptr<AccelBuilder> make_optix_accel_builder(Device& device, uint32_t device_index,
                                                       const std::string& cache_dir);

// A CUDA session's traversal stages (INTERSECT_CLOSEST, INTERSECT_SHADOW) run on its own OptiX
// pipeline, built on the scene's context (0002 D11; RULINGS 70). Host code names no CUDA device
// type (0001 D5.2), so these take the session's Device, which must be a CudaDevice, and are
// defined in src/device/cuda/cuda_device.cpp (ASTRORAY_GPU builds only).
struct TraversalAttach {
  double module_ms = 0, pipeline_ms = 0;  // creation wall time of the module and the pipeline
};
// Builds the pipeline on `scene_accel`'s OptiX context (the scene's builder from
// make_optix_accel_builder); its SBT and launch parameters go to `session_device`'s ledger under
// `owner`. Detach before the scene's context goes. Throws std::invalid_argument for a device or
// builder that is not CUDA/OptiX, std::logic_error when one is attached, DeviceError from OptiX.
TraversalAttach attach_cuda_traversal(Device& session_device, AccelBuilder& scene_accel, Owner owner);
// Waits for the pipeline's launches and frees it; nothing when none is attached.
void detach_cuda_traversal(Device& session_device);
// The traversable the traversal stages trace: the pinned snapshot's SceneData::accel.
void set_cuda_traversable(Device& session_device, uint64_t handle);

}  // namespace astroray::host

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

}  // namespace astroray::host

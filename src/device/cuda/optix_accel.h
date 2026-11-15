// SPDX-License-Identifier: Apache-2.0
// Acceleration structures. A GAS is one compacted triangle structure per mesh record (or one unit
// lamp shape), a TLAS one instance structure per scene snapshot. Both are immutable once built and
// held by shared_ptr: a snapshot's TLAS keeps the GAS it instances alive, a later snapshot shares
// the GAS of every unchanged record, and each buffer is freed by its last holder, so nothing a
// pinned snapshot traces is ever freed under it (RULINGS 70). Every buffer, the temporary arena
// included, is a ledger record of category ACCELERATION (instance arrays are TABLES) made through
// the OptixContext's device.
// OptixAccelBuilder is the scene's builder (host/scene/accel.h); OptixAccel is the older stateful
// view of the same builders for the traversal tests: it tracks uploads by version and reports
// counters.
// Ref: OptiX 9.1 Programming Guide, acceleration structure build and compaction;
// docs/decisions/0007-blender-integration.md d11, d14; 0002 D11; RULINGS 70.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "device/cuda/optix_context.h"
#include "device/cuda/scene_upload.h"
#include "host/scene/accel.h"
#include "kernel/light/light_record.h"

namespace astroray::host {

// Hit-group record of an instance (the instance's sbtOffset), shared with the pipeline's SBT: the
// mesh and rectangle-lamp triangles, the disk-lamp custom primitive, the lamp built-in sphere.
inline constexpr uint32_t kSbtTriangle = 0, kSbtLampDisk = 1, kSbtLampSphere = 2, kSbtHitGroups = 3;

// One built geometry structure. `record` is the identity it was built from (null for a lamp shape).
struct OptixGas {
  ~OptixGas();
  Device* device = nullptr;
  DeviceBuffer buffer;
  uint64_t handle = 0, emitted = 0, uncompacted = 0;
  std::shared_ptr<const MeshRecord> record;
};

// One built instance structure and the geometry structures it instances. `handle` is 0 only for a
// TLAS built without instances when OptiX has no empty structure to give.
struct OptixTlas {
  ~OptixTlas();
  Device* device = nullptr;
  DeviceBuffer buffer, instances;
  uint64_t handle = 0;
  uint32_t instance_count = 0;
  std::vector<std::shared_ptr<const OptixGas>> keep;
};

// What one mesh GAS is built from: the arrays of its device copy.
struct OptixGasJob {
  ar_key key = 0;
  DeviceMeshView view{};
  std::shared_ptr<const MeshRecord> record;
};

class OptixAccelBuilder : public AccelBuilder {
 public:
  OptixAccelBuilder(OptixContext& context, Owner owner) : context_(context), owner_(owner) {}

  // The scene's builder: GAS of every record the previous snapshot did not build, the lamp shapes,
  // and the TLAS. A mesh without triangles has no GAS and its instances are left out; a delta
  // point lamp has no primitive and is left out. Throws std::runtime_error for a light index out
  // of range, a light that is not a lamp, a lamp whose mask carries the shadow bit, and an
  // instance whose key has no mesh, and DeviceError for an OptiX or CUDA failure.
  std::shared_ptr<const SnapshotAccel> build(const AccelInput& in, const SnapshotAccel* prev) override;
  AccelCounters counters() const override { return {gas_builds_, gas_reused_, tlas_builds_}; }

  // The building blocks (also OptixAccel's). Triangle GAS in batches from one temporary arena.
  std::vector<std::shared_ptr<OptixGas>> build_gas(const std::vector<OptixGasJob>& jobs);
  // The unit shape of a lamp (an ar::LampShape: RECT, DISK or SPHERE), built uncompacted.
  std::shared_ptr<OptixGas> build_lamp_gas(uint8_t shape);
  // `mesh(instance, key)` gives the GAS of a mesh instance, or null to leave it out, and throws
  // for a key it cannot resolve; `lamp(shape)` gives the shared unit GAS of a lamp shape.
  using MeshLookup = std::function<std::shared_ptr<const OptixGas>(uint32_t instance, ar_key key)>;
  using LampLookup = std::function<std::shared_ptr<const OptixGas>(uint8_t shape)>;
  std::shared_ptr<OptixTlas> build_tlas(const InstanceTable* table, const std::vector<ar::LightRecord>& lights,
                                        const MeshLookup& mesh, const LampLookup& lamp);

 private:
  OptixContext& context_;
  Owner owner_;
  std::atomic<uint64_t> gas_builds_{0}, gas_reused_{0}, tlas_builds_{0};
};

class OptixAccel {
 public:
  OptixAccel(OptixContext& context, Owner owner) : core_(context, owner), context_(context) {}
  OptixAccel(const OptixAccel&) = delete;
  OptixAccel& operator=(const OptixAccel&) = delete;

  // Builds a GAS for every uploaded mesh that has none or whose version moved and frees
  // the GAS of keys no longer uploaded. Meshes without triangles get no GAS. Returns the
  // number of GAS built.
  uint32_t update_gas(const SceneUpload& upload);
  // Rebuilds the TLAS over `table`'s instances (transform, mask, flags from task 036).
  // Only instances of a mesh with zero triangles are left out. An instance with a light index is
  // a lamp: its unit-shape GAS (built once per shape, kept across generations) is placed by
  // lamp_geometry_xform from `lights[light_index]`, the table's transform being ignored; a delta
  // point lamp has no primitive and is left out. `lights` is the scene's light table, passed
  // explicitly (empty for a scene without lamps) so a forgotten argument cannot drop lamps.
  // Throws std::runtime_error for a light index out of range or naming a light that is not a lamp,
  // naming the key when a mesh with triangles has no GAS or one older than its data (call
  // update_gas first) or when an instance's key was never uploaded.
  void build_tlas(const SceneUpload& upload, const InstanceTable& table, const std::vector<ar::LightRecord>& lights);

  // OptixTraversableHandle; 0 when there is none or when update_gas changed a GAS since the
  // last build_tlas (the TLAS of that build is no longer the current geometry).
  uint64_t tlas_handle() const { return tlas_stale_ || !tlas_ ? 0 : tlas_->handle; }
  uint64_t gas_handle(ar_key key) const;
  uint64_t gas_bytes(ar_key key) const;  // compacted size; 0 when the key has no GAS
  uint64_t gas_emitted_size(ar_key key) const;      // OPTIX_PROPERTY_TYPE_COMPACTED_SIZE of the build
  uint64_t gas_uncompacted_bytes(ar_key key) const; // outputSizeInBytes the build was sized with

  // One entry of the device instance array, read back (tests).
  struct TlasInstance {
    uint32_t instance_id, mask, flags, sbt_offset;
    float transform[12];
    uint64_t handle;
  };
  std::vector<TlasInstance> read_tlas_instances() const;
  uint64_t gas_bytes_total() const;
  uint64_t tlas_bytes() const { return tlas_ ? tlas_->buffer.bytes : 0; }
  uint32_t gas_count() const { return uint32_t(gas_.size()); }

  // Counters for tests and the statistics report.
  uint64_t gas_builds() const { return gas_builds_; }
  uint64_t tlas_builds() const { return tlas_builds_; }
  double last_gas_ms() const { return last_gas_ms_; }  // wall time of the last update_gas that built

 private:
  struct Entry {
    uint64_t version = 0;
    std::shared_ptr<OptixGas> gas;
  };
  OptixAccelBuilder core_;
  OptixContext& context_;
  std::map<ar_key, Entry> gas_;
  std::shared_ptr<OptixGas> lamp_gas_[4];  // by ar::LampShape; NONE and INVALID stay empty
  std::shared_ptr<OptixTlas> tlas_;
  bool tlas_stale_ = false;
  uint64_t gas_builds_ = 0, tlas_builds_ = 0;
  double last_gas_ms_ = 0;
};

}  // namespace astroray::host

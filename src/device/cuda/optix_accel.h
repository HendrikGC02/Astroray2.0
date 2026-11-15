// SPDX-License-Identifier: Apache-2.0
// Acceleration structures: one compacted triangle GAS per geometry key, built in batches
// from one temporary arena, and one TLAS per generation, always rebuilt and never refit.
// GAS persist across edits and are rebuilt only for keys whose uploaded data changed.
// Every buffer, the temporary arena included, is a ledger record of category ACCELERATION
// (instance arrays are TABLES) made through the OptixContext's device.
// Ref: OptiX 9.1 Programming Guide, acceleration structure build and compaction;
// docs/decisions/0007-blender-integration.md d11, d14; 0002 D11.
#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include "device/cuda/optix_context.h"
#include "kernel/light/light_record.h"
#include "device/cuda/scene_upload.h"

namespace astroray::host {

// Hit-group record of an instance (the instance's sbtOffset), shared with the pipeline's SBT: the
// mesh and rectangle-lamp triangles, the disk-lamp custom primitive, the lamp built-in sphere.
inline constexpr uint32_t kSbtTriangle = 0, kSbtLampDisk = 1, kSbtLampSphere = 2, kSbtHitGroups = 3;

class OptixAccel {
 public:
  OptixAccel(OptixContext& context, Owner owner);
  ~OptixAccel();
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
  // last build_tlas (the old TLAS may point at freed memory, so nothing may trace it).
  uint64_t tlas_handle() const { return tlas_handle_; }
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
  uint64_t tlas_bytes() const { return tlas_buffer_.bytes; }
  uint32_t gas_count() const { return uint32_t(gas_.size()); }

  // Counters for tests and the statistics report.
  uint64_t gas_builds() const { return gas_builds_; }
  uint64_t tlas_builds() const { return tlas_builds_; }
  double last_gas_ms() const { return last_gas_ms_; }  // wall time of the last update_gas that built

 private:
  struct Gas {
    uint64_t version = 0, handle = 0, emitted = 0, uncompacted = 0;
    DeviceBuffer buffer;
  };
  void free_gas(Gas& g);
  void ensure_lamp_gas(uint8_t shape);  // an ar::LampShape

  OptixContext& context_;
  Owner owner_;
  std::map<ar_key, Gas> gas_;
  Gas lamp_gas_[4];  // by ar::LampShape (kernel/geom/lamp_intersect.h); NONE and INVALID stay empty
  DeviceBuffer tlas_buffer_, tlas_instances_;
  uint32_t tlas_instance_count_ = 0;
  uint64_t tlas_handle_ = 0;
  uint64_t gas_builds_ = 0, tlas_builds_ = 0;
  double last_gas_ms_ = 0;
};

}  // namespace astroray::host

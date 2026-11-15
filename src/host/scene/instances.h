// SPDX-License-Identifier: Apache-2.0
// Host instance records: placement, motion steps, the 8-bit visibility mask, flags, light
// group and material table slice (0002 D11, 0007 decisions 12, 15, 18). The table is built
// and validated on a worker and published whole when its generation becomes active; the
// device tasks (037, 040) rebuild the TLAS when the scene's tlas epoch moves. GAS are
// never touched here.
// Ref: 0002 D11 (visibility mask table), 0007 d12 (edit map), d15 (motion steps), d18 (keys).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "../../../include/astroray/astroray.h"
#include "../../kernel/scene/scene_view.h"

namespace astroray::host {

class Edit;
struct Pending;

// What a later edit may validate against before the table is built: per instance the
// geometry and object keys, and the motion step count. Built synchronously from the desc.
struct InstanceSummary {
  uint32_t steps = 1;
  std::vector<ar_key> geom, object;
};

struct Instance {
  ar_key geom_key = 0;
  ar_key object_key = 0;
  uint32_t random_id = 0, pass_index = 0, crypto_object = 0, crypto_asset = 0;
  float color[4] = {1, 1, 1, 1};
  uint32_t visibility = 0;  // AR_VISIBILITY_* as given
  uint8_t mask = 0;         // the instance visibility mask (0002 D11)
  uint32_t flags = 0;       // AR_INSTANCE_*: shading and light pass only, never mask bits
  uint8_t light_group = 0;
  uint32_t material_offset = 0, material_count = 0;  // slice of material_pool; 0 = default
  uint32_t light_index = ar::kNoLightIndex;  // set by the lamp-instance task (068)
};

// The motion step rendered until M8 for S steps: (S - 1) / 2 in integer division, so the
// centre for odd S and the earlier of the two middle steps for even S. Instances and meshes
// both use it, so a hit and its geometry always come from the same step.
// Ref: RULINGS.md ruling 68; 0007 d15.
constexpr uint32_t representative_step(uint32_t steps) { return (steps - 1) / 2; }

struct InstanceTable {
  uint32_t motion_steps = 1;
  std::vector<Instance> instances;
  std::vector<ar_xform> xf;  // instances.size() * motion_steps, instance-major
  std::vector<ar_key> material_pool;
  const ar_xform& step(uint32_t i, uint32_t s) const { return xf[size_t(i) * motion_steps + s]; }
  // The motion-blur step used until M8 (S > 1 is stored, the representative step is rendered).
  const ar_xform& centre(uint32_t i) const { return step(i, representative_step(motion_steps)); }
};

// Visibility bits to mask bits: camera 0, diffuse 1, glossy 2, transmission 3, shadow 4,
// volume 5. Bit 6 (medium boundary) stays unset until media exist; bit 7 is never set.
uint8_t visibility_mask(uint32_t visibility);

// The two edit entry points (the C API forwards here). set_instances copies the arrays,
// then validates and builds on a worker, so a bad instance rejects the generation
// (GENERATION_REJECTED, detail names the instance index and key). update_transforms patches
// the listed instances of the table that is active when the generation activates; its
// indices are checked at the call against this edit's table or else the active one.
ar_status set_instances(Edit& edit, const ar_instances_desc* desc);
ar_status update_transforms(Edit& edit, uint32_t n, const uint32_t* idx, const ar_xform* xf);
// Applies one generation's effect to a latest-committed or active summary.
void apply_instance_summary(std::shared_ptr<const InstanceSummary>& cur, const Pending& p);
// Called by Edit::commit: an edit that removes geometry without a new set_instances gets a
// job that rejects the generation when the active table still references a removed key.
void queue_instance_check(Edit& edit);

}  // namespace astroray::host

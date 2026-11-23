// SPDX-License-Identifier: Apache-2.0
// The environment importance table's source (0004 D11): runs the bake_environment kernel on the
// device that owns the world, reads the n x n grid back, normalises it by its maximum and builds
// the EnvTable (host/light/env_table.h). The result is cached under a key of the world, so the
// bake runs on the first request after a world edit and not for an edit that leaves the world
// alone (camera, meshes, materials). An emitter edit may move the light table's radiance scale s
// (0003 d10), which the world record's strength carries; the table, a density, does not depend on
// s and max and mean are returned in unscaled units (below), so that edit does not rebuild either.
// Lazy: nothing bakes until a consumer asks. The upload of the table and the importance energy are
// the consumer's (173).
// Ref: docs/decisions/0004-integrator.md D11 bullet 1, D10 "Importance weighting";
// kernel/stages/bake_environment.h.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "host/device.h"
#include "host/light/env_table.h"

namespace astroray::host {

struct WorldOutput;

// 0004 D11: 1024 x 1024 equal-area cells.
inline constexpr uint32_t kEnvBakeSize = 1024;

struct BakedEnvironment {
  // Built from the grid divided by `max`, so the table is a density and its values lie in [0, 1];
  // null for a black world, which has no importance and never reaches build_env_table.
  std::shared_ptr<const EnvTable> table;
  // Maximum and mean of the un-normalised grid divided by the radiance scale s the kernel saw in
  // the world record (0003 d10), i.e. in the unscaled display-weighted units of display_energy of
  // the world as authored; s = 2^k, so the division is exact. Equal-area cells make the mean the
  // solid-angle mean, so 4 pi mean is the world's importance energy (173). 0 and 0 for a black
  // world.
  double max = 0.0;
  double mean = 0.0;
};

class EnvironmentBaker {
 public:
  explicit EnvironmentBaker(Owner owner = {}) : owner_(owner) {}

  // The part of SceneLights::key that the grid depends on: the world's colour, strength and light
  // group (the same bytes gather_scene_lights puts first). Empty without a world.
  static std::vector<uint8_t> world_key(const WorldOutput* world);

  // The baked environment for `key` at n x n cells. A key and n equal to the last successful
  // call's return the cached result with no launch, whatever `radiance_scale` is now. Otherwise it
  // writes the grid pointer into the
  // device-resident KernelGlobals in `kg_buffer` (the session's, with the world light and the
  // observer tables filled), binds that buffer, launches KernelId::BAKE_ENVIRONMENT over n * n
  // cells, reads the grid back and frees it, and clears the grid pointer again, also when the launch
  // fails. There is no default for `radiance_scale`: the caller passes the radiance_scale of the same
  // latched light table whose world record is bound in `kg_buffer` (1 when that record is unscaled).
  // Throws std::invalid_argument for n = 0 or a radiance_scale that is not finite and positive and
  // std::runtime_error, naming the first cell and its
  // direction, for a cell that is not a finite non-negative radiance; the cache is unchanged then.
  // Not thread-safe: one thread per baker, like every handle.
  BakedEnvironment get(Device& device, const DeviceBuffer& kg_buffer, const std::vector<uint8_t>& key,
                       double radiance_scale, uint32_t n = kEnvBakeSize);

  // Bakes completed so far.
  uint32_t rebuild_count() const { return rebuilds_; }

 private:
  Owner owner_;
  bool valid_ = false;
  std::vector<uint8_t> key_;
  uint32_t n_ = 0;
  BakedEnvironment result_;
  uint32_t rebuilds_ = 0;
};

}  // namespace astroray::host

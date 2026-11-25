// SPDX-License-Identifier: Apache-2.0
// The environment importance table's source (0004 D11): runs the bake_environment kernel over the
// scene's world program, reads the n x n grid back, normalises it by its maximum and builds the
// EnvTable (host/light/env_table.h). The result is cached under a key of the world program, so the
// bake runs on the first request after a world edit and not for an edit that leaves the world
// alone (camera, meshes, materials, emitters). The program runs unscaled (no radiance scale s,
// 0003 d10), so max and mean are in the authored world's units and a change of s moves nothing.
// Lazy: nothing bakes until a consumer asks. The scene table build is the consumer: it bakes before
// it builds the light table and uploads the table as ENV_TABLE (host/scene/scene_tables.h).
// Ref: docs/decisions/0004-integrator.md D11 bullet 1, D10 "Importance weighting";
// kernel/stages/bake_environment.h.
#pragma once
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "host/device.h"
#include "host/light/env_table.h"

namespace astroray::host {

struct ImageArrays;
struct WorldOutput;

// A grid the baker refuses (rule 5): a cell that is not a finite non-negative radiance (a world
// program that overflows or cannot run), or a grid build_env_table rejects. A device failure is not
// this type: it keeps its own (DeviceError, std::bad_alloc), so a caller can retry it.
class BakeRefused : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// 0004 D11: 1024 x 1024 equal-area cells.
inline constexpr uint32_t kEnvBakeSize = 1024;

struct BakedEnvironment {
  // Built from the grid divided by `max`, so the table is a density and its values lie in [0, 1];
  // null for a black world, which has no importance and never reaches build_env_table.
  std::shared_ptr<const EnvTable> table;
  // Maximum and mean of the un-normalised grid, in the unscaled display-weighted units of
  // display_energy of the world as authored (the program runs without s). Equal-area cells make the
  // mean the solid-angle mean, so 4 pi mean is the world's importance energy (173). 0 and 0 for a
  // black world.
  double max = 0.0;
  double mean = 0.0;
};

class EnvironmentBaker {
 public:
  explicit EnvironmentBaker(Owner owner = {}) : owner_(owner) {}

  // What the grid depends on, empty when there is nothing to bake (no world, or a constant, black or
  // magenta world without a program): the world program's words, its constant block, and for every
  // image key the program samples (WorldOutput::images) the key, its slot in `images` and the
  // address of the Image there. An image edit makes a new Image, so it changes the key even when
  // the recompiled program is word for word the same; the caller keeps `images` alive while it
  // holds the key, so an address is never reused under it.
  static std::vector<uint8_t> world_key(const WorldOutput* world, const ImageArrays* images);

  // The baked environment for `key` at n x n cells. A key and n equal to the last successful
  // call's return the cached result with no launch. Otherwise it writes the grid pointer into the
  // device-resident KernelGlobals in `kg_buffer` (its scene and tables naming the world program,
  // its constant block and spectra, the observer, images, rgb2spec and sRGB tables), binds that
  // buffer, launches KernelId::BAKE_ENVIRONMENT over n * n cells, reads the grid back and frees it,
  // and clears the grid pointer again, also when the launch fails.
  // Throws std::invalid_argument for n = 0 or over 2^32 cells; BakeRefused, naming the first cell
  // and its direction, for a cell that is not a finite non-negative radiance (or naming
  // build_env_table's reason); and the device's own errors for a failed allocation, launch or copy.
  // The cache is unchanged then. Not thread-safe: one thread per baker, like every handle.
  BakedEnvironment get(Device& device, const DeviceBuffer& kg_buffer, const std::vector<uint8_t>& key,
                       uint32_t n = kEnvBakeSize);

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

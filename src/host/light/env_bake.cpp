// SPDX-License-Identifier: Apache-2.0
#include "host/light/env_bake.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

#include "../../kernel/kernel_globals.h"
#include "../../kernel/stages/bake_environment.h"
#include "host/scene/world.h"

namespace astroray::host {

namespace {

template <class T>
void put(std::vector<uint8_t>& k, const T& v) {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
  k.insert(k.end(), b, b + sizeof(T));
}

// rule 5: the first cell that is not a finite non-negative radiance, with its centre direction.
void refuse_bad_cells(const std::vector<float>& grid, uint32_t n) {
  for (size_t i = 0; i < grid.size(); ++i) {
    const float v = grid[i];
    if (std::isfinite(v) && v >= 0.0f) continue;
    const uint32_t iv = uint32_t(i / n), iu = uint32_t(i % n);
    const ar::Real3 d = ar::bake_environment_direction(n, uint32_t(i));
    char text[256];
    std::snprintf(text, sizeof text,
                  "bake_environment: cell %zu (iu %u, iv %u) at direction (%.6f, %.6f, %.6f) is %g, not a finite "
                  "non-negative radiance",
                  i, iu, iv, double(d.x), double(d.y), double(d.z), double(v));
    throw std::runtime_error(text);
  }
}

}  // namespace

std::vector<uint8_t> EnvironmentBaker::world_key(const WorldOutput* world) {
  std::vector<uint8_t> k;
  if (world) put(k, world->color), put(k, world->strength), put(k, world->desc.light_group);
  return k;
}

BakedEnvironment EnvironmentBaker::get(Device& device, const DeviceBuffer& kg_buffer, const std::vector<uint8_t>& key,
                                       double radiance_scale, uint32_t n) {
  if (n == 0) throw std::invalid_argument("bake_environment: n is 0");
  if (!(std::isfinite(radiance_scale) && radiance_scale > 0.0))
    throw std::invalid_argument("bake_environment: the radiance scale is not finite and positive");
  if (uint64_t(n) * n > std::numeric_limits<uint32_t>::max())
    throw std::invalid_argument("bake_environment: n = " + std::to_string(n) + " has more than 2^32 cells");
  if (valid_ && n == n_ && key == key_) return result_;

  const uint64_t cells = uint64_t(n) * n;
  DeviceBuffer grid_buf = device.allocate(LedgerCategory::TABLES, cells * sizeof(float), owner_);
  const uint64_t offset = offsetof(ar::KernelGlobals, env_bake);
  std::vector<float> grid(cells);
  // The pointer would dangle once the grid is freed; the next launch writes its own.
  const ar::EnvBake none{nullptr, 0};
  try {
    const ar::EnvBake bake{static_cast<float*>(grid_buf.ptr), n};
    device.upload(kg_buffer, offset, &bake, sizeof bake);
    device.set_kernel_globals(kg_buffer);
    device.launch(KernelId::BAKE_ENVIRONMENT, uint32_t(cells));
    device.synchronize();
    device.download(grid.data(), grid_buf, 0, cells * sizeof(float));
  } catch (...) {
    device.free(grid_buf);
    try {
      device.upload(kg_buffer, offset, &none, sizeof none);
    } catch (...) {  // the original error is the one to report
    }
    throw;
  }
  device.free(grid_buf);
  device.upload(kg_buffer, offset, &none, sizeof none);

  refuse_bad_cells(grid, n);

  BakedEnvironment out;
  double sum = 0.0, peak = 0.0;
  for (float v : grid) peak = std::max(peak, double(v)), sum += double(v);
  if (peak > 0.0) {
    // A density: dividing by the maximum leaves the table's shape and keeps a uniform world's
    // integral and compensated residuals clear of FLT_MIN, which a world below it would not.
    std::vector<double> normalised(cells);
    for (size_t i = 0; i < normalised.size(); ++i) normalised[i] = double(grid[i]) / peak;
    out.table = std::make_shared<const EnvTable>(build_env_table(normalised, n));
    // The grid carries the light table's radiance scale (a power of 2, so this is exact); the
    // table does not, which is why a change of s alone needs no new bake.
    out.max = peak / radiance_scale;
    out.mean = sum / double(cells) / radiance_scale;
  }
  valid_ = true;
  key_ = key;
  n_ = n;
  result_ = out;
  ++rebuilds_;
  return out;
}

}  // namespace astroray::host

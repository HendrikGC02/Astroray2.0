// SPDX-License-Identifier: Apache-2.0
// FilmLayout: which channels exist, their kinds and plane offsets, built once per render from
// the requested outputs; Film: the one device allocation that holds them and the per-sample
// buffer, sized at the session's largest resolution.
// Ref: docs/decisions/0008-film-outputs-denoise.md d1 (layout, offset -1 for an unrequested
// channel, allocation at the largest size), d2 (kinds), d3 (statistics of the moments), d21
// (film bytes go to the ledger's film category).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "host/device.h"
#include "kernel/film/film_write.h"

namespace astroray::host {

enum class ChannelKind : uint32_t { Moment = 0, Add, Count, Crypto, Splat };

struct ChannelRequest {
  std::string name;
  ChannelKind kind = ChannelKind::Moment;
  uint32_t components = 1;
  // Moment only: the response channel it integrates (ar::ResponseTableView index), or
  // ar::kFilmDisplayResponse for the display group.
  uint32_t response = 0;
  // ADD only: the plane holds sample 0 only (a primary-hit id or position), so readback must not
  // divide by n.
  bool sample0 = false;
};

struct FilmRequest {
  uint32_t max_width = 0;
  uint32_t max_height = 0;
  // Samples one pass can hold; 0 means one per pixel at the allocated size (0008 d3).
  uint32_t slot_capacity = 0;
  bool adaptive_sampling = false;  // adds the class-A sum S1A (off in M1a)
  bool light_pass = false;         // adds the SPLAT plane (record 0004's light-tracing pass)
  // Statistics (0008 d12): "bounces" (ADD), "bounce_max" and "bounce_paths" (COUNT). On by default (d12).
  bool statistics = true;
  bool termination_stats = false;  // "term": one COUNT plane per ar::Term value, on request
  bool primary_aov = false;        // "aov_position", "aov_normal", "aov_uv", "aov_material" (d10)
  std::vector<ChannelRequest> extra;  // beyond the always-present display MOMENT channel
};

struct FilmChannel {
  std::string name;
  ChannelKind kind;
  uint32_t components;
  int32_t offset;  // first plane within its kind's planes
  uint32_t response;
  bool sample0 = false;
};

class FilmLayout {
 public:
  // Throws std::invalid_argument for a zero size, a duplicate name, wrong components for a
  // Moment or Splat channel, more Moment channels than the kernel table holds, and for Crypto,
  // which has no producer yet.
  static FilmLayout build(const FilmRequest& request);

  const std::vector<FilmChannel>& channels() const { return channels_; }
  // The channel, or nullptr when it was not requested.
  const FilmChannel* find(const std::string& name) const;
  // First plane of the channel, -1 when it was not requested (no memory exists for it).
  int32_t offset(const std::string& name) const;

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t slot_capacity() const { return slot_capacity_; }
  bool adaptive() const { return adaptive_; }
  uint32_t moment_planes() const { return moment_planes_; }
  uint32_t add_planes() const { return add_planes_; }
  uint32_t count_planes() const { return count_planes_; }
  uint32_t splat_components() const { return splat_components_; }

  // Bytes of the one allocation; every region starts on a 256-byte boundary.
  uint64_t bytes() const;
  // The kernel description with every pointer set inside a block of bytes() bytes at `base`
  // (a device address on the GPU). Views, exposure and scale are the caller's to fill.
  ar::FilmData bind(void* base) const;

 private:
  std::vector<FilmChannel> channels_;
  uint32_t width_ = 0, height_ = 0, slot_capacity_ = 0;
  bool adaptive_ = false;
  uint32_t moment_planes_ = 0, add_planes_ = 0, count_planes_ = 0, splat_components_ = 0;
};

class Film {
 public:
  // Allocates under LedgerCategory::FILM and clears it. Throws what Device::allocate throws.
  Film(Device& device, const FilmLayout& layout, Owner owner);
  ~Film();
  Film(const Film&) = delete;
  Film& operator=(const Film&) = delete;

  const FilmLayout& layout() const { return layout_; }
  const DeviceBuffer& buffer() const { return buffer_; }
  // What the kernels read. The caller sets response, wavelengths, film_exposure and
  // inv_radiance_scale, and uploads the struct where the kernel reads it.
  ar::FilmData& data() { return data_; }
  const ar::FilmData& data() const { return data_; }

  // The sub-rectangle in use. Throws std::invalid_argument above the allocated size or at zero.
  void set_render_size(uint32_t width, uint32_t height);
  // The scheduler calls this before every resolve launch: the pass's samples are
  // rect_width * rect_height * k slots and must fit the slot buffer (0008 d3). Throws
  // std::length_error naming the three numbers.
  void check_resolve_fits(uint32_t k) const;
  // A clear, not an allocation (epoch reset).
  void clear();

 private:
  Device& device_;
  FilmLayout layout_;
  DeviceBuffer buffer_;
  ar::FilmData data_;
};

// Mean = S1 / n. n = 0 gives NaN.
double moment_mean(double s1, uint32_t n);
// Variance of the mean (S2 - S1^2 / n) / (n (n - 1)) in double; NaN for n < 2.
// Ref: Chan, Golub and LeVeque 1983; 0008 d3.
double moment_variance_of_mean(double s1, double s2, uint32_t n);

}  // namespace astroray::host

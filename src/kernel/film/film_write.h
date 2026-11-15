// SPDX-License-Identifier: Apache-2.0
// The film's device-resident description and its only writers. Camera contributions land in a
// per-sample fp32 slot (sample_add); the resolve (resolve.h) folds slots into fp64 moments.
// ADD, COUNT and SPLAT planes are written here by atomics; nothing else writes film memory.
// Ref: docs/decisions/0008-film-outputs-denoise.md d1 (one write path, planes SoA, rows
// bottom-up, allocation at the largest size), d2 (channel kinds), d3 (slot, sample_add,
// flags at path end), d4 (SPLAT plane); 0002 D2 (fp32 atomic add, u32 add and max).
// Includes are relative so the host layout can include this file without src/kernel on its
// include path.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../core/vec.h"
#include "../kernel_globals.h"
#include "../portability.h"
#include "../spectral/wavelength.h"
#include "response.h"

AR_NS_BEGIN

// A MOMENT channel's source: the display group (3 components) or one response channel.
inline constexpr uint32_t kFilmDisplayResponse = 0xFFFFFFFFu;
inline constexpr uint32_t kFilmMaxMomentChannels = 8;

struct FilmMomentChannel {
  uint32_t offset;      // first plane of the channel in the moment arrays
  uint32_t components;  // 3 for display, 1 for a response channel
  uint32_t response;    // kFilmDisplayResponse or a ResponseTableView channel index
  uint32_t reserved;
};

// Every plane of one kind is contiguous, plane i at base + i * plane_stride, pixel (x, y) at
// y * width + x with y = 0 the bottom row. Pointers are null when the plane kind is not
// requested (offset -1 and no memory in the layout); reading one is a bug.
struct FilmData {
  uint32_t width, height;            // allocated size
  uint32_t rect_width, rect_height;  // sub-rectangle in use
  uint32_t plane_stride;             // width * height
  uint32_t slot_capacity;            // samples one pass can hold
  uint32_t moment_channel_count;
  uint32_t splat_components;         // 0: no SPLAT plane
  FilmMomentChannel moment[kFilmMaxMomentChannels];

  double* s1;   // moment planes: S1, S2 and the class-A sum (null unless adaptive sampling)
  double* s2;
  double* s1a;
  uint32_t* n;        // samples per pixel
  uint32_t* invalid;  // invalid samples seen per pixel (0008 d6)
  float* add;         // ADD planes
  uint32_t* count;    // COUNT planes
  float* splat;       // SPLAT planes, same component order as the display channel
  float* slot_L;      // slot_capacity * 4 lanes
  uint32_t* slot_flags;

  ResponseTableView response;
  WavelengthTableView wavelengths;
  double film_exposure;
  double inv_radiance_scale;  // 1 / s of 0003 d10
};

// Slot lanes: fp32 atomic add, since several contributions to one sample can be live at once
// (shadow deliveries arrive after the path ended).
AR_FN void sample_add(const KernelGlobals& kg, uint32_t slot, const Spec& L) {
  const FilmData& f = *kg.film;
  AR_ASSERT(slot < f.slot_capacity);
  for (int i = 0; i < kLanes; ++i) atomic_add(f.slot_L + 4u * slot + uint32_t(i), float(L.v[i]));
}

// Path end: the final flags (SECONDARIES_TERMINATED, ...) of the sample. Whether a sample is
// invalid is decided from its values by the resolve (0008 d6), not by a flag. One writer per slot.
AR_FN void sample_set_flags(const KernelGlobals& kg, uint32_t slot, uint32_t flags) {
  const FilmData& f = *kg.film;
  AR_ASSERT(slot < f.slot_capacity);
  f.slot_flags[slot] = flags;
}

AR_FN void film_add(const KernelGlobals& kg, uint32_t plane, uint32_t pixel, Real v) {
  const FilmData& f = *kg.film;
  AR_ASSERT(f.add != nullptr && pixel < f.plane_stride);
  atomic_add(f.add + uint64_t(plane) * f.plane_stride + pixel, float(v));
}

AR_FN void film_count_add(const KernelGlobals& kg, uint32_t plane, uint32_t pixel, uint32_t v) {
  const FilmData& f = *kg.film;
  AR_ASSERT(f.count != nullptr && pixel < f.plane_stride);
  atomic_add(f.count + uint64_t(plane) * f.plane_stride + pixel, v);
}

AR_FN void film_count_max(const KernelGlobals& kg, uint32_t plane, uint32_t pixel, uint32_t v) {
  const FilmData& f = *kg.film;
  AR_ASSERT(f.count != nullptr && pixel < f.plane_stride);
  atomic_max(f.count + uint64_t(plane) * f.plane_stride + pixel, v);
}

// One splat tap on the pass's SPLAT plane. The producer has already applied the footprint
// weight (filter.h splat_tap_weight) and converted at the light path's film wavelengths.
AR_FN void film_splat_add(const KernelGlobals& kg, uint32_t pixel, const Real3& value) {
  const FilmData& f = *kg.film;
  AR_ASSERT(f.splat != nullptr && f.splat_components == 3u && pixel < f.plane_stride);
  atomic_add(f.splat + pixel, float(value.x));
  atomic_add(f.splat + f.plane_stride + pixel, float(value.y));
  atomic_add(f.splat + 2ull * f.plane_stride + pixel, float(value.z));
}

AR_NS_END

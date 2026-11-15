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
#include "../sample/keys.h"
#include "../state/flags.h"
#include "../state/termination.h"
#include "../spectral/wavelength.h"
#include "response.h"

AR_NS_BEGIN

// A MOMENT channel's source: the display group (3 components) or one response channel.
inline constexpr uint32_t kFilmDisplayResponse = 0xFFFFFFFFu;
inline constexpr uint32_t kFilmMaxMomentChannels = 8;

// One COUNT plane per Term value for the termination histogram. Term only grows by appending
// (termination.h), so the assert fires when a reason is added and this must follow.
inline constexpr uint32_t kTermCount = 16;
static_assert(uint32_t(Term::CANCELLED) + 1 == kTermCount, "Term changed: update kTermCount");

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

  // Statistics and primary-hit AOV channels (0008 d10, d12): first plane within the ADD or COUNT
  // plane kind, -1 when not requested.
  int32_t bounce_sum, bounce_max;  // ADD, COUNT
  int32_t bounce_paths;            // COUNT: paths ended, the denominator of the bounce mean
  int32_t term_hist;               // COUNT x kTermCount (Term values), on request
  int32_t aov_position, aov_normal, aov_uv, aov_material;  // ADD x 3, 3, 2, 1

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

// Marks the sample invalid: its slot holds NaN, so the resolve's film_sample_valid rejects it, counts
// it in the invalid counter and leaves n alone, whatever was added to the slot before (a later
// delivery adds to NaN and stays NaN). Called by the stage that found the non-finite value.
// Ref: 0008 d6 (invalid samples are counted, never averaged); 0004 D17.
AR_FN void sample_mark_invalid(const KernelGlobals& kg, uint32_t slot) {
  const FilmData& f = *kg.film;
  AR_ASSERT(slot < f.slot_capacity);
  const float nan = bit_cast<float>(0x7fc00000u);
  for (int i = 0; i < kLanes; ++i) f.slot_L[4u * slot + uint32_t(i)] = nan;
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

// Active pixel j of the render sub-rectangle as a plane index (0008 d3): the row-order formula
// that film_build_active (M1a) writes into the list. Stages never call it: they read the list,
// so compaction (M4) changes one kernel.
AR_FN uint32_t film_active_pixel(const FilmData& f, uint32_t j) {
  return pixel_id(j % f.rect_width, j / f.rect_width, f.width);
}

// The pixel plane index of a slot: slot = j * k + m, read from the pass's active-pixel list.
AR_FN uint32_t film_slot_pixel(const KernelGlobals& kg, uint32_t slot) {
  return kg.active_pixels[slot / kg.session.samples_per_pass];
}

// Path end: one path of the slot's pixel ended. bounces_total counts scattering events (surface
// or real volume scatter, delta included, null collisions excluded); a tail pass can end several
// paths of one pixel in one launch, hence atomics. Mean = ADD / paths ended at readback (a path
// whose sample the resolve rejects still counts here), max is COUNT max.
// Ref: 0008 d12 (statistics table); 0004 D28 (path-end statistics); 0003 d22 (Term).
AR_FN void film_path_end_stats(const KernelGlobals& kg, uint32_t slot, uint32_t bounces_total, Term term) {
  const FilmData& f = *kg.film;
  const uint32_t pixel = film_slot_pixel(kg, slot);
  if (f.bounce_sum >= 0) film_add(kg, uint32_t(f.bounce_sum), pixel, Real(bounces_total));
  if (f.bounce_paths >= 0) film_count_add(kg, uint32_t(f.bounce_paths), pixel, 1u);
  if (f.bounce_max >= 0) film_count_max(kg, uint32_t(f.bounce_max), pixel, bounces_total);
  AR_ASSERT(uint32_t(term) < kTermCount);
  if (f.term_hist >= 0) film_count_add(kg, uint32_t(f.term_hist) + uint32_t(term), pixel, 1u);
}

// Mean scattering events per path of a pixel: ADD plane / paths ended (not n: an invalid sample is
// excluded from n but its path still ended). False when no path ended or the statistics were not
// requested; *mean is untouched then. The one source for the host readback and the heat map.
// Ref: 0008 d12 (statistics table).
AR_FN bool film_bounce_mean_value(const FilmData& f, uint32_t pixel, Real* mean) {
  if (f.bounce_sum < 0 || f.bounce_paths < 0) return false;
  const uint32_t paths = f.count[uint64_t(f.bounce_paths) * f.plane_stride + pixel];
  if (paths == 0u) return false;
  *mean = Real(f.add[uint64_t(f.bounce_sum) * f.plane_stride + pixel]) / Real(paths);
  return true;
}

// Primary-hit AOVs, written by the surface stage at depth 0, once across pass-throughs
// (AOV_WRITTEN in the path's flags; 0003 d21; 0008 d10). Position and material id keep only
// sample 0 so an id stays exact (these channels are marked sample0 in the layout: readback must
// not divide by n); normal and UV accumulate over samples (readback divides by n).
// Ref: Cycles intern/cycles/kernel/film/data_passes.h, film_write_data_passes (sample == 0
// overwrite vs accumulate).
AR_FN void film_primary_aov(const KernelGlobals& kg, uint32_t slot, uint32_t sample_index, uint32_t* path_flags,
                            const Real3& position, const Real3& normal, Real u, Real v, uint32_t material_id) {
  if ((*path_flags & AOV_WRITTEN) != 0u) return;
  *path_flags |= AOV_WRITTEN;
  const FilmData& f = *kg.film;
  const uint32_t pixel = film_slot_pixel(kg, slot);
  if (f.aov_position >= 0 && sample_index == 0u) {
    film_add(kg, uint32_t(f.aov_position), pixel, position.x);
    film_add(kg, uint32_t(f.aov_position) + 1u, pixel, position.y);
    film_add(kg, uint32_t(f.aov_position) + 2u, pixel, position.z);
  }
  if (f.aov_material >= 0 && sample_index == 0u) film_add(kg, uint32_t(f.aov_material), pixel, Real(material_id));
  if (f.aov_normal >= 0) {
    film_add(kg, uint32_t(f.aov_normal), pixel, normal.x);
    film_add(kg, uint32_t(f.aov_normal) + 1u, pixel, normal.y);
    film_add(kg, uint32_t(f.aov_normal) + 2u, pixel, normal.z);
  }
  if (f.aov_uv >= 0) {
    film_add(kg, uint32_t(f.aov_uv), pixel, u);
    film_add(kg, uint32_t(f.aov_uv) + 1u, pixel, v);
  }
}

AR_NS_END

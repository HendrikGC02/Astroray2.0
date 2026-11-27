// SPDX-License-Identifier: Apache-2.0
// What a session's readback functions return, as pure functions of a film copy: the pass list,
// pass values (mean, variance of the mean, count), the counter words and the merged report. The
// session supplies the film under its render lock; nothing here touches a session.
// Ref: docs/decisions/0008-film-outputs-denoise.md d13a (what readback returns), d10 (pass
// names), d12 (statistics passes are raw values); 0004 D29 (per-pass counters);
// 0001 D2.11 (report snapshot order).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../../../include/astroray/astroray.h"
#include "host/counters.h"
#include "host/device.h"
#include "host/film/display_image.h"
#include "host/film/film_layout.h"
#include "host/film/film_view.h"
#include "host/report.h"

namespace astroray::host {

// Denoised is Combined of a session with a denoiser: the DisplayImage once a denoise succeeded, the
// film until then. NoisyImage is always the film's display mean (0008 d17); the guides are the
// denoiser guide planes' means, unclamped (the clamp belongs to the denoiser's copy, d15). Depth is the
// stored view-space z with a stored 0 (a ray that never hit) read as 1e10, and Alpha is
// saturate(1 - mean transparency), both as Cycles' film/read.h converts them; Alpha is not denoised
// (d18: it is copied from the film).
enum class PassSource : uint8_t {
  Combined, Aov, SampleCount, Bounces, RelError, Invalid, Denoised, NoisyImage, GuideAlbedo, GuideNormal, Depth, Alpha
};

struct PassDesc {
  std::string name;
  uint32_t components = 1;
  uint32_t unit = AR_UNIT_NONE;
  PassSource source = PassSource::Combined;
  std::string channel;  // the film channel behind Combined or an AOV
};

// The passes the layout allocated, in 0008 d10's order: Combined, the primary-hit AOVs
// (Depth, Position, Normal, UV, IndexMA, Alpha), with `denoiser` the denoising passes (Denoising Albedo,
// Denoising Normal, Noisy Image), the statistics (AR Sample Count, AR Bounces, AR Rel Error,
// AR Invalid). The denoising passes exist exactly when a denoiser was selected (d10), and Combined's
// source is then Denoised. Throws std::invalid_argument for a band channel (the layout does not carry
// the response unit, so a band pass would have to guess ENERGY or PHOTON) and for `denoiser` with a
// layout that lacks the guide planes (the session forces them on with a denoiser).
std::vector<PassDesc> list_passes(const FilmLayout& layout, bool denoiser = false);

// The floats read_pass writes: pixels * components for MEAN and VARIANCE, pixels for COUNT.
// Throws std::invalid_argument for a quantity the pass cannot give: VARIANCE only for Combined and
// Noisy Image (the display MOMENT channel; read_pass refuses it for a denoised Combined), COUNT only
// for Combined and Noisy Image (n per pixel), AR Sample Count and AR Invalid (the raw counter); AR
// Sample Count and AR Invalid also give MEAN, the same value.
uint64_t pass_float_count(const PassDesc& pass, uint32_t quantity, uint32_t width, uint32_t height);

// Writes interleaved (pixel-major) floats for the whole render rectangle. `n` must equal
// pass_float_count exactly: a wrong size fails, it never truncates. NaN marks "no data" where
// the film has none (n = 0, or n < 2 for a variance); AR Rel Error is -1 below two samples.
// `denoised` is the session's DisplayImage when a denoise succeeded (null otherwise): a Denoised
// Combined's MEAN is then its RGB and its VARIANCE is refused (std::invalid_argument naming Noisy
// Image, which holds the film's); COUNT stays the film's n. Without it a Denoised Combined is the film's.
void read_pass(const FilmView& film, const PassDesc& pass, uint32_t quantity, uint32_t n, float* dst,
               const DisplayImage* denoised = nullptr);

// PassCounters flattened in format_counters' order: rays_closest, rays_shadow, steps, launches,
// items per stage, active_path_steps, peak_active_paths, refills, camera_paths, light_paths,
// sorts, state_bytes_moved, shadow_item_bytes_moved, lamp_crossings, shadow_rays_emitted,
// permuted_launches.
std::vector<uint64_t> counter_words(const PassCounters& c);

// One snapshot of a Report in its own order.
struct ReportTable {
  ar_report_info info{};
  std::vector<ar_report_entry> entries;
};

// Snapshots `report` and copies it out. Retries when the table changes under a concurrent
// snapshot by another session of the same scene; throws std::runtime_error if it keeps changing.
ReportTable read_report(Report& report);

// Both tables' entries ordered by level (descending), code, subject, detail; revision and dropped
// are sums (each only grows, so the sum changes exactly when either does).
ReportTable merge_reports(const ReportTable& a, const ReportTable& b);

}  // namespace astroray::host

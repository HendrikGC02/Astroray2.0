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
#include "host/film/film_layout.h"
#include "host/film/film_view.h"
#include "host/report.h"

namespace astroray::host {

enum class PassSource : uint8_t { Combined, Aov, SampleCount, Bounces, RelError, Invalid };

struct PassDesc {
  std::string name;
  uint32_t components = 1;
  uint32_t unit = AR_UNIT_NONE;
  PassSource source = PassSource::Combined;
  std::string channel;  // the film channel behind Combined or an AOV
};

// The passes the layout allocated, in 0008 d10's order: Combined, the primary-hit AOVs
// (Position, Normal, UV, IndexMA), the statistics (AR Sample Count, AR Bounces, AR Rel Error,
// AR Invalid). Noisy Image and the denoising guides do not
// exist until a denoiser does. Throws std::invalid_argument for a band channel: the layout does
// not carry the response unit, so a band pass would have to guess ENERGY or PHOTON.
std::vector<PassDesc> list_passes(const FilmLayout& layout);

// The floats read_pass writes: pixels * components for MEAN and VARIANCE, pixels for COUNT.
// Throws std::invalid_argument for a quantity the pass cannot give: VARIANCE only for Combined
// (a MOMENT channel), COUNT only for Combined (n per pixel), AR Sample Count
// and AR Invalid (the raw counter); AR Sample Count and AR Invalid also give MEAN, the same value.
uint64_t pass_float_count(const PassDesc& pass, uint32_t quantity, uint32_t width, uint32_t height);

// Writes interleaved (pixel-major) floats for the whole render rectangle. `n` must equal
// pass_float_count exactly: a wrong size fails, it never truncates. NaN marks "no data" where
// the film has none (n = 0, or n < 2 for a variance); AR Rel Error is -1 below two samples.
void read_pass(const FilmView& film, const PassDesc& pass, uint32_t quantity, uint32_t n, float* dst);

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

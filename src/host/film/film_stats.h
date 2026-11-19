// SPDX-License-Identifier: Apache-2.0
// Readback of the statistics channels and the INVALID_SAMPLE report entry. CPU film memory only;
// a GPU film is read back to host first by the caller.
// Ref: docs/decisions/0008-film-outputs-denoise.md d12 (rel error, reset rule), d6 (invalid
// samples); 0001 D2.11 (INVALID_SAMPLE: count = samples not written).
#pragma once
#include <cstdint>
#include <string>

#include "host/film/film_layout.h"
#include "host/report.h"
#include "kernel/film/film_write.h"

namespace astroray::host {

// n >= 2: max over the three display components of sqrt(Var[mean]) / max(|mean|, 1e-6), the
// variance of the mean from moment_variance_of_mean (a tiny negative rounding value gives 0).
// n < 2: -1 ("no data").
double film_rel_error(const ar::FilmData& f, uint32_t pixel);

// Mean scattering events per path, ADD plane / paths ended; NaN when none ended or statistics were
// not requested.
double film_bounce_mean(const ar::FilmData& f, uint32_t pixel);

// One component of an ADD channel as its readback value: the plane divided by n, except a
// sample0 channel, which is returned as stored. Throws std::invalid_argument for a channel that
// is absent or not ADD; NaN when n = 0 for an averaged channel.
// Ref: 0008 d10 (which AOVs keep sample 0), d13a (readback mean).
double film_add_channel_value(const FilmLayout& layout, const ar::FilmData& f, const std::string& name,
                              uint32_t component, uint32_t pixel);

// Invalid samples over the render sub-rectangle.
uint64_t film_invalid_total(const ar::FilmData& f);

// One INVALID_SAMPLE entry per film. report() raises only what is new since the last call, so
// the entry's count equals the samples not written; on_film_reset() clears the entry and the
// bookkeeping together, with the film's clear. A total below what was already reported means the
// film was cleared without on_film_reset(): report() throws std::logic_error. report_pass() takes
// the invalid samples one resolve counted (FilmData::invalid_total) instead of summing the plane;
// what can go wrong there is a counter that was not cleared before the resolve (it then holds
// earlier passes or stale memory), so a count above `max_per_pass`, the most one resolve can count
// (one per slot and one splat per active pixel), throws std::logic_error.
class InvalidSampleReporter {
 public:
  void report(const ar::FilmData& f, Report& report, uint64_t generation);
  void report_pass(uint64_t pass_invalid, uint64_t max_per_pass, Report& report, uint64_t generation);
  void on_film_reset(Report& report);

 private:
  uint64_t reported_ = 0;
};

}  // namespace astroray::host

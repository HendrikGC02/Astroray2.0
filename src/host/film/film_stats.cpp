// SPDX-License-Identifier: Apache-2.0
#include "host/film/film_stats.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace astroray::host {

double film_rel_error(const ar::FilmData& f, uint32_t pixel) {
  const uint32_t n = f.n[pixel];
  if (n < 2) return -1.0;
  const uint32_t base = f.moment[0].offset;  // the display group is always channel 0
  double worst = 0.0;
  for (uint32_t c = 0; c < 3; ++c) {
    const uint64_t at = uint64_t(base + c) * f.plane_stride + pixel;
    const double var = std::max(0.0, moment_variance_of_mean(f.s1[at], f.s2[at], n));
    const double mean = moment_mean(f.s1[at], n);
    worst = std::max(worst, std::sqrt(var) / std::max(std::fabs(mean), 1e-6));
  }
  return worst;
}

double film_bounce_mean(const ar::FilmData& f, uint32_t pixel) {
  ar::Real mean;
  if (!ar::film_bounce_mean_value(f, pixel, &mean)) return std::numeric_limits<double>::quiet_NaN();
  return double(mean);
}

double film_add_channel_value(const FilmLayout& layout, const ar::FilmData& f, const std::string& name,
                              uint32_t component, uint32_t pixel) {
  const FilmChannel* c = layout.find(name);
  if (c == nullptr || c->kind != ChannelKind::Add || component >= c->components)
    throw std::invalid_argument("film: '" + name + "' is not an ADD channel with that component");
  const double v = f.add[uint64_t(uint32_t(c->offset) + component) * f.plane_stride + pixel];
  if (c->sample0) return v;
  return f.n[pixel] == 0 ? std::numeric_limits<double>::quiet_NaN() : v / double(f.n[pixel]);
}

uint64_t film_invalid_total(const ar::FilmData& f) {
  uint64_t total = 0;
  for (uint32_t y = 0; y < f.rect_height; ++y)
    for (uint32_t x = 0; x < f.rect_width; ++x) total += f.invalid[uint64_t(y) * f.width + x];
  return total;
}

void InvalidSampleReporter::report(const ar::FilmData& f, Report& report, uint64_t generation) {
  const uint64_t total = film_invalid_total(f);
  if (total < reported_)
    throw std::logic_error("film: invalid-sample total fell below the reported count; the film was cleared "
                           "without InvalidSampleReporter::on_film_reset");
  if (total == reported_) return;
  const double value[2] = {0, 0};
  report.raise(AR_REPORT_INVALID_SAMPLE, 0, "", total - reported_, value, generation);
  reported_ = total;
}

void InvalidSampleReporter::report_pass(uint64_t pass_invalid, uint64_t max_per_pass, Report& report,
                                        uint64_t generation) {
  if (pass_invalid > max_per_pass)
    throw std::logic_error("film: one resolve counted " + std::to_string(pass_invalid) +
                           " invalid samples, more than its " + std::to_string(max_per_pass) +
                           " slots and splats; the counter was not cleared before the resolve");
  if (pass_invalid == 0) return;
  const double value[2] = {0, 0};
  report.raise(AR_REPORT_INVALID_SAMPLE, 0, "", pass_invalid, value, generation);
  reported_ += pass_invalid;
}

void InvalidSampleReporter::on_film_reset(Report& report) {
  reported_ = 0;
  report.on_film_reset();
}

}  // namespace astroray::host

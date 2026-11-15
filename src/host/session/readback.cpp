// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0008-film-outputs-denoise.md d13a, d10, d12; 0004 D29; 0001 D2.11.
#include "host/session/readback.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>


namespace astroray::host {

std::vector<PassDesc> list_passes(const FilmLayout& layout) {
  std::vector<PassDesc> out;
  out.push_back({"Combined", 3, AR_UNIT_ENERGY, PassSource::Combined, "display"});
  const struct {
    const char* channel;
    const char* name;
  } aovs[] = {{"aov_position", "Position"}, {"aov_normal", "Normal"}, {"aov_uv", "UV"}, {"aov_material", "IndexMA"}};
  for (const auto& a : aovs)
    if (const FilmChannel* c = layout.find(a.channel))
      out.push_back({a.name, c->components, AR_UNIT_NONE, PassSource::Aov, a.channel});
  out.push_back({"AR Sample Count", 1, AR_UNIT_NONE, PassSource::SampleCount, ""});
  if (layout.find("bounces")) out.push_back({"AR Bounces", 1, AR_UNIT_NONE, PassSource::Bounces, "bounces"});
  out.push_back({"AR Rel Error", 1, AR_UNIT_NONE, PassSource::RelError, "display"});
  out.push_back({"AR Invalid", 1, AR_UNIT_NONE, PassSource::Invalid, ""});
  for (const FilmChannel& c : layout.channels())
    if (c.kind == ChannelKind::Moment && c.name != "display")
      throw std::invalid_argument("band pass '" + c.name +
                                  "': the film layout does not carry the response unit (energy or photon)");
  return out;
}

uint64_t pass_float_count(const PassDesc& pass, uint32_t quantity, uint32_t width, uint32_t height) {
  const uint64_t pixels = uint64_t(width) * height;
  switch (quantity) {
    case AR_QUANTITY_MEAN:
      return pixels * pass.components;
    case AR_QUANTITY_VARIANCE:
      if (pass.source != PassSource::Combined)
        throw std::invalid_argument("pass '" + pass.name + "' has no variance (only MOMENT passes do)");
      return pixels * pass.components;
    case AR_QUANTITY_COUNT:
      if (pass.source != PassSource::Combined && pass.source != PassSource::SampleCount &&
          pass.source != PassSource::Invalid)
        throw std::invalid_argument("pass '" + pass.name + "' has no count");
      return pixels;
    default:
      throw std::invalid_argument("unknown quantity " + std::to_string(quantity));
  }
}

namespace {

template <class T>
void interleave(const std::vector<T>& plane, uint32_t comps, uint32_t k, float* dst) {
  for (size_t j = 0; j < plane.size(); ++j) dst[j * comps + k] = float(plane[j]);
}

}  // namespace

void read_pass(const FilmView& film, const PassDesc& pass, uint32_t quantity, uint32_t n, float* dst) {
  const uint64_t want = pass_float_count(pass, quantity, film.width(), film.height());
  if (n != want)
    throw std::invalid_argument("read_pass '" + pass.name + "': the buffer holds " + std::to_string(n) +
                                " floats, the render needs exactly " + std::to_string(want));
  // Every value comes from FilmView, which owns the plane indexing and the S1 / n formulas.
  if (quantity == AR_QUANTITY_COUNT) {
    if (pass.source == PassSource::Invalid) interleave(film.invalid_counts(), 1, 0, dst);
    else interleave(film.sample_counts(), 1, 0, dst);
    return;
  }
  switch (pass.source) {
    case PassSource::Combined:
      for (uint32_t k = 0; k < pass.components; ++k)
        interleave(quantity == AR_QUANTITY_MEAN ? film.mean(pass.channel, k) : film.variance_of_mean(pass.channel, k),
                   pass.components, k, dst);
      break;
    case PassSource::Aov:
      for (uint32_t k = 0; k < pass.components; ++k) interleave(film.mean(pass.channel, k), pass.components, k, dst);
      break;
    case PassSource::SampleCount:
      interleave(film.sample_counts(), 1, 0, dst);
      break;
    case PassSource::Bounces:
      interleave(film.bounce_mean(), 1, 0, dst);
      break;
    case PassSource::RelError:
      interleave(film.rel_error(), 1, 0, dst);
      break;
    case PassSource::Invalid:
      interleave(film.invalid_counts(), 1, 0, dst);
      break;
  }
}

std::vector<uint64_t> counter_words(const PassCounters& c) {
  // A field added to PassCounters must be added to the list below, in format_counters' order.
  static_assert(sizeof(PassCounters) == (15 + ar::kStageCount) * sizeof(uint64_t),
                "PassCounters changed: update counter_words and its header comment");
  std::vector<uint64_t> w = {c.rays_closest, c.rays_shadow, c.steps, c.launches};
  for (uint32_t i = 0; i < ar::kStageCount; ++i) w.push_back(c.items[i]);
  for (uint64_t v : {c.active_path_steps, c.peak_active_paths, c.refills, c.camera_paths, c.light_paths, c.sorts,
                     c.state_bytes_moved, c.shadow_item_bytes_moved, c.lamp_crossings, c.shadow_rays_emitted,
                     c.permuted_launches})
    w.push_back(v);
  return w;
}

ReportTable read_report(Report& report) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    // Another session of the same scene may replace the snapshot while its entries are copied:
    // the copy counts only if the summary is unchanged afterwards (a change moves the revision).
    report.snapshot();
    ReportTable t;
    if (report.info(&t.info) != AR_OK) throw std::runtime_error("report: info failed");
    t.entries.resize(t.info.entry_count);
    bool ok = true;
    for (uint32_t i = 0; i < t.info.entry_count && ok; ++i) ok = report.entry(i, &t.entries[i]) == AR_OK;
    ar_report_info after{};
    if (ok && report.info(&after) == AR_OK && after.revision == t.info.revision &&
        after.entry_count == t.info.entry_count)
      return t;
  }
  throw std::runtime_error("report: the table kept changing while it was read");
}

ReportTable merge_reports(const ReportTable& a, const ReportTable& b) {
  ReportTable m;
  m.entries = a.entries;
  m.entries.insert(m.entries.end(), b.entries.begin(), b.entries.end());
  std::stable_sort(m.entries.begin(), m.entries.end(), [](const ar_report_entry& x, const ar_report_entry& y) {
    if (x.level != y.level) return x.level > y.level;
    if (x.code != y.code) return x.code < y.code;
    if (x.subject != y.subject) return x.subject < y.subject;
    return std::memcmp(x.detail, y.detail, sizeof x.detail) < 0;
  });
  m.info.revision = a.info.revision + b.info.revision;
  m.info.entry_count = uint32_t(m.entries.size());
  m.info.dropped = a.info.dropped + b.info.dropped;
  m.info.highest_level = m.entries.empty() ? AR_LEVEL_INFO : m.entries.front().level;
  return m;
}

}  // namespace astroray::host

// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0008-film-outputs-denoise.md d13a, d10, d12; 0004 D29; 0001 D2.11.
#include "host/session/readback.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "host/film/film_stats.h"

namespace astroray::host {
namespace {


bool is_moment(PassSource s) { return s == PassSource::Combined || s == PassSource::Band; }

const FilmChannel& channel_of(const FilmCopy& film, const PassDesc& pass) {
  const FilmChannel* c = film.layout().find(pass.channel);
  if (c == nullptr) throw std::invalid_argument("pass '" + pass.name + "': the film has no channel '" + pass.channel + "'");
  return *c;
}

}  // namespace

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
      out.push_back({c.name, c.components, AR_UNIT_ENERGY, PassSource::Band, c.name});
  return out;
}

FilmCopy::FilmCopy(Device& device, const Film& film) : layout_(film.layout()), bytes_(film.buffer().bytes) {
  device.download(bytes_.data(), film.buffer(), 0, bytes_.size());
  data_ = layout_.bind(bytes_.data());
  data_.rect_width = film.data().rect_width;
  data_.rect_height = film.data().rect_height;
}

uint64_t pass_float_count(const PassDesc& pass, uint32_t quantity, uint32_t width, uint32_t height) {
  const uint64_t pixels = uint64_t(width) * height;
  switch (quantity) {
    case AR_QUANTITY_MEAN:
      return pixels * pass.components;
    case AR_QUANTITY_VARIANCE:
      if (!is_moment(pass.source))
        throw std::invalid_argument("pass '" + pass.name + "' has no variance (only MOMENT passes do)");
      return pixels * pass.components;
    case AR_QUANTITY_COUNT:
      if (!is_moment(pass.source) && pass.source != PassSource::SampleCount && pass.source != PassSource::Invalid)
        throw std::invalid_argument("pass '" + pass.name + "' has no count");
      return pixels;
    default:
      throw std::invalid_argument("unknown quantity " + std::to_string(quantity));
  }
}

void read_pass(const FilmCopy& film, const PassDesc& pass, uint32_t quantity, uint32_t n, float* dst) {
  const uint64_t want = pass_float_count(pass, quantity, film.width(), film.height());
  if (n != want)
    throw std::invalid_argument("read_pass '" + pass.name + "': the buffer holds " + std::to_string(n) +
                                " floats, the render needs exactly " + std::to_string(want));
  const ar::FilmData& d = film.data();
  const uint32_t pixels = film.width() * film.height();
  const uint32_t comps = quantity == AR_QUANTITY_COUNT ? 1u : pass.components;
  for (uint32_t j = 0; j < pixels; ++j) {
    const uint32_t p = ar::film_active_pixel(d, j);
    float* out = dst + uint64_t(j) * comps;
    if (quantity == AR_QUANTITY_COUNT) {
      out[0] = float(pass.source == PassSource::Invalid ? d.invalid[p] : d.n[p]);
      continue;
    }
    switch (pass.source) {
      case PassSource::Combined:
      case PassSource::Band: {
        const FilmChannel& c = channel_of(film, pass);
        for (uint32_t k = 0; k < comps; ++k) {
          const uint64_t at = uint64_t(uint32_t(c.offset) + k) * d.plane_stride + p;
          out[k] = float(quantity == AR_QUANTITY_MEAN ? moment_mean(d.s1[at], d.n[p])
                                                      : moment_variance_of_mean(d.s1[at], d.s2[at], d.n[p]));
        }
        break;
      }
      case PassSource::Aov:
        for (uint32_t k = 0; k < comps; ++k)
          out[k] = float(film_add_channel_value(film.layout(), d, pass.channel, k, p));
        break;
      case PassSource::SampleCount:
        out[0] = float(d.n[p]);
        break;
      case PassSource::Bounces:
        out[0] = float(film_bounce_mean(d, p));
        break;
      case PassSource::RelError:
        out[0] = float(film_rel_error(d, p));
        break;
      case PassSource::Invalid:
        out[0] = float(d.invalid[p]);
        break;
    }
  }
}

std::vector<uint64_t> counter_words(const PassCounters& c) {
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
    report.snapshot();
    ReportTable t;
    if (report.info(&t.info) != AR_OK) throw std::runtime_error("report: info failed");
    t.entries.resize(t.info.entry_count);
    bool ok = true;
    for (uint32_t i = 0; i < t.info.entry_count && ok; ++i) ok = report.entry(i, &t.entries[i]) == AR_OK;
    if (ok) return t;
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

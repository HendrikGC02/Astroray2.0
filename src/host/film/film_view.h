// SPDX-License-Identifier: Apache-2.0
// The only way to read film planes on the host: a read-only snapshot of one film's memory, copied
// from the device at construction. It returns what readback returns (mean, variance of the mean,
// counts), so no caller re-derives S1 / n or the variance formula.
// Ref: docs/decisions/0008-film-outputs-denoise.md d13a (what readback returns), d17 (science
// data is reachable only through FilmView or the film_* writers).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <stdexcept>

#include "host/film/film_layout.h"
#include "host/film/film_stats.h"

namespace astroray::host {

class FilmView {
 public:
  // Copies the film's whole allocation from `device` (the caller has synchronized the device).
  FilmView(Device& device, const Film& film);
  FilmView(const FilmView&) = delete;
  FilmView& operator=(const FilmView&) = delete;
  FilmView(FilmView&&) = default;
  FilmView& operator=(FilmView&&) = default;

  const FilmLayout& layout() const { return layout_; }
  // The render sub-rectangle; every plane below has width() * height() values, rows bottom-up.
  uint32_t width() const { return data_.rect_width; }
  uint32_t height() const { return data_.rect_height; }

  // Samples per pixel (the film's n), the NSAMP companion.
  std::vector<uint32_t> sample_counts() const;
  // MOMENT channel: S1 / n (NaN where n = 0). ADD channel: film_add_channel_value (a sample0
  // channel as stored). Throws std::invalid_argument for any other kind or a bad component.
  std::vector<double> mean(const std::string& channel, uint32_t component) const;
  // MOMENT channel only: variance of the mean (NaN where n < 2). Throws std::invalid_argument.
  std::vector<double> variance_of_mean(const std::string& channel, uint32_t component) const;
  // COUNT channel only: the raw counters. Throws std::invalid_argument.
  std::vector<uint32_t> counts(const std::string& channel, uint32_t component) const;

 private:
  const FilmChannel& channel(const std::string& name, ChannelKind kind, uint32_t component) const;
  uint32_t pixel(uint32_t i) const { return (i / width()) * data_.width + i % width(); }

  FilmLayout layout_;
  std::vector<char> bytes_;
  ar::FilmData data_;  // pointers into bytes_ (a vector move keeps them valid)
};

inline FilmView::FilmView(Device& device, const Film& film) : layout_(film.layout()), bytes_(film.buffer().bytes) {
  device.download(bytes_.data(), film.buffer(), 0, bytes_.size());
  data_ = layout_.bind(bytes_.data());
  data_.rect_width = film.data().rect_width;
  data_.rect_height = film.data().rect_height;
}

inline const FilmChannel& FilmView::channel(const std::string& name, ChannelKind kind, uint32_t component) const {
  const FilmChannel* c = layout_.find(name);
  if (c == nullptr || c->kind != kind || component >= c->components)
    throw std::invalid_argument("film view: '" + name + "' is not a channel of the requested kind with component " +
                                std::to_string(component));
  return *c;
}

inline std::vector<uint32_t> FilmView::sample_counts() const {
  std::vector<uint32_t> out(size_t(width()) * height());
  for (size_t i = 0; i < out.size(); ++i) out[i] = data_.n[pixel(uint32_t(i))];
  return out;
}

inline std::vector<double> FilmView::mean(const std::string& name, uint32_t component) const {
  const FilmChannel* c = layout_.find(name);
  std::vector<double> out(size_t(width()) * height());
  if (c != nullptr && c->kind == ChannelKind::Moment) {
    channel(name, ChannelKind::Moment, component);
    const uint64_t plane = uint64_t(uint32_t(c->offset) + component) * data_.plane_stride;
    for (size_t i = 0; i < out.size(); ++i) {
      const uint32_t p = pixel(uint32_t(i));
      out[i] = moment_mean(data_.s1[plane + p], data_.n[p]);
    }
  } else {
    channel(name, ChannelKind::Add, component);
    for (size_t i = 0; i < out.size(); ++i)
      out[i] = film_add_channel_value(layout_, data_, name, component, pixel(uint32_t(i)));
  }
  return out;
}

inline std::vector<double> FilmView::variance_of_mean(const std::string& name, uint32_t component) const {
  const FilmChannel& c = channel(name, ChannelKind::Moment, component);
  const uint64_t plane = uint64_t(uint32_t(c.offset) + component) * data_.plane_stride;
  std::vector<double> out(size_t(width()) * height());
  for (size_t i = 0; i < out.size(); ++i) {
    const uint32_t p = pixel(uint32_t(i));
    out[i] = moment_variance_of_mean(data_.s1[plane + p], data_.s2[plane + p], data_.n[p]);
  }
  return out;
}

inline std::vector<uint32_t> FilmView::counts(const std::string& name, uint32_t component) const {
  const FilmChannel& c = channel(name, ChannelKind::Count, component);
  const uint64_t plane = uint64_t(uint32_t(c.offset) + component) * data_.plane_stride;
  std::vector<uint32_t> out(size_t(width()) * height());
  for (size_t i = 0; i < out.size(); ++i) out[i] = data_.count[plane + pixel(uint32_t(i))];
  return out;
}

}  // namespace astroray::host

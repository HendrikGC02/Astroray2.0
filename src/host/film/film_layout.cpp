// SPDX-License-Identifier: Apache-2.0
#include "host/film/film_layout.h"

#include <limits>
#include <stdexcept>

namespace astroray::host {
namespace {

constexpr uint64_t kAlign = 256;
uint64_t round_up(uint64_t v) { return (v + kAlign - 1) / kAlign * kAlign; }

struct Regions {
  uint64_t s1, s2, s1a, n, invalid, add, count, splat, slot_L, slot_flags, total;
};

Regions regions_of(uint32_t pixels, uint32_t slots, uint32_t moment, bool adaptive, uint32_t add, uint32_t count,
                   uint32_t splat) {
  Regions r{};
  uint64_t at = 0;
  const auto place = [&](uint64_t* out, uint64_t bytes) {
    *out = at;
    at += round_up(bytes);
  };
  const uint64_t plane8 = uint64_t(pixels) * sizeof(double);
  const uint64_t plane4 = uint64_t(pixels) * sizeof(uint32_t);
  place(&r.s1, plane8 * moment);
  place(&r.s2, plane8 * moment);
  place(&r.s1a, adaptive ? plane8 * moment : 0);
  place(&r.n, plane4);
  place(&r.invalid, plane4);
  place(&r.add, plane4 * add);
  place(&r.count, plane4 * count);
  place(&r.splat, plane4 * splat);
  place(&r.slot_L, uint64_t(slots) * 4u * sizeof(float));
  place(&r.slot_flags, uint64_t(slots) * sizeof(uint32_t));
  r.total = at;
  return r;
}

}  // namespace

FilmLayout FilmLayout::build(const FilmRequest& req) {
  if (req.max_width == 0 || req.max_height == 0) throw std::invalid_argument("film: size must be at least 1x1");
  FilmLayout l;
  l.width_ = req.max_width;
  l.height_ = req.max_height;
  l.slot_capacity_ = req.slot_capacity != 0 ? req.slot_capacity : req.max_width * req.max_height;
  l.adaptive_ = req.adaptive_sampling;

  std::vector<ChannelRequest> all;
  all.push_back({"display", ChannelKind::Moment, 3, ar::kFilmDisplayResponse});
  for (const ChannelRequest& c : req.extra) all.push_back(c);
  if (req.light_pass) all.push_back({"splat", ChannelKind::Splat, 3, 0});
  if (req.statistics) {
    all.push_back({"bounces", ChannelKind::Add, 1, 0});
    all.push_back({"bounce_max", ChannelKind::Count, 1, 0});
    all.push_back({"bounce_paths", ChannelKind::Count, 1, 0});
  }
  if (req.termination_stats) all.push_back({"term", ChannelKind::Count, ar::kTermCount, 0});
  if (req.primary_aov) {
    all.push_back({"aov_position", ChannelKind::Add, 3, 0, true});
    all.push_back({"aov_normal", ChannelKind::Add, 3, 0});
    all.push_back({"aov_uv", ChannelKind::Add, 2, 0});
    all.push_back({"aov_material", ChannelKind::Add, 1, 0, true});
  }
  if (req.guides) {
    all.push_back({"guide_albedo", ChannelKind::Add, 3, 0});
    all.push_back({"guide_normal", ChannelKind::Add, 3, 0});
  }

  uint32_t moment_channels = 0;
  for (const ChannelRequest& c : all) {
    for (const FilmChannel& have : l.channels_)
      if (have.name == c.name) throw std::invalid_argument("film: channel '" + c.name + "' requested twice");
    if (c.components == 0) throw std::invalid_argument("film: channel '" + c.name + "' has no components");
    FilmChannel ch{c.name, c.kind, c.components, -1, c.response, c.sample0};
    switch (c.kind) {
      case ChannelKind::Moment:
        if (c.components != (c.response == ar::kFilmDisplayResponse ? 3u : 1u))
          throw std::invalid_argument("film: moment channel '" + c.name +
                                      "' needs 3 components for display and 1 for a response channel");
        if (++moment_channels > ar::kFilmMaxMomentChannels)
          throw std::invalid_argument("film: more than " + std::to_string(ar::kFilmMaxMomentChannels) +
                                      " moment channels");
        ch.offset = int32_t(l.moment_planes_);
        l.moment_planes_ += c.components;
        break;
      case ChannelKind::Add:
        ch.offset = int32_t(l.add_planes_);
        l.add_planes_ += c.components;
        break;
      case ChannelKind::Count:
        ch.offset = int32_t(l.count_planes_);
        l.count_planes_ += c.components;
        break;
      case ChannelKind::Splat:
        if (c.components != 3) throw std::invalid_argument("film: the splat channel has 3 components");
        ch.offset = 0;
        l.splat_components_ = 3;
        break;
      case ChannelKind::Crypto:
        throw std::invalid_argument("film: Cryptomatte channels have no producer yet (0008 d11)");
    }
    l.channels_.push_back(ch);
  }
  return l;
}

const FilmChannel* FilmLayout::find(const std::string& name) const {
  for (const FilmChannel& c : channels_)
    if (c.name == name) return &c;
  return nullptr;
}

int32_t FilmLayout::offset(const std::string& name) const {
  const FilmChannel* c = find(name);
  return c ? c->offset : -1;
}

uint64_t FilmLayout::bytes() const {
  return regions_of(width_ * height_, slot_capacity_, moment_planes_, adaptive_, add_planes_, count_planes_,
                    splat_components_)
      .total;
}

ar::FilmData FilmLayout::bind(void* base) const {
  const Regions r = regions_of(width_ * height_, slot_capacity_, moment_planes_, adaptive_, add_planes_,
                               count_planes_, splat_components_);
  char* b = static_cast<char*>(base);
  ar::FilmData d{};
  d.width = width_, d.height = height_;
  d.rect_width = width_, d.rect_height = height_;
  d.plane_stride = width_ * height_;
  d.slot_capacity = slot_capacity_;
  d.splat_components = splat_components_;
  for (const FilmChannel& c : channels_)
    if (c.kind == ChannelKind::Moment) d.moment[d.moment_channel_count++] = {uint32_t(c.offset), c.components, c.response, 0};
  d.s1 = reinterpret_cast<double*>(b + r.s1);
  d.s2 = reinterpret_cast<double*>(b + r.s2);
  d.s1a = adaptive_ ? reinterpret_cast<double*>(b + r.s1a) : nullptr;
  d.n = reinterpret_cast<uint32_t*>(b + r.n);
  d.invalid = reinterpret_cast<uint32_t*>(b + r.invalid);
  d.add = add_planes_ ? reinterpret_cast<float*>(b + r.add) : nullptr;
  d.count = count_planes_ ? reinterpret_cast<uint32_t*>(b + r.count) : nullptr;
  d.splat = splat_components_ ? reinterpret_cast<float*>(b + r.splat) : nullptr;
  d.slot_L = reinterpret_cast<float*>(b + r.slot_L);
  d.slot_flags = reinterpret_cast<uint32_t*>(b + r.slot_flags);
  d.bounce_sum = offset("bounces");
  d.bounce_max = offset("bounce_max");
  d.bounce_paths = offset("bounce_paths");
  d.term_hist = offset("term");
  d.aov_position = offset("aov_position");
  d.aov_normal = offset("aov_normal");
  d.aov_uv = offset("aov_uv");
  d.aov_material = offset("aov_material");
  d.guide_albedo = offset("guide_albedo");
  d.guide_normal = offset("guide_normal");
  d.film_exposure = 1.0;
  d.inv_radiance_scale = 1.0;
  return d;
}

Film::Film(Device& device, const FilmLayout& layout, Owner owner)
    : device_(device),
      layout_(layout),
      buffer_(device.allocate(LedgerCategory::FILM, layout.bytes(), owner)),
      data_(layout.bind(buffer_.ptr)) {
  clear();
}

Film::~Film() { device_.free(buffer_); }

void Film::set_render_size(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > layout_.width() || height > layout_.height())
    throw std::invalid_argument("film: render size " + std::to_string(width) + "x" + std::to_string(height) +
                                " is outside the allocated " + std::to_string(layout_.width()) + "x" +
                                std::to_string(layout_.height()));
  data_.rect_width = width;
  data_.rect_height = height;
}

void Film::check_resolve_fits(uint32_t k) const {
  const uint64_t need = uint64_t(data_.rect_width) * data_.rect_height * k;
  if (need > data_.slot_capacity)
    throw std::length_error("film: a pass of " + std::to_string(uint64_t(data_.rect_width) * data_.rect_height) +
                            " active pixels x " + std::to_string(k) + " samples needs " + std::to_string(need) +
                            " slots, the film holds " + std::to_string(data_.slot_capacity));
}

void Film::clear() { device_.memset(buffer_, 0, 0, buffer_.bytes); }

double moment_mean(double s1, uint32_t n) {
  return n == 0 ? std::numeric_limits<double>::quiet_NaN() : s1 / double(n);
}

double moment_variance_of_mean(double s1, double s2, uint32_t n) {
  if (n < 2) return std::numeric_limits<double>::quiet_NaN();
  const double dn = double(n);
  return (s2 - s1 * s1 / dn) / (dn * (dn - 1.0));
}

}  // namespace astroray::host

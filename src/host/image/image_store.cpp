// SPDX-License-Identifier: Apache-2.0
#include "host/image/image_store.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "../../kernel/color/srgb.h"
#include "host/image/decode.h"
#include "host/scene/edit.h"

namespace astroray::host {
namespace {

namespace fs = std::filesystem;

// The linear-RGB mean: each texel is decoded by the transfer function before averaging (the
// mean of the light, not of the code values). Grey images count as r = g = b; alpha is ignored.
// An sRGB texel's value is the kernel's own table entry for the byte (Closest returns exactly it).
std::array<float, 3> image_mean(const Image& im) {
  float srgb[ar::kSrgbTableSize];
  ar::srgb_decode_table(srgb);
  double lut[256];
  for (int i = 0; i < 256; ++i) lut[i] = im.encoding == ar::TexEncoding::SRGB ? double(srgb[i]) : i / 255.0;
  double sum[3] = {0, 0, 0};
  const uint8_t* t = im.texels.data();
  const size_t n = size_t(im.width) * im.height;
  for (size_t i = 0; i < n; ++i, t += im.channels) {
    if (im.channels == 1) {
      const double v = lut[t[0]];
      sum[0] += v, sum[1] += v, sum[2] += v;
    } else {
      sum[0] += lut[t[0]], sum[1] += lut[t[1]], sum[2] += lut[t[2]];
    }
  }
  return {float(sum[0] / double(n)), float(sum[1] / double(n)), float(sum[2] / double(n))};
}

ImageResult magenta(uint32_t code, const std::string& detail) {
  auto im = std::make_shared<Image>();
  im->width = im->height = 1;
  im->channels = 3;
  im->encoding = ar::TexEncoding::LINEAR;  // (1, 0, 1) in linear light, as the VM's missing-image colour
  im->texels = {255, 0, 255};
  im->magenta = true;
  im->mean = {1.0f, 0.0f, 1.0f};
  return {std::move(im), code, detail};
}

// FNV-1a 64 content id of packed bytes. It only buckets the cache: a hit also compares the bytes,
// so a collision can never alias two images.
// Ref: Fowler-Noll-Vo hash, http://www.isthe.com/chongo/tech/comp/fnv/ (offset basis 14695981039346656037, prime 1099511628211).
std::string content_id(const std::vector<uint8_t>& b) {
  uint64_t h = 14695981039346656037ull;
  for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
  char buf[20];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

struct Stored {
  ar_image_desc desc = {};
  std::string path;
  std::vector<uint8_t> packed;
};

}  // namespace

std::string interop_name(uint32_t v) {
  switch (v) {
    case AR_COLOR_INTEROP_SRGB_REC709: return "srgb_rec709_display";
    case AR_COLOR_INTEROP_LIN_REC709: return "lin_rec709_scene";
    case AR_COLOR_INTEROP_LIN_REC2020: return "lin_rec2020_scene";
    case AR_COLOR_INTEROP_LIN_AP1: return "lin_ap1_scene";
    case AR_COLOR_INTEROP_LIN_AP0: return "lin_ap0_scene";
    case AR_COLOR_INTEROP_SRGB_P3D65: return "srgb_p3d65_display";
    case AR_COLOR_INTEROP_DATA: return "data";
    default: return "unknown interop id " + std::to_string(v);
  }
}

bool interop_encoding(uint32_t v, ar::TexEncoding* out) {
  switch (v) {
    case AR_COLOR_INTEROP_SRGB_REC709: return *out = ar::TexEncoding::SRGB, true;
    case AR_COLOR_INTEROP_LIN_REC709: return *out = ar::TexEncoding::LINEAR, true;
    case AR_COLOR_INTEROP_DATA: return *out = ar::TexEncoding::DATA, true;
    default: return false;
  }
}

ImageStore::ImageStore(std::shared_ptr<Ledger> ledger) : ledger_(std::move(ledger)), magenta_(magenta(0, std::string()).image) {
  auto a = std::make_shared<ImageArrays>();
  ar::srgb_decode_table(a->srgb_decode.data());
  arrays_ = std::move(a);
}

ImageStore::~ImageStore() {
  if (ledger_id_ != 0) ledger_->remove(ledger_id_);
}

ImageResult ImageStore::load(const ar_image_desc& d, const std::vector<uint8_t>& packed) {
  ar::TexEncoding enc = ar::TexEncoding::LINEAR;
  if (!interop_encoding(d.color_interop, &enc)) return magenta(AR_REPORT_COLORSPACE_UNSUPPORTED, interop_name(d.color_interop));
  std::vector<uint8_t> file;
  const std::vector<uint8_t>* data = &packed;
  std::string id;
  if (d.source == AR_IMAGE_SOURCE_FILE) {
    const fs::path p = fs::u8path(d.path);
    std::error_code ec;
    const uint64_t size = fs::file_size(p, ec);
    if (ec) return magenta(AR_REPORT_IMAGE_UNREADABLE, "cannot read file: " + ec.message());
    const int64_t mtime = int64_t(fs::last_write_time(p, ec).time_since_epoch().count());
    if (ec) return magenta(AR_REPORT_IMAGE_UNREADABLE, "cannot read file: " + ec.message());
    std::ifstream f(p, std::ios::binary);
    if (!f) return magenta(AR_REPORT_IMAGE_UNREADABLE, "cannot open file");
    file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (file.size() != size) return magenta(AR_REPORT_IMAGE_UNREADABLE, "file read was short");
    data = &file;
    id = "file:" + std::string(d.path) + "|" + std::to_string(size) + "|" + std::to_string(mtime);
  } else {
    id = "mem:" + content_id(packed) + "|" + std::to_string(packed.size());
  }
  // The reload counter of 0007 d21 has no field in ar_image_desc yet; it is 0 until the ABI has one.
  id += "|" + std::to_string(d.color_interop) + "|" + std::to_string(d.alpha_mode) + "|" + std::to_string(d.udim_tile) + "|0";
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(id);
    if (it != cache_.end()) {
      if (auto hit = it->second.image.lock()) {
        if (d.source == AR_IMAGE_SOURCE_FILE || it->second.packed == packed) return {std::move(hit), 0, std::string()};
      } else {
        cache_.erase(it);
      }
    }
  }
  DecodedImage dec;
  std::string error;
  if (!decode_image_bytes(data->data(), data->size(), &dec, &error)) return magenta(AR_REPORT_IMAGE_UNREADABLE, error);
  auto im = std::make_shared<Image>();
  im->width = dec.width, im->height = dec.height, im->channels = dec.channels;
  im->encoding = enc;
  im->alpha_mode = d.alpha_mode;
  im->texels = std::move(dec.texels);
  im->mean = image_mean(*im);
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    for (auto it = cache_.begin(); it != cache_.end();) it = it->second.image.expired() ? cache_.erase(it) : std::next(it);
    cache_[id] = CacheEntry{im, d.source == AR_IMAGE_SOURCE_FILE ? std::vector<uint8_t>() : packed};
  }
  return {std::move(im), 0, std::string()};
}

ar_status ImageStore::set_image(Edit& edit, const ar_image_desc* d) {
  if (d == nullptr) return edit.fail("invalid argument: ar_edit_set_image desc is null");
  // The TEX_IMAGE immediate is 32 bits and names the image by its key (0007 appendix, ar_image_desc).
  if (d->key > UINT32_MAX) return edit.fail("invalid argument: image key must fit 32 bits (TEX_IMAGE immediate)");
  if (d->source > AR_IMAGE_SOURCE_MEMORY) throw std::invalid_argument("ar_edit_set_image source is out of range or not decodable in this build");
  if (d->alpha_mode > AR_ALPHA_IGNORE) throw std::invalid_argument("ar_edit_set_image alpha_mode out of range");
  auto s = std::make_shared<Stored>();
  s->desc = *d;
  if (d->source == AR_IMAGE_SOURCE_FILE) {
    if (d->path == nullptr || d->path[0] == 0) throw std::invalid_argument("ar_edit_set_image FILE source has no path");
    s->path = d->path;
  } else {
    if (d->bytes == nullptr && d->byte_count != 0) throw std::invalid_argument("ar_edit_set_image MEMORY bytes is null with a nonzero count");
    if (d->bytes != nullptr) s->packed.assign(d->bytes, d->bytes + d->byte_count);
  }
  s->desc.path = s->path.c_str();
  s->desc.bytes = s->packed.data();
  auto result = std::make_shared<ImageResult>();
  auto item = std::make_shared<EditItem>();
  item->key = d->key;
  item->kind = AR_KIND_IMAGE;
  item->needs_device = false;
  std::shared_ptr<ImageStore> self = shared_from_this();
  const ar_key key = d->key;
  item->job = [self, s, result] { *result = self->load(s->desc, s->packed); };
  item->on_active = [self, key, result] { self->install(key, *result); };
  item->on_dropped = [self, key] { self->dropped(key); };
  if (ar_status st = edit.stage(item)) return st;
  if (ar_status st = edit.item_commit(key)) return st;
  std::lock_guard<std::mutex> lock(mutex_);
  assign_slot_locked(key);
  return AR_OK;
}

uint32_t ImageStore::assign_slot_locked(ar_key key) {
  auto it = slots_.find(key);
  if (it != slots_.end()) return it->second;
  uint32_t s = uint32_t(slot_keys_.size());
  if (!free_slots_.empty()) {
    s = free_slots_.back();
    free_slots_.pop_back();
    slot_keys_[s] = key;
  } else {
    slot_keys_.push_back(key);
  }
  slots_[key] = s;
  return s;
}

// Only a key with no active image gives its slot up; a program compiled against the slot was
// compiled while the image was active and is recompiled in the generation that removes it.
void ImageStore::free_slot_locked(ar_key key) {
  auto it = slots_.find(key);
  if (it == slots_.end() || active_.count(key) != 0) return;
  slot_keys_[it->second] = 0;
  free_slots_.push_back(it->second);
  slots_.erase(it);
}

// A dropped set never happened. A later queued set of the same key reassigns a slot on install.
void ImageStore::dropped(ar_key key) {
  std::lock_guard<std::mutex> lock(mutex_);
  free_slot_locked(key);
}

void ImageStore::install(ar_key key, const ImageResult& r) {
  std::lock_guard<std::mutex> lock(mutex_);
  active_[key] = r.image;
  assign_slot_locked(key);  // normally the slot set_image assigned; again if a removal freed it
  if (r.code != 0) pending_.push_back({key, r.code, r.detail});
  rebuild_locked();
}

void ImageStore::remove(bool reset, const std::set<ar_key>& removed, const std::set<ar_key>& keep) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::set<ar_key> gone;
  if (reset) {
    for (auto it = active_.begin(); it != active_.end();) {
      if (keep.count(it->first)) {
        ++it;
      } else {
        gone.insert(it->first);
        it = active_.erase(it);
      }
    }
  }
  for (ar_key k : removed)
    if (!keep.count(k)) active_.erase(k), gone.insert(k);
  for (ar_key k : gone) free_slot_locked(k);
  rebuild_locked();
}

void ImageStore::rebuild_locked() {
  auto a = std::make_shared<ImageArrays>();
  a->srgb_decode = arrays_->srgb_decode;
  uint64_t bytes = 0;
  std::set<const Image*> counted;  // a cached image shared by several keys is one allocation
  for (uint32_t s = 0; s < slot_keys_.size(); ++s) {
    const ar_key key = slot_keys_[s];
    auto it = slots_.find(key);
    const bool held = it != slots_.end() && it->second == s;
    auto act = held ? active_.find(key) : active_.end();
    const std::shared_ptr<const Image>& image = act != active_.end() ? act->second : magenta_;
    const Image& im = *image;
    ar::DeviceImageView v = {};
    v.texels = uint64_t(uintptr_t(im.texels.data()));
    v.width = im.width, v.height = im.height, v.channels = im.channels, v.encoding = uint32_t(im.encoding);
    a->keys.push_back(held ? key : 0);
    a->views.push_back(v);
    a->images.push_back(image);
    if (counted.insert(&im).second) bytes += im.bytes();
  }
  const uint64_t id = bytes != 0 ? ledger_->add(Owner{}, LedgerCategory::TEXTURES, bytes) : 0;
  if (ledger_id_ != 0) ledger_->remove(ledger_id_);
  ledger_id_ = id;
  arrays_ = std::move(a);
}

void ImageStore::raise_entries(Scene& scene, uint64_t generation) {
  std::vector<Pending> pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending.swap(pending_);
  }
  for (const Pending& p : pending) {
    scene.ctx().log.line("WARNING", "image " + std::to_string(p.key) + ": " + p.detail + " (rendered magenta)");
    scene.report().raise(p.code, p.key, p.detail, 1, nullptr, generation);
  }
}

std::shared_ptr<const Image> ImageStore::find(ar_key key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = active_.find(key);
  return it == active_.end() ? nullptr : it->second;
}

bool ImageStore::mean(ar_key key, std::array<float, 3>* rgb) const {
  const std::shared_ptr<const Image> im = find(key);
  if (!im) return false;
  *rgb = im->mean;
  return true;
}

bool ImageStore::slot(ar_key key, uint32_t* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = slots_.find(key);
  if (it == slots_.end() || active_.count(key) == 0) return false;
  *out = it->second;
  return true;
}

std::shared_ptr<const ImageArrays> ImageStore::arrays() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return arrays_;
}

}  // namespace astroray::host

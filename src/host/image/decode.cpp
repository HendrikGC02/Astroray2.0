// SPDX-License-Identifier: Apache-2.0
// The one translation unit that compiles stb_image. Only the four 8-bit loaders and Radiance HDR
// are enabled and there is no stdio: FILE sources are read by the caller and decoded from memory,
// so the decoder has no path handling and no state of ours (its failure string is thread-local; the
// stb file-scope variables that linear output compiles in are never written by this decoder).
// EXR goes to decode_exr.cpp by its magic.
// Ref: third_party/stb_image/stb_image.h (nothings/stb, pinned in third_party/deps.toml);
// docs/decisions/0007-blender-integration.md d13.
#include "host/image/decode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_ONLY_HDR
#pragma warning(push, 0)
#include "../../../third_party/stb_image/stb_image.h"
#pragma warning(pop)

namespace astroray::host {
namespace {

struct Free {
  void* p;
  ~Free() { stbi_image_free(p); }
};

bool decode_hdr(const uint8_t* bytes, int n, DecodedImage* out, std::string* error) {
  int w = 0, h = 0, comp = 0;
  float* px = stbi_loadf_from_memory(bytes, n, &w, &h, &comp, 0);
  if (px == nullptr) {
    const char* why = stbi_failure_reason();
    return *error = why != nullptr ? why : "unknown decoder error", false;
  }
  Free guard{px};
  if (w <= 0 || h <= 0 || comp < 1 || comp > 4) return *error = "decoder returned an invalid size", false;
  out->width = uint32_t(w), out->height = uint32_t(h), out->channels = uint32_t(comp);
  out->format = ar::TexFormat::FLOAT;
  const size_t row = size_t(w) * size_t(comp) * 4;
  out->texels.resize(row * size_t(h));
  for (int y = 0; y < h; ++y)  // flip to bottom-up
    std::memcpy(out->texels.data() + size_t(h - 1 - y) * row, reinterpret_cast<const uint8_t*>(px) + size_t(y) * row, row);
  return true;
}

const uint8_t kExrMagic[4] = {0x76, 0x2f, 0x31, 0x01};

bool decode_unchecked(const uint8_t* bytes, uint64_t count, DecodedImage* out, std::string* error) {
  *out = DecodedImage{};
  if (bytes == nullptr || count == 0) return *error = "empty image data", false;
  if (count > uint64_t(std::numeric_limits<int>::max())) return *error = "image data larger than 2 GiB", false;
  const int n = int(count);
  if (count >= 4 && std::memcmp(bytes, kExrMagic, 4) == 0) return decode_exr_bytes(bytes, count, out, error);
  // Radiance RGBE becomes float32 as stored (stb's rgbe2float: no gamma, no scale).
  if (stbi_is_hdr_from_memory(bytes, n)) return decode_hdr(bytes, n, out, error);
  // stb narrows 16-bit PNGs to 8 bits on a plain load; that would be a silent loss of precision.
  if (stbi_is_16_bit_from_memory(bytes, n)) return *error = "16-bit integer images are not supported (PNG stays 8-bit)", false;
  int w = 0, h = 0, comp = 0;
  stbi_uc* px = stbi_load_from_memory(bytes, n, &w, &h, &comp, 0);
  if (px == nullptr) {
    const char* why = stbi_failure_reason();
    return *error = why != nullptr ? why : "unknown decoder error", false;
  }
  Free guard{px};
  if (w <= 0 || h <= 0 || comp < 1 || comp > 4) return *error = "decoder returned an invalid size", false;
  out->width = uint32_t(w), out->height = uint32_t(h);
  // Grey+alpha has no kernel layout (channels are 1, 3 or 4): it becomes grey,grey,grey,alpha.
  const size_t from = size_t(comp), to = comp == 2 ? 4u : size_t(comp);
  out->channels = uint32_t(to);
  out->texels.resize(size_t(w) * size_t(h) * to);
  for (int y = 0; y < h; ++y) {
    const stbi_uc* src = px + size_t(y) * size_t(w) * from;
    uint8_t* dst = out->texels.data() + size_t(h - 1 - y) * size_t(w) * to;  // flip to bottom-up
    if (comp != 2) {
      std::memcpy(dst, src, size_t(w) * to);
    } else {
      for (int x = 0; x < w; ++x) dst[4 * x] = dst[4 * x + 1] = dst[4 * x + 2] = src[2 * x], dst[4 * x + 3] = src[2 * x + 1];
    }
  }
  return true;
}

}  // namespace

bool find_bad_texel(const uint8_t* texels, ar::TexFormat format, uint32_t channels, uint64_t texel_count, BadTexel* out) {
  if (format == ar::TexFormat::U8) return false;
  const uint64_t n = texel_count * channels;
  const bool half = format == ar::TexFormat::HALF;
  for (uint64_t i = 0; i < n; ++i) {
    float v;
    if (half) {
      uint16_t b;
      std::memcpy(&b, texels + i * 2, 2);
      v = ar::half_to_float(b);
    } else {
      std::memcpy(&v, texels + i * 4, 4);
    }
    // !(0 <= v <= limit) is also true for a NaN; an Inf is above the limit; -0 is not negative.
    if (!(v >= 0.0f && v <= kMaxTexelMagnitude)) {
      out->index = i / channels, out->channel = uint32_t(i % channels), out->value = v;
      return true;
    }
  }
  return false;
}

std::string bad_texel_message(const BadTexel& t, uint32_t width, uint32_t channels) {
  const char* const rgba = "RGBA";
  const std::string channel = channels == 1 ? "Y" : std::string(1, rgba[std::min<uint32_t>(t.channel, 3)]);
  const std::string where = " in channel " + channel + " at texel (" + std::to_string(t.index % width) + ", " +
                            std::to_string(t.index / width) + "), y from the bottom (index " + std::to_string(t.index) + ")";
  if (!std::isfinite(t.value)) return "NaN or Inf" + where;
  if (t.value < 0.0f) {
    char neg[32];
    std::snprintf(neg, sizeof neg, "%.9g", double(t.value));
    return std::string("negative value ") + neg + where + ": a texel the engine cannot use is refused, never clamped";
  }
  char v[32], lim[32];
  std::snprintf(v, sizeof v, "%.9g", double(t.value));
  std::snprintf(lim, sizeof lim, "%.9g", double(kMaxTexelMagnitude));
  return std::string("value ") + v + where + ", above the limit " + lim;
}

bool decode_image_bytes(const uint8_t* bytes, uint64_t count, DecodedImage* out, std::string* error) {
  if (!decode_unchecked(bytes, count, out, error)) return false;
  BadTexel bad;
  if (!find_bad_texel(out->texels.data(), out->format, out->channels, uint64_t(out->width) * out->height, &bad)) return true;
  *error = bad_texel_message(bad, out->width, out->channels);
  *out = DecodedImage{};
  return false;
}

}  // namespace astroray::host

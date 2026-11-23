// SPDX-License-Identifier: Apache-2.0
// The one translation unit that compiles tinyexr and its bundled miniz. EXR is read, never
// written (R54): the engine has no EXR writer, so only the loading entry points are used. Pixels
// keep the file's precision: half stays half, and any float channel makes the image float (the
// other channels widen exactly). Channels are found by name, never by position.
// Ref: third_party/tinyexr/tinyexr.h (syoyo/tinyexr, pinned in third_party/deps.toml);
// OpenEXR file format documentation (channel names, HALF/FLOAT/UINT pixel types);
// docs/decisions/0007-blender-integration.md d13.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "host/image/decode.h"

#pragma warning(push, 0)
#pragma warning(disable : 4702)  // unreachable code: level 0 does not cover this one in the vendored header
// Reading only (R54), so miniz is built without its compressor: miniz.c's tdefl half does not
// compile as C++ (a tentative `static const` array definition). miniz is included first and
// tinyexr is told not to include it: its own `#include <miniz.h>` needs an include directory this
// build does not add, and with TINYEXR_USE_MINIZ 0 it falls back to the zlib-compatible names
// (uncompress, Z_OK) miniz.h defines. The two compressor names its unreachable writer code still
// mentions are stubs that fail.
#define MINIZ_NO_DEFLATE_APIS
#include "../../../third_party/tinyexr/miniz.c"
static uLong compressBound(uLong n) { return n; }
static int compress(Bytef*, uLong*, const Bytef*, uLong) { return -1; }
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_THREAD 0
#define TINYEXR_IMPLEMENTATION
#include "../../../third_party/tinyexr/tinyexr.h"
#pragma warning(pop)

namespace astroray::host {
namespace {

struct HeaderGuard {
  EXRHeader h;
  HeaderGuard() { InitEXRHeader(&h); }
  ~HeaderGuard() { FreeEXRHeader(&h); }
};

struct ImageGuard {
  EXRImage i;
  ImageGuard() { InitEXRImage(&i); }
  ~ImageGuard() { FreeEXRImage(&i); }
};

// tinyexr's message (it ends in a newline), freed.
std::string take_message(const char* err, const char* fallback) {
  std::string s = err != nullptr ? err : fallback;
  if (err != nullptr) FreeEXRErrorMessage(err);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s.empty() ? std::string(fallback) : s;
}

// A half or float bit pattern with an all-ones exponent is NaN or Inf.
bool half_not_finite(uint16_t b) { return (b & 0x7c00u) == 0x7c00u; }
bool float_not_finite(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return (b & 0x7f800000u) == 0x7f800000u;
}

}  // namespace

bool decode_exr_bytes(const uint8_t* bytes, uint64_t count, DecodedImage* out, std::string* error) {
  EXRVersion version;
  if (ParseEXRVersionFromMemory(&version, bytes, size_t(count)) != TINYEXR_SUCCESS) return *error = "not a readable EXR file", false;
  if (version.multipart) return *error = "multipart EXR files are not supported", false;
  if (version.non_image) return *error = "deep EXR files are not supported", false;

  HeaderGuard hg;
  EXRHeader& hd = hg.h;
  const char* err = nullptr;
  if (ParseEXRHeaderFromMemory(&hd, &version, bytes, size_t(count), &err) != TINYEXR_SUCCESS)
    return *error = "EXR header: " + take_message(err, "unreadable header"), false;

  // R, G, B (and A) or Y (and A) by name; every other channel (Z, layers, ...) is ignored. A
  // luminance-chroma file (Y with RY and BY) would need a conversion this decoder does not do, so
  // it is refused rather than read as grey.
  int idx[5] = {-1, -1, -1, -1, -1};  // R, G, B, A, Y
  static const char* const names[5] = {"R", "G", "B", "A", "Y"};
  bool chroma = false;
  for (int c = 0; c < hd.num_channels; ++c) {
    for (int k = 0; k < 5; ++k)
      if (std::strcmp(hd.channels[c].name, names[k]) == 0) idx[k] = c;
    chroma = chroma || std::strcmp(hd.channels[c].name, "RY") == 0 || std::strcmp(hd.channels[c].name, "BY") == 0;
  }
  if (chroma) return *error = "EXR uses the luminance-chroma layout (RY, BY), which is not supported", false;
  const bool rgb = idx[0] >= 0 || idx[1] >= 0 || idx[2] >= 0;
  std::vector<int> kept;  // channel indices in output order
  if (rgb) {
    std::string missing;
    for (int k = 0; k < 3; ++k)
      if (idx[k] < 0) missing += std::string(missing.empty() ? "" : ", ") + names[k];
    if (!missing.empty()) return *error = "EXR is missing channel " + missing, false;
    kept = {idx[0], idx[1], idx[2]};
    if (idx[3] >= 0) kept.push_back(idx[3]);
  } else if (idx[4] >= 0) {
    // Grey becomes grey,grey,grey (and alpha), the layout RGBA files and grey+alpha 8-bit images use.
    if (idx[3] >= 0)
      kept = {idx[4], idx[4], idx[4], idx[3]};
    else
      kept = {idx[4]};
  } else {
    return *error = "EXR has no R, G, B or Y channel", false;
  }
  bool any_float = false;
  for (int c : kept) {  // a repeated Y is checked more than once, harmlessly
    const EXRChannelInfo& ci = hd.channels[c];
    if (ci.pixel_type == TINYEXR_PIXELTYPE_UINT) return *error = std::string("EXR channel ") + ci.name + " is UINT, which is not an image value", false;
    if (ci.x_sampling != 1 || ci.y_sampling != 1) return *error = std::string("EXR channel ") + ci.name + " is subsampled", false;
    any_float = any_float || ci.pixel_type == TINYEXR_PIXELTYPE_FLOAT;
  }
  const int want = any_float ? TINYEXR_PIXELTYPE_FLOAT : TINYEXR_PIXELTYPE_HALF;
  for (int c : kept) hd.requested_pixel_types[c] = want;  // the other channels keep their stored type

  ImageGuard ig;
  EXRImage& im = ig.i;
  err = nullptr;
  if (LoadEXRImageFromMemory(&im, &hd, bytes, size_t(count), &err) != TINYEXR_SUCCESS) {
    // tinyexr has already freed whatever it allocated and left the pointers dangling: freeing again
    // (the guard's destructor) would corrupt the heap.
    InitEXRImage(&im);
    return *error = "EXR data: " + take_message(err, "unreadable pixel data"), false;
  }
  const int w = im.width, h = im.height;
  if (w <= 0 || h <= 0) return *error = "decoder returned an invalid size", false;
  const uint32_t channels = uint32_t(kept.size());
  const uint64_t comp = any_float ? 4u : 2u;
  const uint64_t total = uint64_t(w) * uint64_t(h) * channels * comp;
  if (total > (uint64_t(1) << 32)) return *error = "EXR image larger than 4 GiB", false;

  out->width = uint32_t(w), out->height = uint32_t(h), out->channels = channels;
  out->format = any_float ? ar::TexFormat::FLOAT : ar::TexFormat::HALF;
  out->texels.assign(size_t(total), 0);
  // Copies planar pixel `src_index` of every kept channel from `planes` to texel (x, top row).
  const auto put = [&](unsigned char* const* planes, size_t src_index, int x, int top) {
    const int y = h - 1 - top;  // bottom-up, Blender's order
    for (uint32_t k = 0; k < channels; ++k)
      std::memcpy(out->texels.data() + ((size_t(y) * size_t(w) + size_t(x)) * channels + k) * size_t(comp),
                  planes[kept[k]] + src_index * size_t(comp), size_t(comp));
  };
  if (!hd.tiled) {
    for (int top = 0; top < h; ++top)
      for (int x = 0; x < w; ++x) put(im.images, size_t(top) * size_t(w) + size_t(x), x, top);
  } else {
    const int tx = hd.tile_size_x, ty = hd.tile_size_y;
    if (tx <= 0 || ty <= 0) return *out = DecodedImage{}, *error = "EXR has an invalid tile size", false;
    uint64_t covered = 0;
    for (int t = 0; t < im.num_tiles; ++t) {  // level 0 only; tile pixels are planar with the tile width as stride
      const EXRTile& tile = im.tiles[t];
      if (tile.level_x != 0 || tile.level_y != 0) continue;
      for (int j = 0; j < ty; ++j)
        for (int i = 0; i < tx; ++i) {
          const int x = tile.offset_x * tx + i, top = tile.offset_y * ty + j;
          if (x >= w || top >= h) continue;
          put(tile.images, size_t(j) * size_t(tx) + size_t(i), x, top);
          ++covered;
        }
    }
    if (covered != uint64_t(w) * uint64_t(h)) return *out = DecodedImage{}, *error = "EXR tile data is incomplete", false;
  }
  // Non-finite values are refused: they would poison the filter and the emission mean. The first
  // one in (row, column, channel) order from the bottom-left names the texel.
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (uint32_t k = 0; k < channels; ++k) {
        const uint8_t* p = out->texels.data() + ((size_t(y) * size_t(w) + size_t(x)) * channels + k) * size_t(comp);
        bool bad = false;
        if (any_float) {
          float f;
          std::memcpy(&f, p, 4);
          bad = float_not_finite(f);
        } else {
          uint16_t b;
          std::memcpy(&b, p, 2);
          bad = half_not_finite(b);
        }
        if (bad) {
          const char* name = hd.channels[kept[k]].name;
          *error = "EXR has a NaN or Inf in channel " + std::string(name) + " at texel (" + std::to_string(x) + ", " + std::to_string(y) +
                   "), y from the bottom";
          *out = DecodedImage{};
          return false;
        }
      }
  return true;
}

}  // namespace astroray::host

// SPDX-License-Identifier: Apache-2.0
// Image ingestion: 8-bit decode, row order, colour spaces, magenta outcomes with one report entry,
// the image mean and its use by the emission estimate (0007 d9, d13, d21; 0005 items 11, 34, 43).
// The test images are tests/data/images/*, written by generate.py from the pattern() below.
// Ref: docs/decisions/0007-blender-integration.md d9, d13, d21; 0005-material-model.md item 43.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "../../third_party/stb_image/stb_image.h"  // declarations only: the one implementation is src/host/image/decode.cpp
#include "host/image/decode.h"
#include "host/image/image_store.h"
#include "host/ledger.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "material/slot_resolve.h"
#include "vm/interp.h"

using namespace astroray::host;

namespace {

constexpr int W = 4, H = 3;

// Texel (x, y) channel c of the generated images; y = 0 is the bottom row. Same formula as
// tests/data/images/generate.py.
uint8_t pattern(int x, int y, int c) { return uint8_t(((x + 1) * 47 + (y + 1) * 83 + c * 61) % 256); }

std::string data_path(const std::string& name) {
  std::string here = __FILE__;
  const std::string dir = here.substr(0, here.find_last_of("/\\") + 1) + "../data/images/";
  if (std::ifstream(dir + name, std::ios::binary).good()) return dir + name;
  return "tests/data/images/" + name;
}

std::vector<uint8_t> read_file(const std::string& name) {
  std::ifstream f(data_path(name), std::ios::binary);
  REQUIRE_MESSAGE(f.good(), name << " not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

DecodedImage decode_ok(const std::vector<uint8_t>& bytes) {
  DecodedImage d;
  std::string err;
  REQUIRE_MESSAGE(decode_image_bytes(bytes.data(), bytes.size(), &d, &err), err);
  return d;
}

// An uncompressed 24-bit TGA of w x h texels given bottom row first; `top_left` stores the rows
// top first and sets the origin bit instead.
std::vector<uint8_t> tga(int w, int h, const std::vector<uint8_t>& rgb_bottom_up, bool top_left = false) {
  // id length, colour map type, image type 2, colour map spec (5), x origin (2), y origin (2), w (2), h (2), bpp, descriptor
  std::vector<uint8_t> t = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(w), 0, uint8_t(h), 0, 24, uint8_t(top_left ? 0x20 : 0)};
  for (int row = 0; row < h; ++row) {
    const int y = top_left ? h - 1 - row : row;
    for (int x = 0; x < w; ++x)
      for (int c = 2; c >= 0; --c) t.push_back(rgb_bottom_up[size_t((y * w + x) * 3 + c)]);
  }
  return t;
}

struct World {
  astroray::ContextState ctx;
  Scene scene{ctx, SceneConfig{1, {}}};
  std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
  std::shared_ptr<MaterialStore> store;
  World() {
    store = std::make_shared<MaterialStore>(ledger);
    scene.materials = store;
  }
  // Sets one image in its own generation and waits for it. Returns the generation number.
  uint64_t put(ar_key key, uint32_t source, const std::string& path, const std::vector<uint8_t>& bytes, uint32_t interop) {
    ar_image_desc d = {};
    d.key = key;
    d.source = source;
    d.color_interop = interop;
    d.path = path.c_str();
    d.bytes = bytes.data();
    d.byte_count = bytes.size();
    auto e = scene.begin_edit();
    REQUIRE(e);
    REQUIRE(store->set_image(*e, &d) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    store->end_edit();
    e.reset();
    scene.generations().wait_idle();
    return g;
  }
  uint64_t put_memory(ar_key key, const std::vector<uint8_t>& bytes, uint32_t interop) {
    return put(key, AR_IMAGE_SOURCE_MEMORY, "", bytes, uint32_t(interop));
  }
  uint64_t put_file(ar_key key, const std::string& name, uint32_t interop) {
    return put(key, AR_IMAGE_SOURCE_FILE, data_path(name), {}, uint32_t(interop));
  }
  // Stages a PIXELS image through image_begin, fills it and commits it in its own generation.
  uint64_t put_pixels(ar_key key, uint32_t w, uint32_t h, const std::vector<float>& rgba, uint32_t interop) {
    ar_image_desc d = {};
    d.key = key;
    d.source = AR_IMAGE_SOURCE_PIXELS;
    d.color_interop = interop;
    d.width = w, d.height = h;
    auto e = scene.begin_edit();
    REQUIRE(e);
    ar_image_buffers b = {};
    REQUIRE(store->image_begin(*e, &d, &b) == AR_OK);
    REQUIRE(b.pixels_count == uint64_t(w) * h * 4);
    REQUIRE(rgba.size() == b.pixels_count);
    std::memcpy(*b.pixels, rgba.data(), rgba.size() * sizeof(float));
    REQUIRE(e->item_commit(key) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    store->end_edit();
    e.reset();
    scene.generations().wait_idle();
    return g;
  }
  // The (code, detail) of every report entry whose subject is `key`.
  std::vector<std::pair<uint32_t, std::string>> entries(ar_key key) {
    scene.report().snapshot();
    ar_report_info info = {};
    REQUIRE(scene.report().info(&info) == AR_OK);
    std::vector<std::pair<uint32_t, std::string>> out;
    for (uint32_t k = 0; k < info.entry_count; ++k) {
      ar_report_entry e = {};
      REQUIRE(scene.report().entry(k, &e) == AR_OK);
      if (e.subject == key) out.emplace_back(e.code, reinterpret_cast<const char*>(e.detail));
    }
    return out;
  }
};

void check_magenta(const std::shared_ptr<const Image>& im) {
  REQUIRE(im);
  CHECK(im->magenta);
  CHECK(im->width == 1);
  CHECK(im->height == 1);
  CHECK(im->channels == 3);
  CHECK((im->texels == std::vector<uint8_t>{255, 0, 255}));
}

// The half and float patterns of the generated EXR files: same tables and index as
// tests/data/images/generate.py (half_bits(), float_value()).
const uint16_t kHalfBits[] = {0x0000, 0x00A8, 0x0001, 0x0400, 0x3400, 0x3800, 0x3C00, 0x4100, 0x3600, 0x63D0, 0x7BFF};
const float kFloatValues[] = {0.0f, 1e-5f, 0.25f, 1.0f, 65504.0f, 1e5f, 0.75f, 3.25f, 123456.789f};

size_t hdr_index(int x, int y, int c, size_t n) { return size_t(((x + 1) * 47 + (y + 1) * 83 + c * 61) % int(n)); }
uint16_t half_bits(int x, int y, int c) { return kHalfBits[hdr_index(x, y, c, std::size(kHalfBits))]; }
float float_value(int x, int y, int c) { return kFloatValues[hdr_index(x, y, c, std::size(kFloatValues))]; }

// Exact value of a finite binary16 in double, from its definition (not through the kernel).
double half_value(uint16_t b) {
  const int e = (b >> 10) & 31, m = b & 1023;
  const double v = e == 0 ? std::ldexp(double(m), -24) : std::ldexp(1.0 + m / 1024.0, e - 15);
  return (b & 0x8000) ? -v : v;
}

// True when every decoded component of `d` equals the generator's pattern bit for bit (channel k
// of the file is pattern channel `map[k]`), rows bottom-up.
bool half_matches(const DecodedImage& d, const std::vector<int>& map) {
  bool same = d.format == ar::TexFormat::HALF && d.texels.size() == size_t(W * H) * map.size() * 2;
  for (int y = 0; y < H && same; ++y)
    for (int x = 0; x < W; ++x)
      for (size_t k = 0; k < map.size(); ++k) {
        uint16_t b;
        std::memcpy(&b, d.texels.data() + ((size_t(y) * W + x) * map.size() + k) * 2, 2);
        same = same && b == half_bits(x, y, map[k]);
      }
  return same;
}

uint32_t bits_of(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
float from_bits(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
float next_up(float f) { return from_bits(bits_of(f) + 1); }  // positive finite f

// The float texels of an image, as bit patterns.
std::vector<uint32_t> texel_bits(const Image& im) {
  std::vector<uint32_t> v(im.texels.size() / 4);
  std::memcpy(v.data(), im.texels.data(), im.texels.size());
  return v;
}

// w x h RGBA floats, texel i channel c = base[(i * 4 + c) % base.size()].
std::vector<float> rgba_of(uint32_t w, uint32_t h, const std::vector<float>& base) {
  std::vector<float> v(size_t(w) * h * 4);
  for (size_t i = 0; i < v.size(); ++i) v[i] = base[i % base.size()];
  return v;
}

double srgb_eotf(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("image_ingest/decoded bytes png tga") {
  struct Case {
    const char* file;
    uint32_t channels;
  };
  for (const Case& cs : {Case{"pattern_4x3_rgb.png", 3}, Case{"pattern_4x3_gray.png", 1}, Case{"pattern_4x3_rgba.png", 4}}) {
    const DecodedImage d = decode_ok(read_file(cs.file));
    INFO(cs.file);
    REQUIRE(d.width == W);
    REQUIRE(d.height == H);
    REQUIRE(d.channels == cs.channels);  // kept as stored, not expanded
    REQUIRE(d.texels.size() == size_t(W * H) * cs.channels);
    bool same = true;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        for (uint32_t c = 0; c < cs.channels; ++c) same = same && d.texels[(size_t(y) * W + x) * cs.channels + c] == pattern(x, y, int(c));
    CHECK(same);
  }
  const DecodedImage t = decode_ok(read_file("pattern_4x3.tga"));
  REQUIRE(t.width == W);
  REQUIRE(t.height == H);
  REQUIRE(t.channels == 3);
  bool same = true;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      for (int c = 0; c < 3; ++c) same = same && t.texels[size_t((y * W + x) * 3 + c)] == pattern(x, y, c);
  CHECK(same);
}

TEST_CASE("image_ingest/jpeg matches stb") {
  // Quality 100, compared with stb's own decode of the same file, which is top row first.
  const std::vector<uint8_t> bytes = read_file("pattern_4x3.jpg");
  const DecodedImage d = decode_ok(bytes);
  int w = 0, h = 0, comp = 0;
  stbi_uc* ref = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp, 0);
  REQUIRE(ref != nullptr);
  REQUIRE(w == W);
  REQUIRE(h == H);
  REQUIRE(comp == 3);
  REQUIRE(d.channels == 3);
  bool same = true;
  for (int y = 0; y < H; ++y)
    for (int i = 0; i < W * 3; ++i) same = same && d.texels[size_t(y) * W * 3 + size_t(i)] == ref[size_t(H - 1 - y) * W * 3 + size_t(i)];
  stbi_image_free(ref);
  CHECK(same);
}

TEST_CASE("image_ingest/rows bottom up") {
  const DecodedImage png = decode_ok(read_file("pattern_4x3_rgb.png"));  // stored top row first
  for (int c = 0; c < 3; ++c) {
    CHECK(png.texels[size_t(c)] == pattern(0, 0, c));                              // first byte of row 0 is the bottom row
    CHECK(png.texels[size_t((2 * W) * 3 + c)] == pattern(0, 2, c));                // the last row is the top row
    CHECK(png.texels[size_t(c)] != png.texels[size_t((2 * W) * 3 + c)]);
  }
  // A TGA that stores its rows top first (origin bit) lands in the same order.
  std::vector<uint8_t> rgb;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      for (int c = 0; c < 3; ++c) rgb.push_back(pattern(x, y, c));
  CHECK(decode_ok(tga(W, H, rgb, true)).texels == rgb);
  CHECK(decode_ok(tga(W, H, rgb, false)).texels == rgb);
}

TEST_CASE("image_ingest/truncated png magenta") {
  World w;
  const uint64_t g = w.put_file(5, "truncated.png", AR_COLOR_INTEROP_SRGB_REC709);
  CHECK(w.scene.generations().active() == g);  // the generation still activates (0007 d9)
  CHECK(w.scene.generations().failed() == 0);
  check_magenta(w.store->images().find(5));
  const auto e = w.entries(5);
  REQUIRE(e.size() == 1);
  CHECK(e[0].first == AR_REPORT_IMAGE_UNREADABLE);
  CHECK(!e[0].second.empty());  // the decoder's reason
  // A missing file and an empty memory image are unreadable the same way.
  w.put(6, AR_IMAGE_SOURCE_FILE, data_path("no_such_image.png"), {}, AR_COLOR_INTEROP_LIN_REC709);
  w.put_memory(7, {}, AR_COLOR_INTEROP_LIN_REC709);
  check_magenta(w.store->images().find(6));
  check_magenta(w.store->images().find(7));
  CHECK(w.entries(6).size() == 1);
  CHECK(w.entries(7).size() == 1);
  CHECK(w.entries(5).size() == 1);
}

TEST_CASE("image_ingest/unsupported interop magenta") {
  World w;
  const std::vector<uint8_t> rgb = {10, 20, 30};
  const std::vector<uint8_t> bytes = tga(1, 1, rgb);
  w.put_memory(1, bytes, AR_COLOR_INTEROP_LIN_REC2020);
  w.put(2, AR_IMAGE_SOURCE_MEMORY, "", bytes, 77);
  for (ar_key k : {ar_key(1), ar_key(2)}) {
    check_magenta(w.store->images().find(k));
    REQUIRE(w.entries(k).size() == 1);  // exactly one, and no decode error on top of it
    CHECK(w.entries(k)[0].first == AR_REPORT_COLORSPACE_UNSUPPORTED);
  }
  CHECK(w.entries(1)[0].second == "lin_rec2020_scene");
  CHECK(w.entries(2)[0].second == "unknown interop id 77");
  // The ids that are decoded raise nothing.
  w.put_memory(3, bytes, AR_COLOR_INTEROP_SRGB_REC709);
  w.put_memory(4, bytes, AR_COLOR_INTEROP_DATA);
  CHECK(w.entries(3).empty());
  CHECK(w.entries(4).empty());
}

TEST_CASE("image_ingest/encoding tag and mean") {
  // 2x2: red alternates 0 and 255, green is 128, blue is 10, 20, 30, 40.
  const std::vector<uint8_t> rgb = {0, 128, 10, 255, 128, 20, 255, 128, 30, 0, 128, 40};
  const std::vector<uint8_t> bytes = tga(2, 2, rgb);
  World w;
  w.put_memory(1, bytes, AR_COLOR_INTEROP_SRGB_REC709);
  w.put_memory(2, bytes, AR_COLOR_INTEROP_LIN_REC709);
  w.put_memory(3, bytes, AR_COLOR_INTEROP_DATA);
  const auto s = w.store->images().find(1), l = w.store->images().find(2), d = w.store->images().find(3);
  REQUIRE((s && l && d));
  CHECK(s->encoding == ar::TexEncoding::SRGB);
  CHECK(l->encoding == ar::TexEncoding::LINEAR);
  CHECK(d->encoding == ar::TexEncoding::DATA);
  CHECK(s->texels == rgb);  // stored bytes, not decoded
  double srgb_b = 0.0;
  for (int v : {10, 20, 30, 40}) srgb_b += srgb_eotf(v / 255.0) / 4.0;
  CHECK(std::fabs(double(s->mean[0]) - 0.5) < 1e-6);
  CHECK(std::fabs(double(s->mean[1]) - srgb_eotf(128.0 / 255.0)) < 1e-6);
  CHECK(std::fabs(double(s->mean[1]) - 0.2158605) < 1e-6);
  CHECK(std::fabs(double(s->mean[2]) - srgb_b) < 1e-6);
  CHECK(std::fabs(double(l->mean[1]) - 128.0 / 255.0) < 1e-6);
  CHECK(std::fabs(double(l->mean[2]) - 25.0 / 255.0) < 1e-6);
  CHECK(std::fabs(double(d->mean[2]) - 25.0 / 255.0) < 1e-6);
}

TEST_CASE("image_ingest/file memory cache and ledger") {
  World w;
  const std::vector<uint8_t> bytes = read_file("pattern_4x3_rgb.png");
  w.put_file(1, "pattern_4x3_rgb.png", AR_COLOR_INTEROP_SRGB_REC709);
  w.put_memory(2, bytes, AR_COLOR_INTEROP_SRGB_REC709);
  const auto a = w.store->images().find(1), b = w.store->images().find(2);
  REQUIRE((a && b));
  CHECK(a->texels == b->texels);
  // The same file under the same key fields decodes once.
  w.put_file(3, "pattern_4x3_rgb.png", AR_COLOR_INTEROP_SRGB_REC709);
  CHECK(w.store->images().find(3).get() == a.get());
  // A different interop id is a different cache entry.
  w.put_file(4, "pattern_4x3_rgb.png", AR_COLOR_INTEROP_DATA);
  CHECK(w.store->images().find(4).get() != a.get());
  const auto arrays = w.store->images().arrays();
  REQUIRE(arrays->keys.size() == 4);
  for (size_t i = 0; i < arrays->keys.size(); ++i) {
    CHECK(arrays->views[i].width == W);
    CHECK(arrays->views[i].height == H);
    CHECK(arrays->views[i].channels == 3);
    CHECK(ar::image_texel(arrays->views[i], 1, 2)[0] == pattern(1, 2, 0));
  }
  // The ledger counts each distinct image once: keys 1 and 3 share one decode.
  std::set<const Image*> distinct;
  uint64_t total = 0;
  for (const auto& im : arrays->images)
    if (distinct.insert(im.get()).second) total += im->bytes();
  CHECK(distinct.size() == 3);
  CHECK(total == 3 * size_t(W * H * 3));
  CHECK(w.ledger->category_total(LedgerCategory::TEXTURES) == total);
}

TEST_CASE("image_ingest/mean of gray and rgba") {
  World w;
  w.put_file(1, "pattern_4x3_gray.png", AR_COLOR_INTEROP_DATA);
  w.put_file(2, "pattern_4x3_rgba.png", AR_COLOR_INTEROP_LIN_REC709);
  double want[3] = {0, 0, 0};
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      for (int c = 0; c < 3; ++c) want[c] += pattern(x, y, c) / 255.0 / (W * H);
  double grey = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) grey += pattern(x, y, 0) / 255.0 / (W * H);
  const auto g = w.store->images().find(1), a = w.store->images().find(2);
  REQUIRE((g && a));
  for (int c = 0; c < 3; ++c) {
    CHECK(std::fabs(double(g->mean[size_t(c)]) - grey) < 1e-6);  // grey counts as r = g = b
    CHECK(std::fabs(double(a->mean[size_t(c)]) - want[c]) < 1e-6);  // alpha is ignored
  }
}

TEST_CASE("image_ingest/rewritten file decodes again") {
  namespace fs = std::filesystem;
  const fs::path p = fs::temp_directory_path() / "astroray_060_rewrite.tga";
  auto write = [&](const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  };
  World w;
  write(tga(1, 1, {10, 20, 30}));
  w.put(1, AR_IMAGE_SOURCE_FILE, p.string(), {}, AR_COLOR_INTEROP_LIN_REC709);
  const auto first = w.store->images().find(1);
  REQUIRE(first);
  CHECK((first->texels == std::vector<uint8_t>{10, 20, 30}));
  write(tga(2, 1, {1, 2, 3, 4, 5, 6}));  // new size (and mtime): a different cache key
  w.put(1, AR_IMAGE_SOURCE_FILE, p.string(), {}, AR_COLOR_INTEROP_LIN_REC709);
  const auto second = w.store->images().find(1);
  REQUIRE(second);
  CHECK(second.get() != first.get());
  CHECK(second->width == 2);
  CHECK((second->texels == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
  std::error_code ec;
  fs::remove(p, ec);
}

TEST_CASE("image_ingest/memory cache compares bytes") {
  World w;
  w.put_memory(1, tga(1, 1, {10, 20, 30}), AR_COLOR_INTEROP_LIN_REC709);
  w.put_memory(2, tga(1, 1, {11, 20, 30}), AR_COLOR_INTEROP_LIN_REC709);
  CHECK(w.store->images().find(1)->texels[0] == 10);
  CHECK(w.store->images().find(2)->texels[0] == 11);
}

TEST_CASE("image_ingest/dropped generation keeps no image") {
  World w;
  auto e = w.scene.begin_edit();
  REQUIRE(e);
  ar_image_desc d = {};
  d.key = 8, d.source = AR_IMAGE_SOURCE_MEMORY, d.color_interop = AR_COLOR_INTEROP_LIN_REC2020;  // would raise an entry
  const std::vector<uint8_t> bytes = tga(1, 1, {1, 2, 3});
  d.bytes = bytes.data(), d.byte_count = bytes.size();
  REQUIRE(w.store->set_image(*e, &d) == AR_OK);
  auto bad = std::make_shared<EditItem>();
  bad->key = 99;
  bad->needs_device = false;
  bad->job = [] { throw std::runtime_error("corner_vert out of range"); };
  REQUIRE(e->stage(bad) == AR_OK);
  REQUIRE(e->item_commit(99) == AR_OK);
  uint64_t g = 0;
  REQUIRE(e->commit(&g) == AR_OK);
  w.store->end_edit();
  e.reset();
  w.scene.generations().wait_idle();
  CHECK(w.scene.generations().failed() == 1);
  CHECK(w.store->images().find(8) == nullptr);
  CHECK(w.entries(8).empty());
  CHECK(w.store->images().arrays()->keys.empty());
  CHECK(w.ledger->category_total(LedgerCategory::TEXTURES) == 0);
}

TEST_CASE("image_ingest/key must fit 32 bits") {
  World w;
  auto e = w.scene.begin_edit();
  REQUIRE(e);
  ar_image_desc d = {};
  d.key = (ar_key(1) << 32) | 5, d.source = AR_IMAGE_SOURCE_MEMORY, d.color_interop = AR_COLOR_INTEROP_LIN_REC709;
  const std::vector<uint8_t> bytes = tga(1, 1, {1, 2, 3});
  d.bytes = bytes.data(), d.byte_count = bytes.size();
  CHECK(w.store->set_image(*e, &d) != AR_OK);
  CHECK(e->last_error().find("32 bits") != std::string::npos);
  CHECK(e->abort() == AR_OK);
  w.store->end_edit();
}

TEST_CASE("image_ingest/emission estimate follows the image mean") {
  // uv -> TEX_IMAGE (image 5) -> EMIT_LE with strength 2, as one material.
  ar_graph_node n[4] = {};
  n[0].op = uint32_t(ar::Op::LOAD_CTX_UV);
  n[1].op = uint32_t(ar::Op::TEX_IMAGE);
  n[2].op = uint32_t(ar::Op::CONST_F32);
  n[3].op = uint32_t(ar::Op::EMIT_LE);
  n[0].i32_first = 0, n[0].i32_count = 1;
  n[1].i32_first = 1, n[1].i32_count = 2;
  n[2].f32_first = 0, n[2].f32_count = 1;
  const int32_t i32[3] = {0, 5, 0};
  const float f32[1] = {2.0f};
  const ar_graph_link links[3] = {{0, 0, 1, 0}, {1, 0, 3, 0}, {2, 0, 3, 1}};
  ar_graph_desc gd = {};
  gd.node_count = 4, gd.link_count = 3, gd.f32_count = 1, gd.i32_count = 3;
  gd.nodes = n, gd.links = links, gd.params_f32 = f32, gd.params_i32 = i32;
  ar_material_desc md = {};
  md.surface_graph = 10;

  World w;
  auto ref_of = [&](float r, float g, float b) {
    ar::SpectrumRef out = {};
    uint32_t flags = 0;
    ar::make_rgb_ref(w.store->table(), ar::Rgb{{ar::Real(r), ar::Real(g), ar::Real(b)}}, ar::kSlotSemantic[int(ar::Slot::EMISSION_COLOR)], &out, &flags);
    return out;
  };
  auto same = [](const ar::SpectrumRef& a, const ar::SpectrumRef& b) {
    return a.kind == b.kind && a.semantic == b.semantic && a.p[0] == b.p[0] && a.p[1] == b.p[1] && a.p[2] == b.p[2] && a.p[3] == b.p[3];
  };
  auto estimate = [&]() {
    const auto arrays = w.store->arrays();
    REQUIRE(arrays->emission_estimates.size() == 1);
    REQUIRE(arrays->emission_estimates[0].size() == 1);
    return arrays->emission_estimates[0][0];
  };
  auto commit = [&](const std::function<void(Edit&)>& body) {
    auto e = w.scene.begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    w.store->end_edit();
    e.reset();
    w.scene.generations().wait_idle();
  };
  auto set_image = [&](Edit& e, const std::vector<uint8_t>& rgb) {
    const std::vector<uint8_t> bytes = tga(2, 1, rgb);
    ar_image_desc d = {};
    d.key = 5, d.source = AR_IMAGE_SOURCE_MEMORY, d.color_interop = AR_COLOR_INTEROP_LIN_REC709;
    d.bytes = bytes.data(), d.byte_count = bytes.size();
    REQUIRE(w.store->set_image(e, &d) == AR_OK);
  };
  // The graph, the material and the image arrive in one edit: the estimate is the image's mean.
  commit([&](Edit& e) {
    REQUIRE(w.store->set_graph(e, 10, &gd) == AR_OK);
    REQUIRE(w.store->set_material(e, 20, &md) == AR_OK);
    set_image(e, {0, 128, 255, 255, 128, 0});
  });
  CHECK(same(estimate().color, ref_of(0.5f, float(128.0 / 255.0), 0.5f)));
  CHECK(estimate().strength == 2.0f);
  // Replacing only the image recompiles the material that read it.
  commit([&](Edit& e) { set_image(e, {51, 51, 51, 51, 51, 51}); });
  CHECK(same(estimate().color, ref_of(0.2f, 0.2f, 0.2f)));
  // An image that is not in the scene samples magenta, and the estimate says so.
  commit([&](Edit& e) {
    REQUIRE(e.remove(AR_KIND_IMAGE, 5) == AR_OK);
    REQUIRE(w.store->note_remove(e, AR_KIND_IMAGE, 5) == AR_OK);
  });
  CHECK(w.store->images().find(5) == nullptr);
  CHECK(same(estimate().color, ref_of(1.0f, 0.0f, 1.0f)));
}

TEST_CASE("image_ingest/exr half stored as half") {
  struct Case {
    const char* file;
    std::vector<int> map;  // pattern channel of each stored channel (R, G, B, A -> 0..3; Y -> 0)
  };
  for (const Case& cs : {Case{"pattern_4x3_half_rgba.exr", {0, 1, 2, 3}}, Case{"pattern_4x3_half_zip.exr", {0, 1, 2}},
                         Case{"pattern_4x3_half_tiled.exr", {0, 1, 2}}, Case{"pattern_4x3_half_y.exr", {0}},
                         Case{"pattern_4x3_half_ya.exr", {0, 0, 0, 3}}}) {  // Y+A keeps alpha: grey,grey,grey,alpha
    INFO(cs.file);
    const DecodedImage d = decode_ok(read_file(cs.file));
    REQUIRE(d.width == W);
    REQUIRE(d.height == H);
    REQUIRE(d.channels == cs.map.size());  // kept as stored, not expanded
    CHECK(half_matches(d, cs.map));        // subnormal, 65504 and the row order included
  }
}

TEST_CASE("image_ingest/exr float and hdr stored as float") {
  const DecodedImage f = decode_ok(read_file("pattern_4x3_float_rgb.exr"));
  REQUIRE(f.format == ar::TexFormat::FLOAT);
  REQUIRE(f.channels == 3);
  REQUIRE(f.texels.size() == size_t(W * H) * 3 * 4);
  bool same = true;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      for (int c = 0; c < 3; ++c) {
        uint32_t got, want;
        const float v = float_value(x, y, c);
        std::memcpy(&got, f.texels.data() + ((size_t(y) * W + x) * 3 + size_t(c)) * 4, 4);
        std::memcpy(&want, &v, 4);
        same = same && got == want;
      }
  CHECK(same);
  // Radiance RGBE as stb_image 2.32 decodes it (stbi__hdr_convert): (mantissa + 0.5) * 2^(e - 136),
  // exact in float; rows bottom-up.
  const DecodedImage r = decode_ok(read_file("pattern_4x3.hdr"));
  REQUIRE(r.format == ar::TexFormat::FLOAT);
  REQUIRE(r.channels == 3);
  REQUIRE((r.width == W && r.height == H));
  bool hdr_same = true;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      for (int c = 0; c < 3; ++c) {
        float got;
        std::memcpy(&got, r.texels.data() + ((size_t(y) * W + x) * 3 + size_t(c)) * 4, 4);
        const double want = std::ldexp(double(pattern(x, y, c)) + 0.5, 120 + (x + 2 * y) % 5 - 136);
        hdr_same = hdr_same && double(got) == want;
      }
  CHECK(hdr_same);
}

TEST_CASE("image_ingest/exr unreadable cases") {
  struct Case {
    const char* file;
    const char* detail;  // a substring of the one entry's detail
  };
  const Case cases[] = {{"bad_uint_channel.exr", "channel R is UINT"},
                        {"bad_no_green.exr", "missing channel G"},
                        {"bad_luma_chroma.exr", "luminance-chroma layout"},
                        {"bad_nan_half.exr", "NaN or Inf in channel G at texel (2, 1)"},
                        {"bad_inf_float.exr", "NaN or Inf in channel B at texel (3, 2)"},
                        {"bad_deep.exr", "deep EXR"},
                        {"bad_multipart.exr", "multipart EXR"},
                        {"bad_truncated.exr", "EXR data"},
                        {"bad_png16.png", "16-bit integer"}};
  World w;
  ar_key key = 1;
  for (const Case& cs : cases) {
    INFO(cs.file);
    for (int memory = 0; memory < 2; ++memory, ++key) {  // both source kinds: the file, then its bytes
      if (memory)
        w.put_memory(key, read_file(cs.file), AR_COLOR_INTEROP_LIN_REC709);
      else
        w.put_file(key, cs.file, AR_COLOR_INTEROP_LIN_REC709);
      check_magenta(w.store->images().find(key));
      const auto e = w.entries(key);
      REQUIRE(e.size() == 1);
      CHECK(e[0].first == AR_REPORT_IMAGE_UNREADABLE);
      CHECK_MESSAGE(e[0].second.find(cs.detail) != std::string::npos, e[0].second);
    }
  }
}

TEST_CASE("image_ingest/half and float are linear and count their bytes") {
  World w;
  const std::vector<uint8_t> hbytes = read_file("pattern_4x3_half_rgba.exr");
  w.put_file(1, "pattern_4x3_half_rgba.exr", AR_COLOR_INTEROP_SRGB_REC709);  // an sRGB id is read as linear
  w.put_memory(2, hbytes, AR_COLOR_INTEROP_LIN_REC709);
  w.put_file(3, "pattern_4x3_float_rgb.exr", AR_COLOR_INTEROP_DATA);
  w.put_file(4, "pattern_4x3.hdr", AR_COLOR_INTEROP_LIN_REC709);
  const auto a = w.store->images().find(1), b = w.store->images().find(2), f = w.store->images().find(3), r = w.store->images().find(4);
  REQUIRE((a && b && f && r));
  for (ar_key k : {ar_key(1), ar_key(2), ar_key(3), ar_key(4)}) CHECK(w.entries(k).empty());
  CHECK(a->encoding == ar::TexEncoding::LINEAR);
  CHECK(b->encoding == ar::TexEncoding::LINEAR);
  CHECK(f->encoding == ar::TexEncoding::DATA);
  CHECK(a->texels == b->texels);
  CHECK(a->format == ar::TexFormat::HALF);
  CHECK(f->format == ar::TexFormat::FLOAT);
  // Real bytes: width * height * channels * 2 and * 4.
  CHECK(a->bytes() == uint64_t(W * H * 4 * 2));
  CHECK(f->bytes() == uint64_t(W * H * 3 * 4));
  CHECK(r->bytes() == uint64_t(W * H * 3 * 4));
  const auto arrays = w.store->images().arrays();
  REQUIRE(arrays->keys.size() == 4);
  CHECK(arrays->views[0].format == uint32_t(ar::TexFormat::HALF));
  CHECK(arrays->views[2].format == uint32_t(ar::TexFormat::FLOAT));
  uint64_t total = 0;
  std::set<const Image*> distinct;
  for (const auto& im : arrays->images)
    if (distinct.insert(im.get()).second) total += im->bytes();
  CHECK(w.ledger->category_total(LedgerCategory::TEXTURES) == total);
  CHECK(total >= uint64_t(W * H * (4 * 2 + 3 * 4 + 3 * 4)));
  // The mean is of the stored linear values, no transfer function; alpha is ignored.
  double want[3] = {0, 0, 0}, wantf[3] = {0, 0, 0};
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      for (int c = 0; c < 3; ++c) {
        want[c] += half_value(half_bits(x, y, c)) / (W * H);
        wantf[c] += double(float_value(x, y, c)) / (W * H);
      }
  for (int c = 0; c < 3; ++c) {
    CHECK(std::fabs(double(a->mean[size_t(c)]) - want[c]) <= 1e-6 * std::fabs(want[c]) + 1e-9);
    CHECK(std::fabs(double(f->mean[size_t(c)]) - wantf[c]) <= 1e-6 * std::fabs(wantf[c]) + 1e-9);
  }
  // Y-only half: grey counts as r = g = b.
  w.put_file(5, "pattern_4x3_half_y.exr", AR_COLOR_INTEROP_LIN_REC709);
  double grey = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) grey += half_value(half_bits(x, y, 0)) / (W * H);
  const auto g = w.store->images().find(5);
  REQUIRE(g);
  for (int c = 0; c < 3; ++c) CHECK(std::fabs(double(g->mean[size_t(c)]) - grey) <= 1e-6 * std::fabs(grey) + 1e-9);
  // An interop id this build does not convert stays magenta with its own entry.
  w.put_file(6, "pattern_4x3_half_rgba.exr", AR_COLOR_INTEROP_LIN_REC2020);
  check_magenta(w.store->images().find(6));
  REQUIRE(w.entries(6).size() == 1);
  CHECK(w.entries(6)[0].first == AR_REPORT_COLORSPACE_UNSUPPORTED);
}

TEST_CASE("image_ingest/pixels bit exact") {
  World w;
  // Above 1, tiny, subnormal and a value no 8-bit image holds; alpha varies too.
  const std::vector<float> base = {50.0f, 0.5f, 0.25f, 1.0f, 1e-30f, 123456.789f, 3.0f, 0.5f, 0.0f, 1.4e-45f, 0.7f, 0.25f};
  const std::vector<float> in = rgba_of(3, 2, base);
  w.put_pixels(1, 3, 2, in, AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(2, 3, 2, in, AR_COLOR_INTEROP_DATA);
  w.put_pixels(3, 3, 2, in, AR_COLOR_INTEROP_SRGB_REC709);
  const auto lin = w.store->images().find(1), data = w.store->images().find(2), srgb = w.store->images().find(3);
  REQUIRE((lin && data && srgb));
  for (ar_key k : {ar_key(1), ar_key(2), ar_key(3)}) CHECK(w.entries(k).empty());
  CHECK((!lin->magenta && lin->width == 3 && lin->height == 2 && lin->channels == 4));
  CHECK(lin->format == ar::TexFormat::FLOAT);
  CHECK(lin->encoding == ar::TexEncoding::LINEAR);
  CHECK(data->encoding == ar::TexEncoding::DATA);
  CHECK(srgb->encoding == ar::TexEncoding::LINEAR);  // decoded at ingest, like a float image's id
  // Bit for bit, rows as staged (bottom-up).
  std::vector<uint32_t> want(in.size());
  for (size_t i = 0; i < in.size(); ++i) want[i] = bits_of(in[i]);
  CHECK(texel_bits(*lin) == want);
  CHECK(texel_bits(*data) == want);
  // sRGB: the kernel's own transfer function on R, G, B, the staged alpha untouched.
  std::vector<uint32_t> want_srgb(in.size());
  for (size_t i = 0; i < in.size(); ++i) want_srgb[i] = bits_of(i % 4 == 3 ? in[i] : ar::srgb_to_linear(in[i]));
  CHECK(texel_bits(*srgb) == want_srgb);
  // 8-bit codes in Blender's pixel form (k * (1/255)) decode to the byte path's table entries.
  std::vector<float> codes(256 * 4, 1.0f);
  for (int k = 0; k < 256; ++k)
    for (int c = 0; c < 3; ++c) codes[size_t(k) * 4 + size_t(c)] = float(k) * (1.0f / 255.0f);
  w.put_pixels(4, 256, 1, codes, AR_COLOR_INTEROP_SRGB_REC709);
  const auto dec = w.store->images().find(4);
  REQUIRE(dec);
  const auto arrays = w.store->images().arrays();
  for (int k = 0; k < 256; ++k)
    for (int c = 0; c < 3; ++c) {
      uint32_t got;
      std::memcpy(&got, dec->texels.data() + (size_t(k) * 4 + size_t(c)) * 4, 4);
      REQUIRE(got == bits_of(arrays->srgb_decode[size_t(k)]));
    }
  // The mean is of the stored linear values, alpha ignored.
  double m[3] = {0, 0, 0};
  for (size_t i = 0; i < 6; ++i)
    for (size_t c = 0; c < 3; ++c) m[c] += double(in[i * 4 + c]) / 6.0;
  for (size_t c = 0; c < 3; ++c) CHECK(std::fabs(double(lin->mean[c]) - m[c]) <= 1e-6 * std::fabs(m[c]) + 1e-12);
  // Four bytes a component, counted in the textures category with the other images.
  CHECK(lin->bytes() == uint64_t(3 * 2 * 4 * 4));
  uint64_t total = 0;
  std::set<const Image*> distinct;
  for (const auto& im : arrays->images)
    if (distinct.insert(im.get()).second) total += im->bytes();
  CHECK(w.ledger->category_total(LedgerCategory::TEXTURES) == total);
}

TEST_CASE("image_ingest/pixels descriptor refusals") {
  World w;
  const auto begin = [&](const ar_image_desc& d, bool expect_throw) {
    auto e = w.scene.begin_edit();
    REQUIRE(e);
    ar_image_buffers b = {};
    if (expect_throw)
      CHECK_THROWS_AS(w.store->image_begin(*e, &d, &b), std::invalid_argument);
    else
      CHECK(w.store->image_begin(*e, &d, &b) != AR_OK);
    CHECK(b.pixels == nullptr);
    e->abort();
    w.store->end_edit();
  };
  ar_image_desc d = {};
  d.key = 1, d.source = AR_IMAGE_SOURCE_PIXELS, d.width = 2, d.height = 2;
  ar_image_desc zero_w = d, zero_h = d, huge = d, file = d, big_key = d, bad_alpha = d;
  zero_w.width = 0;
  zero_h.height = 0;
  huge.width = huge.height = 0xFFFFFFFFu;  // width x height fits 64 bits, but its 16 bytes a texel do not
  file.source = AR_IMAGE_SOURCE_FILE;
  big_key.key = uint64_t(UINT32_MAX) + 1;
  bad_alpha.alpha_mode = AR_ALPHA_IGNORE + 1;
  begin(zero_w, true);
  begin(zero_h, true);
  begin(huge, true);
  begin(file, true);
  begin(bad_alpha, true);
  begin(big_key, false);
}

TEST_CASE("image_ingest/wide texel refused") {
  // The limit, stated again from the arithmetic: 2 |t| (1 + 8u) <= FLT_MAX, u = 2^-24, and it is the
  // largest float that does, so the next float is refused.
  const double u = std::ldexp(1.0, -24), fmax = double(std::numeric_limits<float>::max());
  const float lim = kMaxTexelMagnitude, over = next_up(lim);
  CHECK(bits_of(lim) == 0x7efffff7u);
  CHECK(2.0 * double(lim) * (1.0 + 8.0 * u) <= fmax);
  CHECK(2.0 * double(over) * (1.0 + 8.0 * u) > fmax);

  // The compiled Linear filter at the limit, all four texels equal, over many fractions: the sum
  // and the lift scale 2 * max stay finite (the bound's weights, products and additions).
  {
    const std::vector<float> texels(2 * 2 * 3, lim);
    ar::DeviceImageView v = {};
    v.texels = uint64_t(uintptr_t(texels.data()));
    v.width = v.height = 2, v.channels = 3, v.format = uint32_t(ar::TexFormat::FLOAT);
    double worst = 0;
    for (int i = 0; i < 20000; ++i) {
      const float x = 0.5f + float((i * 7919) % 10007) / 10007.0f, y = 0.5f + float((i * 104729) % 10009) / 10009.0f;
      float out[3];
      ar::tex_linear(v, x, y, false, out);
      for (float c : out) {
        REQUIRE(std::isfinite(2.0f * c));
        worst = std::max(worst, double(c));
      }
    }
    CHECK(2.0 * worst <= fmax);
  }

  World w;
  const std::vector<float> clean = rgba_of(4, 3, {0.25f, 0.5f, 1.0f, 1.0f});
  const auto with = [&](size_t index, float value) {
    std::vector<float> v = clean;
    v[index] = value;
    return v;
  };
  // Texel 6 of 12 (x = 2, y = 1), channel B: float index 6 * 4 + 2.
  const size_t at = 6 * 4 + 2;
  w.put_pixels(1, 4, 3, clean, AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(2, 4, 3, with(at, lim), AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(3, 4, 3, with(at, -lim), AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(4, 4, 3, with(at, over), AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(5, 4, 3, with(at, -over), AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(6, 4, 3, with(at, std::numeric_limits<float>::infinity()), AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(7, 4, 3, with(at, std::numeric_limits<float>::quiet_NaN()), AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(8, 4, 3, with(at, std::numeric_limits<float>::max()), AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(9, 4, 3, with(at, 1e30f), AR_COLOR_INTEROP_SRGB_REC709);  // the transfer overflows
  for (ar_key k : {ar_key(1), ar_key(2)}) {
    INFO("key " << k);
    REQUIRE(w.store->images().find(k));
    CHECK(!w.store->images().find(k)->magenta);
    CHECK(w.entries(k).empty());
  }
  CHECK(texel_bits(*w.store->images().find(2))[at] == bits_of(lim));
  for (ar_key k = 3; k <= 9; ++k) {  // -lim is refused as a negative texel, not by its magnitude
    INFO("key " << k);
    check_magenta(w.store->images().find(k));
    const auto e = w.entries(k);
    REQUIRE(e.size() == 1);
    CHECK(e[0].first == AR_REPORT_IMAGE_UNREADABLE);
    CHECK_MESSAGE(e[0].second.find("channel B at texel (2, 1), y from the bottom (index 6)") != std::string::npos, e[0].second);
  }
  CHECK(w.entries(3)[0].second.find("negative value -1.70141092e+38") != std::string::npos);
  CHECK(w.entries(4)[0].second.find("above the limit 1.70141092e+38") != std::string::npos);
  CHECK(w.entries(4)[0].second.find("value 1.70141102e+38") != std::string::npos);
  CHECK(w.entries(6)[0].second.find("NaN or Inf") != std::string::npos);
  CHECK(w.entries(7)[0].second.find("NaN or Inf") != std::string::npos);

  // The clean image beside the bad ones has no entry of its own.
  CHECK(w.entries(1).empty());

  // FILE and MEMORY images follow the same rule.
  w.put_file(20, "ok_wide_float.exr", AR_COLOR_INTEROP_LIN_REC709);
  w.put_memory(21, read_file("ok_wide_float.exr"), AR_COLOR_INTEROP_LIN_REC709);
  w.put_file(22, "bad_wide_float.exr", AR_COLOR_INTEROP_LIN_REC709);
  w.put_memory(23, read_file("bad_wide_float.exr"), AR_COLOR_INTEROP_LIN_REC709);
  for (ar_key k : {ar_key(20), ar_key(21)}) {
    INFO("key " << k);
    REQUIRE(w.store->images().find(k));
    CHECK(!w.store->images().find(k)->magenta);
    CHECK(w.entries(k).empty());
  }
  for (ar_key k : {ar_key(22), ar_key(23)}) {
    INFO("key " << k);
    check_magenta(w.store->images().find(k));
    const auto e = w.entries(k);
    REQUIRE(e.size() == 1);
    CHECK(e[0].first == AR_REPORT_IMAGE_UNREADABLE);
    CHECK_MESSAGE(e[0].second.find("channel B at texel (3, 2)") != std::string::npos, e[0].second);
    CHECK_MESSAGE(e[0].second.find("above the limit") != std::string::npos, e[0].second);
  }
}

TEST_CASE("image_ingest/negative texels are refused") {
  // A negative half or float texel in any channel, from any source, renders the image magenta with
  // one IMAGE_UNREADABLE entry naming it; -0 and small positive values are fine (0007 d13).
  World w;
  const std::vector<float> clean = rgba_of(4, 3, {0.25f, 0.5f, 1.0f, 1.0f});
  const auto with = [&](size_t index, float value) {
    std::vector<float> v = clean;
    v[index] = value;
    return v;
  };
  const size_t at = 6 * 4 + 2;  // texel 6 (x = 2, y = 1), channel B
  w.put_pixels(1, 4, 3, with(at, -0.0f), AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(2, 4, 3, with(at, 1e-30f), AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(3, 4, 3, with(at, -0.375f), AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(4, 4, 3, with(at, -1e-30f), AR_COLOR_INTEROP_LIN_REC709);
  w.put_pixels(5, 4, 3, with(6 * 4 + 3, -0.5f), AR_COLOR_INTEROP_LIN_REC709);  // alpha
  w.put_pixels(6, 4, 3, with(at, -0.5f), AR_COLOR_INTEROP_SRGB_REC709);        // the transfer keeps the sign
  for (ar_key k : {ar_key(1), ar_key(2)}) {
    INFO("key " << k);
    REQUIRE(w.store->images().find(k));
    CHECK(!w.store->images().find(k)->magenta);
    CHECK(w.entries(k).empty());
  }
  for (ar_key k = 3; k <= 6; ++k) {
    INFO("key " << k);
    check_magenta(w.store->images().find(k));
    const auto e = w.entries(k);
    REQUIRE(e.size() == 1);
    CHECK(e[0].first == AR_REPORT_IMAGE_UNREADABLE);
    CHECK_MESSAGE(e[0].second.find("negative value") != std::string::npos, e[0].second);
  }
  CHECK(w.entries(3)[0].second.find("negative value -0.375 in channel B at texel (2, 1)") != std::string::npos);
  CHECK(w.entries(5)[0].second.find("channel A") != std::string::npos);

  // EXR files, half and float, FILE and MEMORY.
  w.put_file(10, "bad_negative_half.exr", AR_COLOR_INTEROP_LIN_REC709);
  w.put_memory(11, read_file("bad_negative_half.exr"), AR_COLOR_INTEROP_LIN_REC709);
  w.put_file(12, "bad_negative_float.exr", AR_COLOR_INTEROP_LIN_REC709);
  w.put_memory(13, read_file("bad_negative_float.exr"), AR_COLOR_INTEROP_LIN_REC709);
  for (ar_key k = 10; k <= 13; ++k) {
    INFO("key " << k);
    check_magenta(w.store->images().find(k));
    const auto e = w.entries(k);
    REQUIRE(e.size() == 1);
    CHECK(e[0].first == AR_REPORT_IMAGE_UNREADABLE);
    CHECK_MESSAGE(e[0].second.find("negative value") != std::string::npos, e[0].second);
  }
  CHECK(w.entries(10)[0].second.find("-0.375 in channel G at texel (2, 1)") != std::string::npos);
  CHECK(w.entries(12)[0].second.find("-0.75 in channel B at texel (3, 2)") != std::string::npos);
}

TEST_CASE("image_ingest/blender exr equals its pixels") {
  // Written by Blender (generate.py) with the float RGBA it reads back from the file: the MEMORY
  // decode, and the FILE one, must hold exactly those values.
  World w;
  const std::vector<uint8_t> px_half = read_file("blender_half_rgba.pixels"), px_float = read_file("blender_float_rgba.pixels");
  REQUIRE(px_half.size() == size_t(W * H * 4 * 4));
  REQUIRE(px_float.size() == px_half.size());
  w.put_memory(1, read_file("blender_half_rgba.exr"), AR_COLOR_INTEROP_LIN_REC709);
  w.put_memory(2, read_file("blender_float_rgba.exr"), AR_COLOR_INTEROP_LIN_REC709);
  w.put_file(3, "blender_float_rgba.exr", AR_COLOR_INTEROP_LIN_REC709);
  const auto h = w.store->images().find(1), f = w.store->images().find(2), ff = w.store->images().find(3);
  REQUIRE((h && f && ff));
  for (ar_key k : {ar_key(1), ar_key(2), ar_key(3)}) CHECK(w.entries(k).empty());
  REQUIRE(h->format == ar::TexFormat::HALF);
  REQUIRE(h->channels == 4);
  REQUIRE(h->texels.size() == size_t(W * H * 4 * 2));
  bool same = true;
  for (size_t i = 0; i < size_t(W * H * 4) && same; ++i) {
    uint16_t b;
    std::memcpy(&b, h->texels.data() + i * 2, 2);
    uint32_t want;
    std::memcpy(&want, px_half.data() + i * 4, 4);
    same = bits_of(ar::half_to_float(b)) == want;
  }
  CHECK(same);
  CHECK(f->format == ar::TexFormat::FLOAT);
  CHECK(f->texels == px_float);
  CHECK(ff->texels == px_float);
}

}  // TEST_SUITE

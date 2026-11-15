// SPDX-License-Identifier: Apache-2.0
// TEX_IMAGE (0005 item 34): Closest and Linear at level 0 with Repeat and Extend on 8-bit images,
// texel centres at (i + 0.5) / size, filtering on the stored bytes and the sRGB decode after it,
// as Cycles does; grey and RGBA expansion; and the image key to slot resolution of the store: a
// key that is not set samples magenta with one entry, and setting it recompiles the material.
// Expected values are computed here independently: the IEC 61966-2-1 decode in double of the
// encoded value, and the texel each wrap rule names.
// Ref: docs/decisions/0005-material-model.md item 34; Cycles intern/cycles/kernel/util/image.h
// (interp_closest, interp_linear, wrap_periodic, wrap_clamp; Apache-2.0); IEC 61966-2-1:1999.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "host/image/image_store.h"
#include "host/ledger.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "vm/interp.h"

using namespace astroray::host;

namespace {

constexpr uint32_t kClosest = 0u, kLinear = ar::kTexLinear, kRepeat = 0u, kExtend = ar::kTexExtend;

double iec_decode(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

// A 4x4 three-channel image; texel (x, y) channel c, y = 0 the bottom row. Row 0 holds 0 and 255
// side by side, where the encoded and the decoded average differ the most.
struct Img4 {
  std::vector<uint8_t> t;
  ar::DeviceImageView view = {};
  float srgb[ar::kSrgbTableSize];
  ar::SceneTables tables = {};
  explicit Img4(ar::TexEncoding enc) : t(4 * 4 * 3) {
    ar::srgb_decode_table(srgb);
    tables.srgb_decode = srgb;
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        for (int c = 0; c < 3; ++c) t[size_t((y * 4 + x) * 3 + c)] = uint8_t((x * 61 + y * 23 + c * 97 + 5) % 256);
    t[0] = t[1] = t[2] = 0;
    t[3] = t[4] = t[5] = 255;
    view.texels = uint64_t(uintptr_t(t.data()));
    view.width = view.height = 4;
    view.channels = 3;
    view.encoding = uint32_t(enc);
    tables.images = &view;
    tables.image_count = 1;
  }
  uint8_t at(int x, int y, int c) const { return t[size_t((y * 4 + x) * 3 + c)]; }
};

std::vector<float> sample(const ar::SceneTables& tb, float u, float v, uint32_t mode, uint32_t slot = 0) {
  const float uv[3] = {u, v, 0.0f};
  float out[3];
  ar::op_tex_image(tb, slot, mode, uv, out);
  return {out[0], out[1], out[2]};
}

// The decoded value of encoded value e (bytes / 255, or their average).
double decoded(double e, ar::TexEncoding enc) { return enc == ar::TexEncoding::SRGB ? iec_decode(e) : e; }

bool near(const std::vector<float>& got, const double want[3], double tol) {
  for (int c = 0; c < 3; ++c)
    if (!(std::fabs(double(got[size_t(c)]) - want[c]) <= tol)) return false;
  return true;
}

uint32_t bits(float f) { return ar::bit_cast<uint32_t>(f); }

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("tex_image/texel centres are exact in both modes") {
  for (ar::TexEncoding enc : {ar::TexEncoding::SRGB, ar::TexEncoding::LINEAR, ar::TexEncoding::DATA}) {
    const Img4 im(enc);
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) {
        const float u = (float(x) + 0.5f) / 4.0f, v = (float(y) + 0.5f) / 4.0f;
        const std::vector<float> c = sample(im.tables, u, v, kClosest), l = sample(im.tables, u, v, kLinear | kExtend);
        double want[3];
        for (int k = 0; k < 3; ++k) want[k] = decoded(im.at(x, y, k) / 255.0, enc);
        INFO("encoding " << uint32_t(enc) << " texel " << x << "," << y);
        CHECK(near(c, want, 1e-6));
        for (int k = 0; k < 3; ++k) CHECK(bits(c[size_t(k)]) == bits(l[size_t(k)]));
      }
  }
}

TEST_CASE("tex_image/filter in encoded space") {
  const Img4 im(ar::TexEncoding::SRGB);
  // Halfway between the centres of texels (0, 0) = 0 and (1, 0) = 255: u = 1/4.
  const std::vector<float> got = sample(im.tables, 0.25f, 0.125f, kLinear);
  const double encoded_avg = iec_decode(0.5), decoded_avg = 0.5 * (iec_decode(0.0) + iec_decode(1.0));
  REQUIRE(std::fabs(encoded_avg - decoded_avg) > 0.25);  // the two orders are far apart here
  const double want[3] = {encoded_avg, encoded_avg, encoded_avg};
  CHECK(near(got, want, 1e-6));
  // A quarter of the way, on a pair of ordinary texels in both directions.
  const std::vector<float> q = sample(im.tables, 0.5f, 0.4375f, kLinear);  // x = 2 -> between 1 and 2; y = 1.75 -> 1 and 2
  double wq[3];
  for (int c = 0; c < 3; ++c) {
    const double e = 0.5 * 0.75 * im.at(1, 1, c) + 0.5 * 0.75 * im.at(2, 1, c) + 0.5 * 0.25 * im.at(1, 2, c) +
                     0.5 * 0.25 * im.at(2, 2, c);
    wq[c] = iec_decode(e / 255.0);
  }
  CHECK(near(q, wq, 1e-6));
}

TEST_CASE("tex_image/repeat wraps") {
  const Img4 im(ar::TexEncoding::SRGB);
  const float v = 0.625f;  // the centre of row 2
  double want[3];
  // u = -0.25: x = -1. Cycles' frac takes int(x) - 1 for every negative x, so a negative integer
  // lands one column further left: -2, which repeats to column 2 (Blender's own value there).
  // u = 1.25: x = 5 -> column 1.
  for (int c = 0; c < 3; ++c) want[c] = iec_decode(im.at(2, 2, c) / 255.0);
  CHECK(near(sample(im.tables, -0.25f, v, kClosest | kRepeat), want, 1e-6));
  for (int c = 0; c < 3; ++c) want[c] = iec_decode(im.at(1, 2, c) / 255.0);
  CHECK(near(sample(im.tables, 1.25f, v, kClosest | kRepeat), want, 1e-6));
  // Linear: x - 0.5 = -1.5 averages columns -2 and -1, which are 2 and 3; x - 0.5 = 4.5
  // averages columns 4 and 5, which are 0 and 1.
  for (int c = 0; c < 3; ++c) want[c] = iec_decode(0.5 * (im.at(2, 2, c) + im.at(3, 2, c)) / 255.0);
  CHECK(near(sample(im.tables, -0.25f, v, kLinear | kRepeat), want, 1e-6));
  for (int c = 0; c < 3; ++c) want[c] = iec_decode(0.5 * (im.at(0, 2, c) + im.at(1, 2, c)) / 255.0);
  CHECK(near(sample(im.tables, 1.25f, v, kLinear | kRepeat), want, 1e-6));
}

TEST_CASE("tex_image/extend clamps") {
  const Img4 im(ar::TexEncoding::SRGB);
  const float v = 0.625f;
  double first[3], last[3];
  for (int c = 0; c < 3; ++c) {
    first[c] = iec_decode(im.at(0, 2, c) / 255.0);
    last[c] = iec_decode(im.at(3, 2, c) / 255.0);
  }
  for (uint32_t interp : {kClosest, kLinear}) {
    INFO("interpolation " << interp);
    CHECK(near(sample(im.tables, -0.25f, v, interp | kExtend), first, 1e-6));
    CHECK(near(sample(im.tables, 1.25f, v, interp | kExtend), last, 1e-6));
  }
  // Both axes at once, below and above: the corner texels.
  double corner[3];
  for (int c = 0; c < 3; ++c) corner[c] = iec_decode(im.at(3, 0, c) / 255.0);
  CHECK(near(sample(im.tables, 7.0f, -3.0f, kLinear | kExtend), corner, 1e-6));
}

TEST_CASE("tex_image/grey and rgba expand, missing slot is magenta") {
  const std::vector<uint8_t> grey = {0, 51, 102, 255};
  const std::vector<uint8_t> rgba = {10, 20, 30, 40};
  ar::DeviceImageView views[2] = {};
  views[0].texels = uint64_t(uintptr_t(grey.data()));
  views[0].width = 2, views[0].height = 2, views[0].channels = 1, views[0].encoding = uint32_t(ar::TexEncoding::DATA);
  views[1].texels = uint64_t(uintptr_t(rgba.data()));
  views[1].width = 1, views[1].height = 1, views[1].channels = 4, views[1].encoding = uint32_t(ar::TexEncoding::LINEAR);
  float srgb[ar::kSrgbTableSize];
  ar::srgb_decode_table(srgb);
  ar::SceneTables tb = {};
  tb.srgb_decode = srgb;
  tb.images = views;
  tb.image_count = 2;
  const float k = 1.0f / 255.0f;
  const std::vector<float> g = sample(tb, 0.25f, 0.75f, kClosest, 0);  // texel (0, 1) = 102
  CHECK((g[0] == 102.0f * k && g[1] == 102.0f * k && g[2] == 102.0f * k));
  const ar::Texel t = ar::load_texel(grey.data() + 1, 1);
  CHECK((t.v[0] == 51.0f * k && t.v[3] == 1.0f));
  const ar::Texel a = ar::load_texel(rgba.data(), 4);
  CHECK((a.v[0] == 10.0f * k && a.v[1] == 20.0f * k && a.v[2] == 30.0f * k && a.v[3] == 40.0f * k));
  const ar::Texel r3 = ar::load_texel(rgba.data(), 3);
  CHECK(r3.v[3] == 1.0f);
  // The centre of a 2x2 grey image weighs all four texels by 1/4; data images are not decoded.
  const double mean4 = 0.25 * (0 + 51 + 102 + 255) / 255.0;
  const double want4[3] = {mean4, mean4, mean4};
  CHECK(near(sample(tb, 0.5f, 0.5f, kLinear, 0), want4, 1e-6));
  // The same grey bytes tagged sRGB: a Closest lookup decodes the one byte through the table into
  // all three channels (texel (1, 0) = 51; its neighbours 102 and 255 must not leak in).
  views[0].encoding = uint32_t(ar::TexEncoding::SRGB);
  const std::vector<float> gs = sample(tb, 0.75f, 0.25f, kClosest, 0);
  CHECK((bits(gs[0]) == bits(srgb[51]) && bits(gs[1]) == bits(srgb[51]) && bits(gs[2]) == bits(srgb[51])));
  CHECK(std::fabs(double(gs[0]) - iec_decode(51.0 / 255.0)) < 1e-6);
  views[0].encoding = uint32_t(ar::TexEncoding::DATA);
  for (uint32_t slot : {2u, ar::kTexNoImage}) {
    const std::vector<float> m = sample(tb, 0.5f, 0.5f, kLinear, slot);
    CHECK((m[0] == 1.0f && m[1] == 0.0f && m[2] == 1.0f));
  }
}

TEST_CASE("tex_image/missing key reports and recompiles when set") {
  astroray::ContextState ctx;
  Scene scene{ctx, SceneConfig{1, {}}};
  auto ledger = std::make_shared<Ledger>();
  auto store = std::make_shared<MaterialStore>(ledger);
  scene.materials = store;
  auto commit = [&](const std::function<void(Edit&)>& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t gen = 0;
    REQUIRE(e->commit(&gen) == AR_OK);
    store->end_edit();
    e.reset();
    scene.generations().wait_idle();
  };
  auto set_image = [&](Edit& e, ar_key key, uint8_t value) {
    // A 1x1 uncompressed 24-bit TGA (image type 2) of grey `value`.
    const std::vector<uint8_t> tga = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 24, 0, value, value, value};
    ar_image_desc d = {};
    d.key = key, d.source = AR_IMAGE_SOURCE_MEMORY, d.color_interop = AR_COLOR_INTEROP_LIN_REC709;
    d.bytes = tga.data(), d.byte_count = tga.size();
    REQUIRE(store->set_image(e, &d) == AR_OK);
  };
  auto entries = [&](ar_key key) {
    scene.report().snapshot();
    ar_report_info info = {};
    REQUIRE(scene.report().info(&info) == AR_OK);
    std::vector<std::pair<uint32_t, std::string>> out;
    for (uint32_t i = 0; i < info.entry_count; ++i) {
      ar_report_entry e = {};
      REQUIRE(scene.report().entry(i, &e) == AR_OK);
      if (e.subject == key) out.emplace_back(e.code, reinterpret_cast<const char*>(e.detail));
    }
    return out;
  };
  // The slot immediate of the material's one TEX_IMAGE (uv -> TEX_IMAGE key 5 -> base_color).
  auto program_slot = [&]() {
    const auto a = store->arrays();
    REQUIRE(a->headers.size() == 2);  // the material, then the default material
    const uint32_t* p = a->programs.data() + a->headers[0].program_offset;
    const uint32_t entry = p[0];
    REQUIRE((p[entry] & 0xffu) == uint32_t(ar::Op::LOAD_CTX_UV));
    REQUIRE((p[entry + 1] & 0xffu) == uint32_t(ar::Op::TEX_IMAGE));
    CHECK(p[entry + 3] == (ar::kTexLinear | ar::kTexExtend));
    return p[entry + 2];
  };
  ar_graph_node n[3] = {};
  n[0].op = uint32_t(ar::Op::LOAD_CTX_UV), n[0].i32_first = 0, n[0].i32_count = 1;
  n[1].op = uint32_t(ar::Op::TEX_IMAGE), n[1].i32_first = 1, n[1].i32_count = 2;
  n[2].op = uint32_t(ar::Op::STORE_SLOT), n[2].i32_first = 3, n[2].i32_count = 1;
  const int32_t i32[4] = {0, 5, int32_t(ar::kTexLinear | ar::kTexExtend), int32_t(ar::RecordSlot::base_color)};
  const ar_graph_link links[2] = {{0, 0, 1, 0}, {1, 0, 2, 0}};
  ar_graph_desc gd = {};
  gd.node_count = 3, gd.link_count = 2, gd.i32_count = 4, gd.nodes = n, gd.links = links, gd.params_i32 = i32;
  ar_material_desc md = {};
  md.surface_graph = 10;

  // Image 5 is not set: the lookup is magenta and the material has one entry.
  commit([&](Edit& e) {
    REQUIRE(store->set_graph(e, 10, &gd) == AR_OK);
    REQUIRE(store->set_material(e, 20, &md) == AR_OK);
  });
  CHECK(program_slot() == ar::kTexNoImage);
  const auto missing = entries(20);
  REQUIRE(missing.size() == 1);
  CHECK(missing[0].first == AR_REPORT_IMAGE_UNREADABLE);
  CHECK(missing[0].second == "image key 5 is not set");
  // Another image takes slot 0; setting 5 alone recompiles the material, which now holds 5's slot.
  commit([&](Edit& e) { set_image(e, 6, 10); });
  CHECK(program_slot() == ar::kTexNoImage);
  commit([&](Edit& e) { set_image(e, 5, 200); });
  uint32_t s5 = 99, s6 = 99;
  REQUIRE(store->images().slot(5, &s5));
  REQUIRE(store->images().slot(6, &s6));
  CHECK(s6 == 0u);
  CHECK(s5 == 1u);
  CHECK(program_slot() == s5);
  CHECK(entries(20).empty());
  // The slot is stable across a replacement of the image, and the table entry is the image.
  commit([&](Edit& e) { set_image(e, 5, 100); });
  CHECK(program_slot() == s5);
  {
    const auto a = store->images().arrays();
    REQUIRE(a->views.size() == 2);
    CHECK(*ar::image_texel(a->views[s5], 0, 0) == 100);
  }
  // Removing 5 frees its slot (1x1 magenta in the table) and the material reports it again.
  commit([&](Edit& e) {
    REQUIRE(e.remove(AR_KIND_IMAGE, 5) == AR_OK);
    REQUIRE(store->note_remove(e, AR_KIND_IMAGE, 5) == AR_OK);
  });
  CHECK(program_slot() == ar::kTexNoImage);
  CHECK(entries(20).size() == 1);
  {
    const auto a = store->images().arrays();
    REQUIRE(a->views.size() == 2);
    CHECK(a->keys[s5] == 0u);
    const ar::DeviceImageView& m = a->views[s5];
    CHECK((m.width == 1 && m.height == 1 && m.channels == 3));
    const uint8_t* px = ar::image_texel(m, 0, 0);
    CHECK((px[0] == 255 && px[1] == 0 && px[2] == 255));
  }
  // A new key takes the freed slot: the table stays dense.
  commit([&](Edit& e) { set_image(e, 7, 1); });
  uint32_t s7 = 99;
  REQUIRE(store->images().slot(7, &s7));
  CHECK(s7 == s5);
}

}  // TEST_SUITE

// SPDX-License-Identifier: Apache-2.0
// TEX_IMAGE agreement (0005 Verification 6, 0009 d17 step ii): 10^5 lookups, every mode, on an
// sRGB, a linear grey and a data RGBA image plus a slot outside the table, run on the GPU built
// without FMA contraction and on the CPU (float). Closest must be bit-identical (the first 256
// lookups hit every byte value of the sRGB image at its texel centre, so the sRGB decode is
// compared on its whole Closest domain); Linear within 1e-6 relative.
// Ref: docs/decisions/0005-material-model.md item 34, Verification 6; docs/decisions/0002-kernel-
// architecture.md D12, D13; 0009 d17.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "agree.h"
#include "host/device.h"
#include "vm/interp.h"

using namespace astroray;
using namespace astroray::agree;
using astroray::host::DeviceBuffer;
using astroray::host::Ledger;
using astroray::host::LedgerCategory;
using astroray::host::Owner;

namespace {

constexpr uint32_t kN = 100000;
constexpr uint32_t kIn = 4, kOut = 3;
constexpr double kMaxRel = 1e-6;  // Verification 6: GPU no-FMA within 1e-6 relative of CPU float

std::string nofma_module(const char* name) { return std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/" + name + ".cubin"; }

struct HostImage {
  uint32_t w, h, ch;
  ar::TexEncoding enc;
  std::vector<uint8_t> t;
};

// 16x16 sRGB RGB whose red channel takes every byte value once; 5x3 linear grey; 3x2 data RGBA.
std::vector<HostImage> images() {
  std::vector<HostImage> im(3);
  im[0] = {16, 16, 3, ar::TexEncoding::SRGB, {}};
  for (uint32_t i = 0; i < 256; ++i) im[0].t.insert(im[0].t.end(), {uint8_t(i), uint8_t((i * 7 + 3) & 255u), uint8_t(255 - i)});
  im[1] = {5, 3, 1, ar::TexEncoding::LINEAR, {}};
  for (uint32_t i = 0; i < 15; ++i) im[1].t.push_back(uint8_t((i * 53 + 11) & 255u));
  im[2] = {3, 2, 4, ar::TexEncoding::DATA, {}};
  for (uint32_t i = 0; i < 24; ++i) im[2].t.push_back(uint8_t((i * 89 + 7) & 255u));
  return im;
}

ar::DeviceImageView view_of(const HostImage& h, uint64_t texels) {
  ar::DeviceImageView v = {};
  v.texels = texels;
  v.width = h.w, v.height = h.h, v.channels = h.ch, v.encoding = uint32_t(h.enc);
  return v;
}

// Rows 0-255: the centre of sRGB texel i in Closest. The rest: u, v in [-1.5, 2.5), slot 0-3
// (3 is outside the table), mode 0-3.
Inputs make_inputs() {
  Inputs in;
  in.n = kN;
  in.stride = kIn;
  in.v.resize(size_t(kN) * kIn);
  for (uint32_t i = 0; i < kN; ++i) {
    float* r = in.v.data() + size_t(i) * kIn;
    if (i < 256) {
      r[0] = (float(i % 16) + 0.5f) / 16.0f, r[1] = (float(i / 16) + 0.5f) / 16.0f, r[2] = 0.0f, r[3] = 0.0f;
      continue;
    }
    r[0] = float(unit(i, 0) * 4.0 - 1.5);
    r[1] = float(unit(i, 1) * 4.0 - 1.5);
    r[2] = float(std::floor(unit(i, 2) * 4.0));
    r[3] = float(std::floor(unit(i, 3) * 4.0));
  }
  return in;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("tex_image_agree/gpu nofma vs cpu float") {
  const std::vector<HostImage> hi = images();
  const Inputs in = make_inputs();
  Ledger ledger;
  auto dev = host::make_cuda_device(ledger, 0);

  std::vector<DeviceBuffer> texels;
  std::vector<ar::DeviceImageView> dviews, hviews;
  for (const HostImage& h : hi) {
    texels.push_back(dev->allocate(LedgerCategory::TEXTURES, h.t.size(), Owner{}));
    dev->upload(texels.back(), 0, h.t.data(), h.t.size());
    dviews.push_back(view_of(h, reinterpret_cast<uint64_t>(texels.back().ptr)));
    hviews.push_back(view_of(h, uint64_t(uintptr_t(h.t.data()))));
  }
  const uint64_t vbytes = sizeof(ar::DeviceImageView) * dviews.size();
  DeviceBuffer vb = dev->allocate(LedgerCategory::TABLES, vbytes, Owner{});
  dev->upload(vb, 0, dviews.data(), vbytes);
  // The decode table is built once on the host and read by both targets (color/srgb.h).
  float srgb[ar::kSrgbTableSize];
  ar::srgb_decode_table(srgb);
  DeviceBuffer sb = dev->allocate(LedgerCategory::TABLES, sizeof srgb, Owner{});
  dev->upload(sb, 0, srgb, sizeof srgb);
  uint64_t views_p = reinterpret_cast<uint64_t>(vb.ptr), srgb_p = reinterpret_cast<uint64_t>(sb.ptr);
  uint32_t count = uint32_t(dviews.size());
  const Results gpu = run_gpu(*dev, nofma_module("tex_agree"), "ar_tex_agree", in, kOut, {&views_p, &count, &srgb_p});

  ar::SceneTables tables = {};
  tables.images = hviews.data();
  tables.image_count = count;
  tables.srgb_decode = srgb;
  uint32_t closest = 0, closest_bad = 0, missing = 0;
  std::vector<double> lin_err;
  for (uint32_t i = 0; i < in.n; ++i) {
    const float* r = in.v.data() + size_t(i) * kIn;
    const float uv[3] = {r[0], r[1], 0.0f};
    float cpu[3];
    ar::op_tex_image(tables, uint32_t(r[2]), uint32_t(r[3]), uv, cpu);
    const double* g = gpu.v.data() + size_t(i) * kOut;
    missing += uint32_t(r[2]) >= count;
    if ((uint32_t(r[3]) & ar::kTexLinear) == 0u) {
      ++closest;
      bool same = true;
      for (int c = 0; c < 3; ++c) same = same && ar::bit_cast<uint32_t>(float(g[c])) == ar::bit_cast<uint32_t>(cpu[c]);
      closest_bad += same ? 0u : 1u;
      continue;
    }
    double num = 0.0, den = 0.0;
    for (int c = 0; c < 3; ++c) {
      num += (g[c] - double(cpu[c])) * (g[c] - double(cpu[c]));
      den += double(cpu[c]) * double(cpu[c]);
    }
    lin_err.push_back(den > 0.0 ? std::sqrt(num / den) : (num == 0.0 ? 0.0 : INFINITY));
  }
  const Stats s = stats_from(lin_err);
  std::printf("tex_image_agree closest: %u lookups, %u not bit-identical; linear: %u lookups, max relative error %.3e "
              "(p99.9 %.3e); %u outside the table\n",
              closest, closest_bad, s.n, s.max, s.p999, missing);
  CHECK(closest > 40000u);
  CHECK(s.n > 40000u);
  CHECK(missing > 10000u);
  CHECK(closest_bad == 0u);
  CHECK(s.max <= kMaxRel);

  dev->free(vb);
  dev->free(sb);
  for (DeviceBuffer& b : texels) dev->free(b);
  CHECK(ledger.total() == 0);
}

}  // TEST_SUITE

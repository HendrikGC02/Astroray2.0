// SPDX-License-Identifier: Apache-2.0
// Generated data files: the common binary header, the observer.bin layout, SHA-256, and the
// loader of the copies embedded in the DLL as RCDATA (0001 D8.4, 0003 d14, d18).
// Header-only parts are shared with tools/specgen so the format exists once.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace astroray {

// SHA-256, FIPS 180-4. Used for the observer hash and the data manifest checks.
inline void sha256(const void* data, size_t n, uint8_t out[32]) {
  static const uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rotr = [](uint32_t x, int r) { return (x >> r) | (x << (32 - r)); };
  auto block = [&](const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  };
  const uint8_t* p = static_cast<const uint8_t*>(data);
  size_t full = n / 64;
  for (size_t i = 0; i < full; ++i) block(p + 64 * i);
  uint8_t tail[128] = {};
  size_t rem = n - full * 64;
  if (rem) std::memcpy(tail, p + full * 64, rem);
  tail[rem] = 0x80;
  size_t len = (rem < 56) ? 64 : 128;
  uint64_t bits = uint64_t(n) * 8;
  for (int i = 0; i < 8; ++i) tail[len - 1 - i] = uint8_t(bits >> (8 * i));
  block(tail);
  if (len == 128) block(tail + 64);
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
}

// Common prefix of every generated binary file (observer.bin now, the .jh tables of 023
// later): magic, version, kind, observer_hash. Little-endian, packed.
constexpr size_t kDataMagicBytes = 8;
constexpr char kDataMagic[kDataMagicBytes] = {'A', 'R', 'D', 'A', 'T', 'A', '\0', '\0'};
constexpr size_t kDataHashOffset = 16;  // 8 bytes: first 8 bytes of SHA-256 of the CMF float data
constexpr uint32_t kObserverVersion = 1;
constexpr uint32_t kDataKindObserver = 1;

// observer.bin layout, offsets in bytes:
//   0 magic[8]  8 version u32  12 kind u32  16 observer_hash[8]
//   24 n u32  28 lambda_min_nm u32  32 lambda_step_nm u32
//   36 xbar f32[n], ybar f32[n], zbar f32[n]    then Y_n f64, K f64 (K = 683 Y_n)
//   then D65 SPD f32[n] (Y = 1)    then M_ws f64[9], XYZ -> linear Rec.709, row-major
constexpr uint32_t kObserverSamples = 471;
constexpr uint32_t kObserverLambdaMin = 360;
constexpr size_t kObserverCmfOffset = 36;
constexpr size_t kObserverYnOffset = kObserverCmfOffset + 3 * 4 * kObserverSamples;
constexpr size_t kObserverKOffset = kObserverYnOffset + 8;
constexpr size_t kObserverD65Offset = kObserverKOffset + 8;
constexpr size_t kObserverMwsOffset = kObserverD65Offset + 4 * kObserverSamples;
constexpr size_t kObserverBytes = kObserverMwsOffset + 72;

struct ObserverTables {
  float cmf[3][kObserverSamples];  // xbar, ybar, zbar at 360 + i nm
  float d65[kObserverSamples];
  double y_n = 0, k = 0;
  double m_ws[9] = {};
  float y_row[3] = {};  // the working space's RGB-to-Y row, derived in parse_observer from m_ws
  uint8_t observer_hash[8] = {};
};

// Row 1 (Y) of the inverse of the XYZ-to-RGB matrix `m` (row-major), in double: the weights of the
// working space's scene-linear luminance, which is what Blender's implicit colour-to-float
// conversion takes. Row 1 of an inverse is the cofactor column of m's row 1, over the determinant.
// Ref: Cycles src/scene/shader.cpp (rgb_to_y = rgb_to_xyz.y) and kernel/util/colorspace.h
// (linear_rgb_to_gray), cycles ref cache of task 143.
inline void working_space_y_row(const double m[9], double out[3]) {
  const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                     m[2] * (m[3] * m[7] - m[4] * m[6]);
  out[0] = -(m[1] * m[8] - m[2] * m[7]) / det;
  out[1] = (m[0] * m[8] - m[2] * m[6]) / det;
  out[2] = -(m[0] * m[7] - m[1] * m[6]) / det;
}

// The observer hash: first 8 bytes of SHA-256 over xbar, ybar, zbar as little-endian floats.
inline void observer_hash_of(const float cmf[3][kObserverSamples], uint8_t out[8]) {
  uint8_t d[32];
  sha256(cmf, sizeof(float) * 3 * kObserverSamples, d);
  std::memcpy(out, d, 8);
}

// Returns an empty string on success, otherwise what is wrong (fail loudly: the caller shows it).
inline std::string parse_observer(const void* data, size_t size, ObserverTables* t) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  if (size != kObserverBytes) return "observer.bin has " + std::to_string(size) + " bytes, expected " + std::to_string(kObserverBytes);
  if (std::memcmp(p, kDataMagic, kDataMagicBytes) != 0) return "observer.bin: bad magic";
  uint32_t v, kind, n, l0, step;
  std::memcpy(&v, p + 8, 4);
  std::memcpy(&kind, p + 12, 4);
  std::memcpy(&n, p + 24, 4);
  std::memcpy(&l0, p + 28, 4);
  std::memcpy(&step, p + 32, 4);
  if (v != kObserverVersion || kind != kDataKindObserver) return "observer.bin: unsupported version or kind";
  if (n != kObserverSamples || l0 != kObserverLambdaMin || step != 1) return "observer.bin: unexpected sampling";
  std::memcpy(t->cmf, p + kObserverCmfOffset, sizeof(t->cmf));
  std::memcpy(&t->y_n, p + kObserverYnOffset, 8);
  std::memcpy(&t->k, p + kObserverKOffset, 8);
  std::memcpy(t->d65, p + kObserverD65Offset, sizeof(t->d65));
  std::memcpy(t->m_ws, p + kObserverMwsOffset, sizeof(t->m_ws));
  double y[3];
  working_space_y_row(t->m_ws, y);
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(y[i])) return "observer.bin: M_ws is singular";
    t->y_row[i] = float(y[i]);
  }
  std::memcpy(t->observer_hash, p + kDataHashOffset, 8);
  uint8_t h[8];
  observer_hash_of(t->cmf, h);
  if (std::memcmp(h, t->observer_hash, 8) != 0) return "observer.bin: observer_hash does not match its CMF data";
  return {};
}

struct DataBlob {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

// Finds an embedded data file by its path under data/ (e.g. "spectral/observer.bin").
// Returns false when this module has no such resource.
bool find_data(const char* name, DataBlob* out);

// A set is consistent when every file that has the common header carries the same
// observer_hash (0003 d14). Files without the header are not part of the comparison.
// Returns an empty string when consistent, else the reason.
inline std::string check_hash_set(const std::vector<std::pair<std::string, DataBlob>>& files) {
  uint8_t first[8] = {};
  std::string first_name;
  for (const auto& f : files) {
    const DataBlob& b = f.second;
    if (b.size < kDataHashOffset + 8 || std::memcmp(b.data, kDataMagic, kDataMagicBytes) != 0) continue;
    if (first_name.empty()) {
      std::memcpy(first, b.data + kDataHashOffset, 8);
      first_name = f.first;
    } else if (std::memcmp(first, b.data + kDataHashOffset, 8) != 0) {
      return "observer_hash of " + f.first + " differs from " + first_name;
    }
  }
  if (first_name.empty()) return "no data file carries an observer_hash";
  return {};
}

// check_hash_set over every file embedded in this module.
std::string check_observer_consistency();

}  // namespace astroray

// SPDX-License-Identifier: Apache-2.0
// The authored inputs of a GRATING material: the 64 bytes of ar_material_desc::ext_constants
// (0005 items 16, 23, 24; 0007 appendix row ar_material_desc). They are generator inputs: the host
// turns them into ExtParams and an efficiency table at compile time and none of them reaches the
// device (item 24). The field list below is the one source of the layout: this struct, the parse
// and scenes/lib/ar.py (which reads this file) all follow it. No cross-version promise (0001 D2).
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace astroray::host {

// X(name, c type, struct-module format character). Little-endian, packed in this order; the bytes
// after the last field (to 64) are reserved and required zero.
#define AR_GRATING_AUTHORED_FIELDS(X) \
  X(pitch_nm, float, 'f')             \
  X(depth_nm, float, 'f')             \
  X(coating_n, float, 'f')            \
  X(coating_k, float, 'f')            \
  X(mode, uint8_t, 'B')               \
  X(profile, uint8_t, 'B')            \
  X(m_min, int8_t, 'b')               \
  X(m_max, int8_t, 'b')

enum class GratingAuthoredMode : uint8_t { Reflect = 0, Transmit = 1 };
enum class GratingAuthoredProfile : uint8_t { Sinusoidal = 0, Blazed = 1, Csv = 2 };

struct GratingAuthored {
#define AR_X(name, type, fmt) type name;
  AR_GRATING_AUTHORED_FIELDS(AR_X)
#undef AR_X
  uint8_t reserved[44];  // required zero
};
static_assert(sizeof(GratingAuthored) == 64, "the authored block is ar_material_desc::ext_constants, 64 B");
static_assert(alignof(GratingAuthored) == 4, "no padding between the fields");
static_assert(offsetof(GratingAuthored, reserved) == 20, "the fields take 20 B");

// Why a block is refused: `field` is the offending field's name, `why` the full text.
struct GratingAuthoredError {
  std::string field;
  std::string why;
};

// Copies the 64 bytes into `out` and checks them. Returns false and fills `err` on the first bad
// field, in the order of the list above. Transmission, and the blazed and CSV profiles, are valid
// choices that are not built: refused with "not built". Ref: 0005 items 23-24.
inline bool parse_grating_authored(const uint8_t bytes[64], GratingAuthored* out, GratingAuthoredError* err) {
  std::memcpy(out, bytes, sizeof(*out));
  const GratingAuthored& g = *out;
  auto bad = [&](const char* field, const std::string& why) {
    err->field = field;
    err->why = std::string(field) + " " + why;
    return false;
  };
  if (!(std::isfinite(g.pitch_nm) && g.pitch_nm > 0.0f)) return bad("pitch_nm", std::to_string(g.pitch_nm) + " is not finite and positive");
  if (!(std::isfinite(g.depth_nm) && g.depth_nm > 0.0f)) return bad("depth_nm", std::to_string(g.depth_nm) + " is not finite and positive");
  if (!(std::isfinite(g.coating_n) && g.coating_n >= 0.0f)) return bad("coating_n", std::to_string(g.coating_n) + " is not finite and >= 0");
  if (!(std::isfinite(g.coating_k) && g.coating_k >= 0.0f)) return bad("coating_k", std::to_string(g.coating_k) + " is not finite and >= 0");
  if (g.mode > uint8_t(GratingAuthoredMode::Transmit))
    return bad("mode", std::to_string(g.mode) + " is neither 0 (reflect) nor 1 (transmit)");
  if (g.profile > uint8_t(GratingAuthoredProfile::Csv))
    return bad("profile", std::to_string(g.profile) + " is not 0 (sinusoidal), 1 (blazed) or 2 (csv)");
  if (g.m_min < -8 || g.m_min > 8) return bad("m_min", std::to_string(g.m_min) + " is outside -8..8");
  if (g.m_max < -8 || g.m_max > 8) return bad("m_max", std::to_string(g.m_max) + " is outside -8..8");
  if (g.m_min > g.m_max) return bad("m_min", std::to_string(g.m_min) + " exceeds m_max " + std::to_string(g.m_max));
  for (size_t i = 0; i < sizeof(g.reserved); ++i)
    if (g.reserved[i] != 0) return bad("reserved", "byte " + std::to_string(offsetof(GratingAuthored, reserved) + i) + " is not zero");
  if (g.mode == uint8_t(GratingAuthoredMode::Transmit)) return bad("mode", "transmission grating not built");
  if (g.profile != uint8_t(GratingAuthoredProfile::Sinusoidal))
    return bad("profile", std::string(g.profile == uint8_t(GratingAuthoredProfile::Blazed) ? "blazed" : "csv") + " not built");
  return true;
}

}  // namespace astroray::host

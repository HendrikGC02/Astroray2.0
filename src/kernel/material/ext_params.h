// SPDX-License-Identifier: Apache-2.0
// ExtParams: the per-material constants of an extension kind, and the closed kind list.
// Ref: docs/decisions/0005-material-model.md items 14 (kinds), 16 (64 B block at ext_index;
// ext_fields.def), 23 (what the grating's block must hold).
#pragma once
#include <cstdint>

#include "../portability.h"
#include "spectrum_ref.h"

AR_NS_BEGIN

// The ids come from ext_fields.def, the one place they are written.
enum class ExtKind : uint32_t {
#define AR_EXT_KIND(name, id) name = id,
#include "ext_fields.def"
};

// 16 x 32-bit, PROVISIONAL (item 16). Each kind lays out its own words (the grating's follow item 23);
// ar_material_desc::ext_constants carries this block byte for byte.
struct ExtParams {
  uint32_t w[16];
};

static_assert(sizeof(ExtParams) == 64, "ExtParams is 64 B (0005 item 16)");
static_assert(alignof(ExtParams) == 4, "ExtParams is 16 u32 words");

// The GRATING kind's view of its block (item 23). Words: w[0] pitch_nm (f32 bits); w[1] mode u8 |
// m_min i8 << 8 | m_max i8 << 16, top byte unread (written 0); w[2] table (TableArena handle);
// w[3..8] substrate IOR SpectrumRef, read in transmission only; w[9..15] unread. The block crosses
// the ABI byte for byte, so this one decoder and its encoder are the layout.
enum class GratingMode : uint32_t { Reflect = 0, Transmit = 1 };

struct GratingParams {
  float pitch_nm;
  uint32_t mode;  // GratingMode; the compiler refuses anything else
  int32_t m_min, m_max;
  uint32_t table;
  SpectrumRef substrate_ior;
};

struct GratingRefWords {
  uint32_t w[6];
};
static_assert(sizeof(GratingRefWords) == sizeof(SpectrumRef), "the substrate ref is six words");

AR_FN GratingParams grating_params(const ExtParams& e) {
  GratingParams g;
  g.pitch_nm = bit_cast<float>(e.w[0]);
  g.mode = e.w[1] & 0xffu;
  g.m_min = int32_t(int8_t(uint8_t((e.w[1] >> 8) & 0xffu)));
  g.m_max = int32_t(int8_t(uint8_t((e.w[1] >> 16) & 0xffu)));
  g.table = e.w[2];
  GratingRefWords ref;
  for (int i = 0; i < 6; ++i) ref.w[i] = e.w[3 + i];
  g.substrate_ior = bit_cast<SpectrumRef>(ref);
  return g;
}

AR_FN ExtParams grating_ext_params(const GratingParams& g) {
  ExtParams e = {};
  e.w[0] = bit_cast<uint32_t>(g.pitch_nm);
  e.w[1] = (g.mode & 0xffu) | (uint32_t(uint8_t(int8_t(g.m_min))) << 8) | (uint32_t(uint8_t(int8_t(g.m_max))) << 16);
  e.w[2] = g.table;
  const auto ref = bit_cast<GratingRefWords>(g.substrate_ior);
  for (int i = 0; i < 6; ++i) e.w[3 + i] = ref.w[i];
  return e;
}

AR_NS_END

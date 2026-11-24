// SPDX-License-Identifier: Apache-2.0
// SpectrumRef: one colour or IOR source of a material slot, 24 B. Which spectrum it is lives in
// `kind`; how an RGB source is lifted is fixed by the slot and recorded in `semantic`.
// Ref: docs/decisions/0005-material-model.md item 9 (layout, closed kinds), item 10 (semantics).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

// Closed list (0005 item 9). CONSTANT, RGB, TABLE, BLACKBODY (ILLUMINANT slots) and CAUCHY_ABBE
// (IOR slots) resolve; the compiler rejects the others, so they exist as values only. CONSTANT, not
// CONST: windows.h defines CONST.
enum class SpectrumKind : uint8_t {
  CONSTANT = 0,  // p[0] = value on every wavelength
  RGB,           // p[0..2] = sigmoid c0..c2 (Jakob-Hanika), p[3] = scale
  TABLE,         // index = arena offset of the first wavelength, aux = n points, p[0] = scale
  BLACKBODY,     // p[0] = T, p[1] = scale x 1 / Y(Planck(T))
  CAUCHY_ABBE,   // p[0] = n_d, p[1] = V_d, p[2] = dispersion scale
  SELLMEIER,     // index = arena offset of six coefficients (M4)
  GAUSSIAN       // p[0] = centre, p[1] = width, p[2] = peak (M4)
};

// The lift semantic of the slot (0005 item 10 table).
enum class SpectrumSemantic : uint8_t {
  REFLECTANCE = 0,  // clamp to [0,1], albedo sigmoid
  UNBOUNDED,        // unbounded form, scale = 2 max
  ILLUMINANT,       // unbounded form times the working space's illuminant
  IOR               // flat value per shading point; RGB sources are refused by the compiler
};

struct SpectrumRef {
  uint8_t kind;      // SpectrumKind
  uint8_t semantic;  // SpectrumSemantic
  uint16_t aux;
  uint32_t index;
  float p[4];
};

static_assert(sizeof(SpectrumRef) == 24, "SpectrumRef is 24 B (0005 item 9)");

AR_NS_END

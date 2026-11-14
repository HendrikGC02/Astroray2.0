// Fixed material record modelled on OpenPBR Surface (v1.1 parameter groups).
// Stored packed as floats (MatBlob); unpack() converts to Real. Colours are
// already 4-/8-wavelength spectra (upsampling is out of scope for the spike).
#pragma once
#include "kl.h"

namespace kl {
enum ScalarSlot {
  S_BASE_WEIGHT, S_BASE_ROUGH, S_METALNESS,
  S_SPEC_WEIGHT, S_SPEC_ROUGH, S_SPEC_IOR, S_SPEC_ANISO,
  S_TRANS_WEIGHT,
  S_COAT_WEIGHT, S_COAT_ROUGH, S_COAT_IOR,
  S_FUZZ_WEIGHT, S_FUZZ_ROUGH,
  S_EMISSION_LUM,
  S_FILM_THICK, S_FILM_IOR,
  S_OPACITY,
  S_PAD0, S_PAD1, S_PAD2,
  S_COUNT // 20
};
enum SpecSlot { SP_BASE, SP_SPEC, SP_TRANS, SP_COAT, SP_FUZZ, SP_EMISSION, SP_COUNT };

template <int N> struct MatRec {
  Real s[S_COUNT];
  Spec<N> sp[SP_COUNT];
};
template <int N> struct MatLayout { enum { FLOATS = S_COUNT + SP_COUNT * N }; };

// Packed (float) record: [scalars(20)][6 spectra * N]
template <int N> FN MatRec<N> unpack(const float* b) {
  MatRec<N> m;
  for (int i = 0; i < S_COUNT; ++i) m.s[i] = (Real)b[i];
  for (int k = 0; k < SP_COUNT; ++k)
    for (int i = 0; i < N; ++i) m.sp[k].v[i] = (Real)b[S_COUNT + k * N + i];
  return m;
}
} // namespace kl

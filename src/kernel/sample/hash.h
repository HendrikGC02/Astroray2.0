// SPDX-License-Identifier: Apache-2.0
// Integer hashes of the sampler: pbrt's MixBits for keys, Burley's nested uniform
// (Owen) scramble for the sample index and the Sobol lanes, and pcg4d for the
// independent mode. Integer arithmetic only, so every build draws identical bits.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d23;
//   Burley 2020, "Practical Hash-based Owen Scrambling", JCGT 9(4), listings
//   laine_karras_permutation and nested_uniform_scramble (constants as printed there);
//   pbrt-v4 src/pbrt/util/hash.h MixBits (Apache-2.0);
//   Jarzynski and Olano 2020, "Hash Functions for GPU Rendering", JCGT 9(3), pcg4d.
#pragma once
#include <cstdint>

#include "core/real.h"
#include "portability.h"

AR_NS_BEGIN

// pbrt-v4 MixBits: a 64-bit finaliser.
AR_FN uint64_t mix_bits(uint64_t v) {
  v ^= (v >> 31);
  v *= 0x7fb5d329728ea185ull;
  v ^= (v >> 27);
  v *= 0x81dadef4bc2dd44dull;
  v ^= (v >> 33);
  return v;
}

AR_FN uint32_t reverse_bits32(uint32_t v) {
  v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
  v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
  v = ((v >> 4) & 0x0f0f0f0fu) | ((v & 0x0f0f0f0fu) << 4);
  v = ((v >> 8) & 0x00ff00ffu) | ((v & 0x00ff00ffu) << 8);
  return (v >> 16) | (v << 16);
}

// Burley 2020, Laine-Karras-style permutation (applied to bit-reversed input).
AR_FN uint32_t laine_karras_permutation(uint32_t x, uint32_t seed) {
  x += seed;
  x ^= x * 0x6c50b47cu;
  x ^= x * 0xb82f1e52u;
  x ^= x * 0xc7afe638u;
  x ^= x * 0x8d22f6e6u;
  return x;
}

// Burley 2020: Owen scrambling of a 32-bit fixed-point value, nested uniform.
AR_FN uint32_t nested_uniform_scramble(uint32_t x, uint32_t seed) {
  x = reverse_bits32(x);
  x = laine_karras_permutation(x, seed);
  return reverse_bits32(x);
}

// s = hash(id, seed, stream, b) of 0003 d23. d23 mandates MixBits and does not say how
// the four words are combined; chaining two MixBits rounds over 64-bit halves is our choice.
AR_FN uint32_t block_seed(uint32_t id, uint32_t seed, uint32_t stream, uint32_t block) {
  uint64_t h = mix_bits((uint64_t(id) << 32) | seed);
  h = mix_bits(h ^ ((uint64_t(stream) << 32) | block));
  return uint32_t(h >> 32);
}

// hash(s, j): the scramble seed of lane j in the block seeded by s. Burley 2020 Listing 3
// writes hash_combine(seed, i) and nowhere defines it (the text only asks for "a different
// seed in each dimension"), so MixBits stands in, as d23 mandates it for hashing.
AR_FN uint32_t lane_seed(uint32_t s, uint32_t lane) {
  return uint32_t(mix_bits((uint64_t(s) << 32) | lane) >> 32);
}

struct UInt4 {
  uint32_t x, y, z, w;
};

// Jarzynski and Olano 2020, pcg4d.
AR_FN UInt4 pcg4d(UInt4 v) {
  v.x = v.x * 1664525u + 1013904223u;
  v.y = v.y * 1664525u + 1013904223u;
  v.z = v.z * 1664525u + 1013904223u;
  v.w = v.w * 1664525u + 1013904223u;
  v.x += v.y * v.w;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v.w += v.y * v.z;
  v.x ^= v.x >> 16u;
  v.y ^= v.y >> 16u;
  v.z ^= v.z >> 16u;
  v.w ^= v.w >> 16u;
  v.x += v.y * v.w;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v.w += v.y * v.z;
  return v;
}

AR_NS_END

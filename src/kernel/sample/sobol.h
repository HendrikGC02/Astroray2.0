// SPDX-License-Identifier: Apache-2.0
// Owen-scrambled Sobol sampler (0003 d23): sample() is the Burley 2020 shuffled,
// scrambled 4-D Sobol point of one (id, index, stream) key, lane by lane, and
// sample_independent() is the pcg4d mode. Integer arithmetic only; the final
// conversion (x >> 8) * 2^-24 is exact in float and double.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d23;
//   Burley 2020, "Practical Hash-based Owen Scrambling", JCGT 9(4), listing
//   shuffled_scrambled_sobol4d (the index is Owen-shuffled with the block seed,
//   each lane scrambled with a seed derived from it);
//   Joe and Kuo 2008, "Constructing Sobol sequences with better two-dimensional
//   projections", SIAM J. Sci. Comput. 30(5), file new-joe-kuo-6.21201.
//
// The direction numbers below are the first four dimensions of new-joe-kuo-6.21201
// (dimension 1: van der Corput; then rows "d s a m_i" = "2 1 0 1", "3 2 1 1 3",
// "4 3 1 1 3 1"), expanded by the Joe and Kuo recurrence into v_k = m_k * 2^(32-k).
//
// Joe and Kuo licence (BSD-3-Clause), applying to the direction numbers:
//   Copyright (c) 2008, Frances Y. Kuo and Stephen Joe. All rights reserved.
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are met:
//   * Redistributions of source code must retain the above copyright notice, this
//     list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the names of the copyright holders nor the names of the University
//     of New South Wales and the University of Waikato and its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY EXPRESS OR
//   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
//   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
//   EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY DIRECT, INDIRECT,
//   INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
//   EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"
#include "hash.h"

AR_NS_BEGIN

// Sampler mode: a per-session value passed to the kernels, never chosen per path.
inline constexpr uint32_t SAMPLER_SOBOL = 0;
inline constexpr uint32_t SAMPLER_INDEPENDENT = 1;

// Lane j of the 4-D Sobol point of `index`, as a 32-bit fixed-point fraction:
// the XOR of the direction numbers v_k for every set bit k of the index.
// The table is a local so the same code runs on the host and in device code
// without a separate constant-memory copy. One array per lane, so a caller whose lane is a
// compile-time constant keeps 128 B of table on the GPU stack instead of 512 B (the stack
// budget of film_resolve, 0002 D10).
AR_FN uint32_t sobol_lane(uint32_t lane, uint32_t index) {
  constexpr uint32_t kDir0[32] = {0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u, 0x08000000u, 0x04000000u, 0x02000000u,
      0x01000000u, 0x00800000u, 0x00400000u, 0x00200000u, 0x00100000u, 0x00080000u, 0x00040000u,
      0x00020000u, 0x00010000u, 0x00008000u, 0x00004000u, 0x00002000u, 0x00001000u, 0x00000800u,
      0x00000400u, 0x00000200u, 0x00000100u, 0x00000080u, 0x00000040u, 0x00000020u, 0x00000010u,
      0x00000008u, 0x00000004u, 0x00000002u, 0x00000001u};
  constexpr uint32_t kDir1[32] = {0x80000000u, 0xc0000000u, 0xa0000000u, 0xf0000000u, 0x88000000u, 0xcc000000u, 0xaa000000u,
      0xff000000u, 0x80800000u, 0xc0c00000u, 0xa0a00000u, 0xf0f00000u, 0x88880000u, 0xcccc0000u,
      0xaaaa0000u, 0xffff0000u, 0x80008000u, 0xc000c000u, 0xa000a000u, 0xf000f000u, 0x88008800u,
      0xcc00cc00u, 0xaa00aa00u, 0xff00ff00u, 0x80808080u, 0xc0c0c0c0u, 0xa0a0a0a0u, 0xf0f0f0f0u,
      0x88888888u, 0xccccccccu, 0xaaaaaaaau, 0xffffffffu};
  constexpr uint32_t kDir2[32] = {0x80000000u, 0xc0000000u, 0x60000000u, 0x90000000u, 0xe8000000u, 0x5c000000u, 0x8e000000u,
      0xc5000000u, 0x68800000u, 0x9cc00000u, 0xee600000u, 0x55900000u, 0x80680000u, 0xc09c0000u,
      0x60ee0000u, 0x90550000u, 0xe8808000u, 0x5cc0c000u, 0x8e606000u, 0xc5909000u, 0x6868e800u,
      0x9c9c5c00u, 0xeeee8e00u, 0x5555c500u, 0x8000e880u, 0xc0005cc0u, 0x60008e60u, 0x9000c590u,
      0xe8006868u, 0x5c009c9cu, 0x8e00eeeeu, 0xc5005555u};
  constexpr uint32_t kDir3[32] = {0x80000000u, 0xc0000000u, 0x20000000u, 0x50000000u, 0xf8000000u, 0x74000000u, 0xa2000000u,
      0x93000000u, 0xd8800000u, 0x25400000u, 0x59e00000u, 0xe6d00000u, 0x78080000u, 0xb40c0000u,
      0x82020000u, 0xc3050000u, 0x208f8000u, 0x51474000u, 0xfbea2000u, 0x75d93000u, 0xa0858800u,
      0x914e5400u, 0xdbe79e00u, 0x25db6d00u, 0x58800080u, 0xe54000c0u, 0x79e00020u, 0xb6d00050u,
      0x800800f8u, 0xc00c0074u, 0x200200a2u, 0x50050093u};
  AR_ASSERT(lane < 4u);
  uint32_t x = 0u;
  for (uint32_t k = 0u; index != 0u; index >>= 1, ++k)
    if (index & 1u) x ^= lane == 0u ? kDir0[k] : lane == 1u ? kDir1[k] : lane == 2u ? kDir2[k] : kDir3[k];
  return x;
}

// The 32 random bits of sample(): the fixed-point value before the conversion.
AR_FN uint32_t sample_bits(uint32_t id, uint32_t index, uint32_t stream, uint32_t d, uint32_t seed) {
  const uint32_t block = d >> 2;
  const uint32_t lane = d & 3u;
  const uint32_t s = block_seed(id, seed, stream, block);
  const uint32_t shuffled = nested_uniform_scramble(index, s);
  return nested_uniform_scramble(sobol_lane(lane, shuffled), lane_seed(s, lane));
}

AR_FN Real bits_to_unit(uint32_t x) { return Real(x >> 8) * Real(5.9604644775390625e-08); }  // 2^-24

// u = sample(id, index, stream, d, seed) of 0003 d23.
AR_FN Real sample(uint32_t id, uint32_t index, uint32_t stream, uint32_t d, uint32_t seed) {
  return bits_to_unit(sample_bits(id, index, stream, d, seed));
}

// Independent mode: same keys and dimension map, mutually independent samples.
AR_FN Real sample_independent(uint32_t id, uint32_t index, uint32_t stream, uint32_t d,
                              uint32_t seed) {
  AR_ASSERT(stream < 256u && d < (1u << 24));
  const UInt4 h = pcg4d(UInt4{id, index, (stream << 24) + d, seed});
  return bits_to_unit(h.x);
}

// The draw every consumer calls; `mode` is the session's SAMPLER_* uniform.
AR_FN Real draw(uint32_t mode, uint32_t id, uint32_t index, uint32_t stream, uint32_t d,
                uint32_t seed) {
  return mode == SAMPLER_INDEPENDENT ? sample_independent(id, index, stream, d, seed)
                                     : sample(id, index, stream, d, seed);
}

AR_NS_END

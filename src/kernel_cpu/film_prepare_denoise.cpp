// SPDX-License-Identifier: Apache-2.0
// film_prepare_denoise on the host thread pool.
// Ref: docs/decisions/0008-film-outputs-denoise.md d15; film/prepare_denoise.h.
#include "../kernel/film/prepare_denoise.h"

void cpu_film_prepare_denoise(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  for (uint32_t j = first; j < last; ++j) ar::film_prepare_denoise_pixel(kg, j);
}

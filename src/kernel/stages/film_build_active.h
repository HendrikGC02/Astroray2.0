// SPDX-License-Identifier: Apache-2.0
// film_build_active: the per-pass kernel that writes the film-owned active-pixel list at pass
// start, one thread per listed pixel. In M1a the list holds every pixel of the render
// sub-rectangle in row order (film_active_pixel); adaptive compaction (M4) changes only this
// kernel. generate_camera, the resolve and every later stage read the list and never write it.
// The count u is film_build_active_count: the host launches that many items.
// Ref: docs/decisions/0008-film-outputs-denoise.md d3 (slot assignment, active-pixel list);
// 0002-kernel-architecture.md D8; 0003 d23 (the key reads the list).
#pragma once
#include <cstdint>

#include "../film/film_write.h"
#include "../kernel_globals.h"
#include "../portability.h"

AR_NS_BEGIN

AR_FN uint32_t film_build_active_count(const KernelGlobals& kg) { return kg.film->rect_width * kg.film->rect_height; }

AR_FN void film_build_active_pixel(const KernelGlobals& kg, uint32_t j) {
  AR_ASSERT(j < kg.film->plane_stride);
  kg.active_pixels[j] = film_active_pixel(*kg.film, j);
}

AR_NS_END

// CPU chunk function: listed pixels [first, last). Defined in kernel_cpu/film_build_active.cpp.
void cpu_film_build_active(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);

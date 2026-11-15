// SPDX-License-Identifier: Apache-2.0
// generate_camera: the refill launch that starts camera paths in free main-pool slots.
// Item i of the launch takes slot main_queue[step.start + i] and camera sample
// sample_id = step.shadow_base + i (the host's overwrite of the TERMINATED entry,
// sched/step_table.h); the sample's pixel is active_pixels[sample_id / k] and its index
// n + sample_id % k (camera_key, 0003 d23), so slot = j * k + m of 0008 d3 holds by
// construction. Dimensions 0-7 of stream 0 are drawn from that key: filter offset (0-1), lens
// (2-3, unused by the pinhole), time (4, not stored without motion), film wavelength (5), hero
// lane (6). What the key determines (pixel, film wavelengths, pf) is not stored (0003 d2).
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d2, d6, d19, d21-d23;
// 0008-film-outputs-denoise.md d3, d5; 0002-kernel-architecture.md D7 step 4, D8;
// Akenine-Moller, Nowrouzezahrai, Dachsbacher and Munkberg 2021, "Improved Shader and Texture
// Level of Detail Using Ray Cones", JCGT 10(1): the initial cone has width 0 and spread angle
// atan(2 tan(fov_y / 2) / H), the angle one pixel subtends at the centre of the view.
#pragma once
#include <cstdint>

#include "../camera/camera.h"
#include "../core/real.h"
#include "../core/spectrum.h"
#include "../film/film_write.h"
#include "../film/filter.h"
#include "../kernel_globals.h"
#include "../portability.h"
#include "../sample/dims.h"
#include "../sample/keys.h"
#include "../sample/sobol.h"
#include "../sched/step_table.h"
#include "../spectral/wavelength.h"
#include "../state/flags.h"
#include "../state/stage.h"
#include "../state/termination.h"

AR_NS_BEGIN

// ray_type is the instance-mask bit of the next ray (0003 d19 row 4): AR_VISIBILITY_CAMERA of
// include/astroray/astroray.h, which a kernel header cannot include (a test pins the two).
inline constexpr uint8_t kRayTypeCamera = 0x1;

// The spread of the initial ray cone. The film plane is z = -1, so one pixel is film_extent.y /
// height tall there, which is 2 tan(fov_y / 2) / H of the paper with any film shift folded in.
AR_FN Real camera_cone_spread(const CameraView& c) { return atan2(c.film_extent.y / c.height, Real(1)); }

// Sampler dimension d of the camera sample `key` (stream 0, the session's mode and seed).
AR_FN Real camera_draw(const KernelGlobals& kg, const SamplerKey& key, uint32_t d) {
  return draw(kg.session.sampler_mode, key.id, key.index, STREAM_FIXED, d, kg.session.seed);
}

// The film position in pixels of the sample whose pixel plane index is key.id: the pixel centre
// plus the filter-importance-sampled offset (0008 d5), or the centre alone under the
// PIXEL_CENTRE hook. y is up from the bottom row (camera.h).
AR_FN Real2 camera_film_position(const KernelGlobals& kg, const SamplerKey& key) {
  const uint32_t width = kg.film->width;
  Real dx = Real(0), dy = Real(0);
  if ((kg.session.test_hooks & TEST_HOOK_PIXEL_CENTRE) == 0u) {
    const FilterOffset o = sample_filter(*kg.filter, camera_draw(kg, key, DIM_CAM_FILTER_X),
                                         camera_draw(kg, key, DIM_CAM_FILTER_Y));
    dx = o.dx;
    dy = o.dy;
  }
  return Real2{Real(key.id % width) + Real(0.5) + dx, Real(key.id / width) + Real(0.5) + dy};
}

AR_FN void stage_generate_camera(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  const uint32_t sample_id = step.shadow_base + item;
  const SessionUniforms& s = kg.session;
  const SamplerKey key = camera_key(kg.active_pixels, sample_id, s.samples_done, s.samples_per_pass);

  CameraSample cs;
  cs.film = camera_film_position(kg, key);
  cs.lens = Real2{camera_draw(kg, key, DIM_CAM_LENS_U), camera_draw(kg, key, DIM_CAM_LENS_V)};
  cs.time = camera_draw(kg, key, DIM_CAM_TIME);
  const CameraRay ray = camera_generate_ray(*kg.camera, cs);

  // lambda_t = lambda_f at the start of a path (0003 d2); the resolve recomputes lambda_f and pf
  // from the same key and dimension (film_resolve_pixel).
  const FilmWavelengths fw = sample_film_wavelengths(kg.film->wavelengths, camera_draw(kg, key, DIM_CAM_WAVELENGTH));
  Spec lambda_t;
  for (int i = 0; i < kLanes; ++i) lambda_t.v[i] = fw.lf.nm[i];

  // Hero lane floor(4 u) (0003 d6); the clamp guards u rounding up to 1.
  uint32_t hero = uint32_t(camera_draw(kg, key, DIM_CAM_HERO) * Real(4));
  if (hero > 3u) hero = 3u;

  ps_sample_id(kg, slot) = sample_id;
  ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::INTERSECT_CLOSEST);
  ps_term(kg, slot) = static_cast<uint8_t>(Term::ACTIVE);
  ps_ray_type(kg, slot) = kRayTypeCamera;
  ps_flags(kg, slot) = set_hero_lane(0u, hero);  // PATH_KIND clear: a camera path
  ps_bounces(kg, slot) = U16x6{{0, 0, 0, 0, 0, 0}};
  ps_event_index(kg, slot) = 0;
  ps_ray_o(kg, slot) = ray.o;
  ps_ray_d(kg, slot) = ray.d;
  ps_cone(kg, slot) = Real2{Real(0), camera_cone_spread(*kg.camera)};
  ps_lambda_t(kg, slot) = lambda_t;
  ps_beta(kg, slot) = spec_fill(Real(1));
  ps_r_u(kg, slot) = spec_fill(Real(1));
  ps_r_l(kg, slot) = spec_fill(Real(1));
  ps_eta_scale(kg, slot) = Real(1);
  // No previous vertex yet. The hit fields belong to intersect_closest.
  ps_prev_ng(kg, slot) = 0u;
  ps_prev_ns(kg, slot) = 0u;
  ps_prev_dist(kg, slot) = Real(0);
  // ray_time exists only with motion blur (M8): the camera time is not stored without it.
  if (kg.path->ray_time != nullptr) ps_ray_time(kg, slot) = cs.time;
}

AR_NS_END

// CPU chunk function of the refill launch, defined by AR_CPU_STAGE in kernel_cpu/generate_camera.cpp.
void cpu_stage_generate_camera(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first,
                               uint32_t last);

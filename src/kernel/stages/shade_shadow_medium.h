// SPDX-License-Identifier: Apache-2.0
// shade_shadow_medium: the media pre-pass of the SHADE_SHADOW segment (0006 D8, D19). The scheduler
// launches it over the same segment as shade_shadow and before it, only with the media feature. It
// is a kernel, not a Stage: it routes nothing and leaves every item in the segment, so shade_shadow
// then delivers each item as before media, the world program included. Split from shade_shadow under
// 0002 D9 (volumes and shadow evaluation are pre-split by structure there): the medium's volume entry
// beside the world program's entry gave shade_shadow two inlined interpreters, 544 B of stack against
// D10's 512 B for VM kernels.
// An item whose medium_stack names a medium (an M1b item lies in at most one, shadow_medium_field in
// medium/ratio_track.h) is ratio-tracked over its whole segment [0, tmax]; any other item is left
// untouched. The medium is homogeneous until grids arrive: its coefficients come from its volume
// entry once, at the segment start, at the item's lanes, and the majorant is sigma_a + sigma_s, as
// shade_volume's (pbrt-v4 HomogeneousMedium). The distances are drawn at the item's hero lane
// (item_flags bits 2-3) from the shadow medium stream (0003 d23 stream 2) of the target sample's key,
// continuing the item's medium_rng, which the stage stores back. pbrt-v4 SampleLd delivers
// beta f_hat T_ray L / avg(r_l + r_u) with the tracked r_u and r_l; the contribution arrives as
// beta f_hat L / avg(r_l0 + r_u0) (the emit's weight, integrator/mis.h, from the item's r_u0 =
// r_p p_scatter and r_l0 = r_p p_l), so it is multiplied by
// T_ray avg(r_l0 + r_u0) / avg(r_l0 rescale_l + r_u0 rescale_u) and written back, and the medium
// field is cleared: the item is done with its medium step and shade_shadow sees an item in no
// medium. A zero estimate blocks the item; D8's 131,072 tentative collisions block it too and count
// VOLUME_LIMIT once in the termination histogram at the target's pixel. A blocked item ends as an
// occluded one (SHADOW_TERMINATED) with contribution 0, so shade_shadow, which runs over the whole
// segment, adds nothing for it. A volume entry that fails, or a non-finite coefficient or product,
// marks the sample invalid and blocks the item.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D8, D19; 0002 D8, D9, D10; 0003 d20, d23;
// 0004 D4, D5; pbrt-v4 src/pbrt/cpu/integrators.cpp VolPathIntegrator::SampleLd (Apache-2.0) in
// C:\Astroray2.0\third_party\_cache\refs\pbrt-v4-092b23a47ad4.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../film/film_write.h"
#include "../kernel_globals.h"
#include "../medium/medium.h"
#include "../medium/medium_block.h"
#include "../medium/medium_eval.h"
#include "../medium/ratio_track.h"
#include "../portability.h"
#include "../sample/dims.h"
#include "../sample/keys.h"
#include "../scene/scene_view.h"
#include "../sched/step_table.h"
#include "../state/flags.h"
#include "../state/stage.h"
#include "../state/termination.h"
#include "../vm/vm_regs.h"

AR_NS_BEGIN

// Ends item `slot` as occluded with nothing to deliver.
AR_FN void shadow_medium_block(const KernelGlobals& kg, uint32_t slot) {
  sh_contribution(kg, slot) = spec_fill(Real(0));
  sh_medium_stack(kg, slot) = shadow_medium_field(0u);
  sh_next_stage(kg, slot) = static_cast<uint8_t>(Stage::SHADOW_TERMINATED);
}

AR_FN void stage_shade_shadow_medium(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.shadow_queue + step.start + item);
  const uint32_t m = sh_medium_stack(kg, slot).v[0];
  if (m == 0u) return;
  const SceneData& s = scene_of(kg);
  AR_ASSERT(m - 1u < s.medium_count);
  const uint32_t target = sh_target(kg, slot);
  Spec sigma_a, sigma_s;
  {
    Lambda lambda;
    const Spec lt = sh_lambda_t(kg, slot);
    for (int i = 0; i < kLanes; ++i) lambda.nm[i] = lt.v[i];
    MediumSample ms;
    VmRegs regs;
    const MediumTables mt{s.medium_programs, s.medium_blocks, *kg.tables};
    const bool ok = medium_eval(mt, s.medium_descs[m - 1u], sh_ray_o(kg, slot), lambda, &regs, &ms) == VmStatus::OK;
    sigma_a = ms.sigma_a, sigma_s = ms.sigma_s;
    // A NaN or infinite coefficient would read as a zero majorant or a NaN estimate.
    if (!ok || !is_finite(sigma_a) || !is_finite(sigma_s)) {
      sample_mark_invalid(kg, target);
      shadow_medium_block(kg, slot);
      return;
    }
  }
  // Distances in the coefficients' unit (metres): ray_d need not be unit length.
  const Real t_max = sh_tmax(kg, slot) * length(sh_ray_d(kg, slot));
  const SamplerKey key = camera_key(kg.active_pixels, target, kg.session.samples_done, kg.session.samples_per_pass);
  const MediumStream st{kg.session.sampler_mode, key.id, key.index, kg.session.seed, STREAM_SHADOW_MEDIUM};
  uint32_t counter = sh_medium_rng(kg, slot);
  const RatioTrack r = ratio_track_segment(sigma_a, sigma_s, sigma_a + sigma_s, t_max, item_hero_lane(sh_item_flags(kg, slot)), st,
                                           &counter, kShadowCollisionLimit);
  sh_medium_rng(kg, slot) = counter;
  if (r.limit) {
    const FilmData& f = *kg.film;
    if (f.term_hist >= 0)
      film_count_add(kg, uint32_t(f.term_hist) + uint32_t(Term::VOLUME_LIMIT), film_slot_pixel(kg, target), 1u);
    shadow_medium_block(kg, slot);
    return;
  }
  if (!(max_lane(r.T) > Real(0))) {
    shadow_medium_block(kg, slot);
    return;
  }
  const Spec r_u0 = sh_r_u(kg, slot), r_l0 = sh_r_l(kg, slot);
  const Spec c = sh_contribution(kg, slot) * r.T * (avg(r_l0 + r_u0) / avg(r_l0 * r.r_l + r_u0 * r.r_u));
  if (!is_finite(c)) {
    sample_mark_invalid(kg, target);
    shadow_medium_block(kg, slot);
    return;
  }
  sh_contribution(kg, slot) = c;
  sh_medium_stack(kg, slot) = shadow_medium_field(0u);
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU kernel function (a CpuKernelFn over the SHADE_SHADOW segment's items), defined in
// kernel_cpu/shade_shadow_medium.cpp; registered by make_cpu_session_device.
void cpu_shade_shadow_medium(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
#endif

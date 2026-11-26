// SPDX-License-Identifier: Apache-2.0
// shade_shadow: delivery of an unoccluded shadow item. intersect_shadow is an OptiX raygen,
// limited to traversal (0002 D11), so it only writes the outcome: SHADOW_TERMINATED when the
// segment is blocked, SHADE_SHADOW when it is clear. This stage then adds the item's
// contribution to its target, the per-sample buffer slot of the sample that emitted it (0004 D5:
// the path may have ended, or its pool slot been refilled, since), and frees the shadow slot.
// The item is self-contained: nothing of the creating path is read (0004 D4, 0003 d20).
// M1a has no non-opaque materials, so there are no recorded hits to evaluate; M2 extends this stage
// with ah_record's hits (opacity through the VM, and media along the segment in t order).
// An item flagged ITEM_WORLD_LE (NEE toward an environment world, light/world_env.h) still lacks
// the world's radiance: it is the world program run along ray_d at the item's lanes, through
// shade_background's world_program_radiance in the background context with no ray type and depth 0
// (as the bake evaluates it). The NEE stage already applied the MIS weight and the record's
// strength. A program that cannot run, or a non-finite product, marks the sample invalid (rule 5).
// This makes shade_shadow a VM kernel (0002 D10 lists it among them).
// Media (0006 D8, D19): with the media feature, an item whose medium_stack names a medium (an M1b
// item lies in at most one, shadow_medium_field in medium/ratio_track.h) is ratio-tracked over its
// whole segment [0, tmax] first. The medium is homogeneous until grids arrive: its coefficients
// come from its volume entry once, at the segment start, at the item's lanes, and the majorant is
// sigma_a + sigma_s, as shade_volume's (pbrt-v4 HomogeneousMedium). The distances are drawn at the
// item's hero lane (item_flags bits 2-3) from the shadow medium stream (0003 d23 stream 2) of the
// target sample's key, continuing the item's medium_rng, which the stage stores back. Delivery is
// then pbrt-v4 SampleLd's beta f_hat T_ray L / avg(r_l + r_u) with the tracked r_u and r_l: the
// contribution arrives as beta f_hat L / avg(r_l0 + r_u0) (the emit's weight, integrator/mis.h,
// from the item's r_u0 = r_p p_scatter and r_l0 = r_p p_l), so it is multiplied by
// T_ray avg(r_l0 + r_u0) / avg(r_l0 rescale_l + r_u0 rescale_u). A zero estimate blocks the item;
// D8's 131,072 tentative collisions block it too and count VOLUME_LIMIT once in the termination
// histogram at the target's pixel. A volume entry that fails, or a non-finite coefficient or product,
// marks the sample invalid. An item in no medium takes the code path it took before media.
// Ref: docs/decisions/0004-integrator.md D4 (delivery), D5 (late delivery to the sample's slot),
// D11 (radiance from the world program, never the table); 0003 d20 (item_flags, lambda_t);
// 0002 D5, D8 (shade_shadow), D11 (outcomes); 0003 d20 (shadow item), d23; 0006 D8, D19;
// pbrt-v4 src/pbrt/cpu/integrators.cpp VolPathIntegrator::SampleLd (Apache-2.0) in
// C:\Astroray2.0\third_party\_cache\refs\pbrt-v4-092b23a47ad4.
#pragma once
#include <cstdint>

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
#include "shade_background.h"

AR_NS_BEGIN

// The medium part of delivery for an item in medium `medium` (a medium_descs index): *c becomes the
// tracked contribution. False when the item ends here, blocked or with its sample marked invalid.
AR_FN bool shadow_through_medium(const KernelGlobals& kg, uint32_t slot, uint32_t medium, Spec* c) {
  const SceneData& s = scene_of(kg);
  AR_ASSERT(medium < s.medium_count);
  const uint32_t target = sh_target(kg, slot);
  Spec sigma_a, sigma_s;
  {
    Lambda lambda;
    const Spec lt = sh_lambda_t(kg, slot);
    for (int i = 0; i < kLanes; ++i) lambda.nm[i] = lt.v[i];
    MediumSample ms;
    VmRegs regs;
    const MediumTables mt{s.medium_programs, s.medium_blocks, *kg.tables};
    const bool ok = medium_eval(mt, s.medium_descs[medium], sh_ray_o(kg, slot), lambda, &regs, &ms) == VmStatus::OK;
    sigma_a = ms.sigma_a, sigma_s = ms.sigma_s;
    // A NaN or infinite coefficient would read as a zero majorant or a NaN estimate.
    if (!ok || !is_finite(sigma_a) || !is_finite(sigma_s)) {
      sample_mark_invalid(kg, target);
      return false;
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
    return false;
  }
  if (!(max_lane(r.T) > Real(0))) return false;
  const Spec r_u0 = sh_r_u(kg, slot), r_l0 = sh_r_l(kg, slot);
  *c = *c * r.T * (avg(r_l0 + r_u0) / avg(r_l0 * r.r_l + r_u0 * r.r_u));
  if (!is_finite(*c)) {
    sample_mark_invalid(kg, target);
    return false;
  }
  return true;
}

AR_FN void stage_shade_shadow(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.shadow_queue + step.start + item);
  Spec c = sh_contribution(kg, slot);
  if (kg.session.features.media) {
    const uint32_t m = sh_medium_stack(kg, slot).v[0];
    if (m != 0u && !shadow_through_medium(kg, slot, m - 1u, &c)) {
      sh_next_stage(kg, slot) = static_cast<uint8_t>(Stage::SHADOW_TERMINATED);
      return;
    }
  }
  if (kg.shadow->item_flags != nullptr && (sh_item_flags(kg, slot) & ITEM_WORLD_LE) != 0u) {
    const Spec lt = sh_lambda_t(kg, slot);
    Lambda lam;
    for (int i = 0; i < kLanes; ++i) lam.nm[i] = lt.v[i];
    Spec le;
    if (kg.scene == nullptr || !world_program_radiance(*kg.scene, *kg.tables, world_shade_context(normalize(sh_ray_d(kg, slot)), lam), &le) ||
        !is_finite(c * le)) {
      sample_mark_invalid(kg, sh_target(kg, slot));
      sh_next_stage(kg, slot) = static_cast<uint8_t>(Stage::SHADOW_TERMINATED);
      return;
    }
    c *= le;
  }
  sample_add(kg, sh_target(kg, slot), c);
  sh_next_stage(kg, slot) = static_cast<uint8_t>(Stage::SHADOW_TERMINATED);
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_shadow.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_shadow(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);
#endif

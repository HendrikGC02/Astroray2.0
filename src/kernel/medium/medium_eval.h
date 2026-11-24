// SPDX-License-Identifier: Apache-2.0
// medium_eval: the MediumSample of one medium at one point and the path's wavelengths. It runs the
// medium's volume program (the same interpreter as surfaces, entry mode run_volume_entry), so a
// coefficient has one lowering path (0006 A.1). VOLUME_COEFFS resolves the block's refs with
// resolve(), one sigmoid per lane for a folded colour.
//
// MediumDesc::program indexes `blocks`: the medium's constant block, whose `entry` is the word
// offset of its volume entry in `programs` (0 = no program: vacuum). Media are static in 2.0, so
// there is no time argument. `lambda_rest` is the medium's rest-frame wavelengths; it equals the
// path's until the rest-frame shift of M7. p is in metres. `regs` is the calling kernel's one
// register file. Any VmStatus other than OK leaves *out as medium_sample_zero() or partially
// filled; the caller counts the error and stops the path.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md A.1, A.2, A.4, D2.
#pragma once
#include "../core/real.h"
#include "../core/spectrum.h"
#include "../core/vec.h"
#include "../portability.h"
#include "../vm/context.h"
#include "../vm/interp.h"
#include "medium.h"
#include "medium_block.h"

AR_NS_BEGIN

struct MediumTables {
  const uint32_t* programs;  // volume programs of every medium, word 0 reserved so entry 0 means none
  const MediumBlock* blocks;
  SceneTables scene;         // the observer for resolve(); rgb2spec is not read by a volume entry
};

AR_FN VmStatus medium_eval(const MediumTables& t, const MediumDesc& desc, Real3 p, const Lambda& lambda_rest, VmRegs* regs,
                           MediumSample* out) {
  *out = medium_sample_zero();
  const MediumBlock& b = t.blocks[desc.program];
  if (b.entry == 0u) return VmStatus::OK;
  ShadeContext ctx = {};
  ctx.P = p;
  ctx.lambda_t = lambda_rest;
  return run_volume_entry(t.programs, b.entry, regs, ctx, t.scene, &b, out);
}

AR_NS_END

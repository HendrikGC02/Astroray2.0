// SPDX-License-Identifier: Apache-2.0
// The scene's media: ar_medium_desc to a compiled medium block, kept keyed beside the lamps and
// replaced with the edit that sets it. The device upload and the scene's MediumDesc table are the
// renderer's (the kernel reads blocks by index); this file owns what a host can say about a medium
// before that: the coefficients, their fold into one MediumBlock by the volume compiler, and the
// reports the fold raised.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md A.1, A.2, D3; 0007-appendix-abi.md
// ar_medium_desc; 0001 D2.3 (staging calls).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../../include/astroray/astroray.h"
#include "../../kernel/color/rgb2spec.h"
#include "../../kernel/medium/medium.h"
#include "../../kernel/medium/medium_block.h"

namespace astroray::host {

class Edit;

// One medium as the scene stores it. `block.entry` is 0 and `program` holds the volume entry's words:
// the table that places them in the medium program array sets the offset. `desc.program` is
// kNoMediumProgram until that table assigns the block's index, so a MediumDesc that was never placed
// cannot be mistaken for medium 0; the upload must refuse it.
struct Medium {
  static constexpr uint32_t kNoMediumProgram = 0xffffffffu;
  ar_key key = 0;
  ar::MediumBlock block = {};
  std::vector<uint32_t> program;
  ar::MediumDesc desc = {};
  std::vector<std::string> approximations;  // one NODE_APPROXIMATED detail each
};

// Throws std::invalid_argument for a coefficient that is not finite or is negative. Flags are not
// looked at here: a non-zero flags word is an AR_ERR_UNSUPPORTED the C entry point raises.
std::shared_ptr<const Medium> build_medium(ar_key key, const ar_medium_desc& d, const ar::Rgb2SpecTable& table);

// ar_edit_set_medium: validates now, stages and commits its own item; the medium joins the scene and
// its approximations are raised when the generation activates.
ar_status set_medium(Edit& edit, ar_key key, const ar_medium_desc* desc, const ar::Rgb2SpecTable& table);

}  // namespace astroray::host

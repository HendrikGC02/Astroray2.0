// SPDX-License-Identifier: Apache-2.0
// The world: ar_world_desc's WORLD graph compiled to a constant radiance (M1a has constant
// worlds only; environment maps are M1b). The graph is compiled by the material compiler (058/059:
// Background colour x strength is lowered to EMIT_LE), so the world follows the same unsupported-node
// policy as a material. A graph whose emission does not fold to a constant raises NODE_UNSUPPORTED
// naming the first node that does not fold, and the world renders as constant magenta radiance
// of strength 1, the world form of 0005 item 40's magenta material. The material store keeps the
// world's per-generation state beside the graphs it depends on (replay after a dropped generation).
// Ref: docs/decisions/0005-material-model.md items 32 (world entry), 35 (folding), 40, 43
// (CONSTANT_EMISSION); 0004 D6 (world row); 0007-appendix-abi.md ar_world_desc.
#pragma once
#include <cstdint>
#include <vector>

#include "astroray/astroray.h"
#include "../../kernel/color/rgb2spec.h"
#include "../../kernel/material/spectrum_ref.h"
#include "host/material/compiler.h"

namespace astroray::host {

struct StoredGraph;

struct WorldOutput {
  ar_world_desc desc = {};
  ar::SpectrumRef color = {};  // ILLUMINANT semantic; radiance = strength * color
  float strength = 0.0f;       // 0: a black world (no world graph, or emission folding to 0)
  bool magenta = false;
  ar_key subject = 0;          // report subject of the entries: the WORLD graph key
  std::vector<MaterialEntry> entries;
};

// `graph` is the stored graph desc.surface_graph names, or null when that key has no graph.
// Never throws on bad input.
WorldOutput compile_world(const ar_world_desc& desc, const StoredGraph* graph, const ar::Rgb2SpecTable& table);

}  // namespace astroray::host

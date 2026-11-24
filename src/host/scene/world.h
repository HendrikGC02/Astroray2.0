// SPDX-License-Identifier: Apache-2.0
// The world: ar_world_desc's WORLD graph compiled by the material compiler (058/059: Background
// colour x strength is lowered to EMIT_LE), so the world follows the same unsupported-node policy as
// a material. A graph whose emission folds to a constant is one radiance (colour x strength, the
// light record's own). Any other emission (an Environment Texture, several emissions) is a program,
// the emission entry of a material program: shade_background runs it per escaped ray with the ray
// direction as the context position, times the record's strength (0005 item 32). A graph that reads
// the UV context raises NODE_UNSUPPORTED naming that node, and the world renders as constant magenta
// radiance of strength 1, the world form of 0005 item 40's magenta material. The material store
// keeps the world's per-generation state beside the graphs it depends on (replay after a dropped
// generation) and recompiles it when an image it samples changes.
// Ref: docs/decisions/0005-material-model.md items 32 (world entry), 35 (folding), 40, 43
// (CONSTANT_EMISSION); 0004 D6 (world row); 0007-appendix-abi.md ar_world_desc.
#pragma once
#include <cstdint>
#include <vector>

#include "astroray/astroray.h"
#include "../../kernel/color/rgb2spec.h"
#include "../../kernel/material/spectrum_ref.h"
#include "../../kernel/material/constant_block.h"
#include "host/material/compiler.h"

namespace astroray::host {

struct StoredGraph;

struct WorldOutput {
  ar_world_desc desc = {};
  ar::SpectrumRef color = {};  // ILLUMINANT semantic; radiance = strength * color
  float strength = 0.0f;       // 0: a black world (no world graph, or emission folding to 0)
  bool magenta = false;
  // A program world (empty for a constant, magenta or black one): the entry table and emission
  // entry of the world's graph, laid out like a material program, and its constant block. The
  // record then carries strength 1 (times the radiance scale), and `color` and `strength` are not
  // radiance.
  std::vector<uint32_t> program;
  ar::ConstantBlock block = {};
  std::vector<uint32_t> images;  // image keys the program samples, sorted: recompile when one changes
  ar_key subject = 0;          // report subject of the entries: the WORLD graph key
  std::vector<MaterialEntry> entries;
};

// `graph` is the stored graph desc.surface_graph names, or null when that key has no graph.
// `means` and `slots` are the image store's (compiler.h); without `slots` every image samples
// magenta, so the caller recompiles such a world once the images are installed. Never throws on bad
// input.
WorldOutput compile_world(const ar_world_desc& desc, const StoredGraph* graph, const ar::Rgb2SpecTable& table,
                          const ImageMeanFn* means = nullptr, const ImageSlotFn* slots = nullptr);

}  // namespace astroray::host

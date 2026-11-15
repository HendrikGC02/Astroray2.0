// SPDX-License-Identifier: Apache-2.0
// The material compiler core: an op graph to bytecode, constant block and SpectrumRefs for the
// surface and emission entries. Folding, dead-code elimination, register allocation and layout
// are here; material flags, the emission estimate and report entries belong to the ABI wiring.
// Ref: docs/decisions/0005-material-model.md items 29-32, 35, 38; Cycles src/scene/svm.cpp and
// shader_graph.cpp (Apache-2.0) for the compile-and-fold structure.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "astroray/astroray.h"
#include "../../kernel/color/rgb2spec.h"
#include "../../kernel/material/constant_block.h"

namespace astroray::host {

struct CompileResult {
  bool ok = false;
  std::string error;  // set when !ok; the compiler never throws
  // Entry table (surface, opacity, emission, volume word offsets; 0 = absent or constant) followed
  // by the entries' code. Empty when the surface folded completely: length 0, CONSTANT_SURFACE.
  std::vector<uint32_t> program;
  bool constant_surface = false;
  ar::ConstantBlock block = {};  // OpenPBR defaults overridden by every constant store
  // ar::kRgbNegativeClamped / kRgbAboveOneClamped seen while packing constants, for the report.
  uint32_t clamp_flags = 0;
};

// Compiles the SURFACE graph. `table` is the working space's RGB-to-spectrum table.
CompileResult compile_surface(const ar_graph_desc& desc, const ar::Rgb2SpecTable& table);

}  // namespace astroray::host

// SPDX-License-Identifier: Apache-2.0
// The material compiler core: an op graph to bytecode, constant block and SpectrumRefs for the
// surface and emission entries. Folding, dead-code elimination, register allocation and layout
// are here; material flags, the emission estimate and report entries belong to the ABI wiring.
// compile_material adds the outputs other stages read (flags, emission estimate) and the
// unsupported-node policy: a material that cannot be compiled is diffuse magenta with its entries.
// Ref: docs/decisions/0005-material-model.md items 29-32, 35, 38, 40, 43; 0001 D2.11 (codes);
// Cycles src/scene/svm.cpp and shader_graph.cpp (Apache-2.0) for the compile-and-fold structure.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "astroray/astroray.h"
#include "../../kernel/color/rgb2spec.h"
#include "../../kernel/material/constant_block.h"

namespace astroray::host {

// One term of the emission estimate: Le = color * strength (0005 item 43). A sum of SpectrumRefs is
// band-summable: each term integrates over a wavelength band on its own.
struct EmissionTerm {
  ar::SpectrumRef color;
  float strength = 0.0f;
};

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
  uint64_t runtime_slots = 0;           // bit s: RecordSlot s is stored by the program, per point
  std::vector<EmissionTerm> emission;   // one term per EMIT_LE that does not fold to 0
};

// A report entry the compiler owes for one material (subject = the material key).
struct MaterialEntry {
  uint32_t code = 0;  // AR_REPORT_*
  std::string detail;
};

struct MaterialOutput {
  CompileResult compiled;  // ok is true: a material that failed is the magenta one
  uint32_t flags = 0;      // material_header.h bits
  std::vector<EmissionTerm> emission_estimate;  // empty: the estimate is 0
  std::vector<MaterialEntry> entries;           // one per (code, detail), never duplicated
  bool magenta = false;
};

// Compiles the SURFACE graph. `table` is the working space's RGB-to-spectrum table.
CompileResult compile_surface(const ar_graph_desc& desc, const ar::Rgb2SpecTable& table);

// compile_surface plus the policy of 0005 items 40 and 43: UNSUPPORTED or unknown ops give
// NODE_UNSUPPORTED (one per node type), a compile error or a SpectrumRef kind M1a cannot resolve
// gives PROGRAM_REJECTED, and either makes the material magenta; a clamped colour gives
// NODE_APPROXIMATED. Never throws on bad input.
MaterialOutput compile_material(const ar_graph_desc& desc, const ar::Rgb2SpecTable& table);

// The material_header.h flags of a compiled program (0005 item 43).
uint32_t material_flags(const CompileResult& r);

// The magenta material (0005 item 40) with one entry: for rejections found outside the graph.
MaterialOutput magenta_material(const ar::Rgb2SpecTable& table, uint32_t code, const std::string& detail);

}  // namespace astroray::host

// SPDX-License-Identifier: Apache-2.0
// The material compiler core: an op graph to bytecode, constant block and SpectrumRefs for the
// surface and emission entries. Folding, dead-code elimination, register allocation and layout
// are here; material flags, the emission estimate and report entries belong to the ABI wiring.
// compile_material adds the outputs other stages read (flags, emission estimate) and the
// unsupported-node policy: a material that cannot be compiled is diffuse magenta with its entries.
// Ref: docs/decisions/0005-material-model.md items 29-32, 35, 38, 40, 43; 0001 D2.11 (codes);
// Cycles src/scene/svm.cpp and shader_graph.cpp (Apache-2.0) for the compile-and-fold structure.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "astroray/astroray.h"
#include "../../kernel/color/rgb2spec.h"
#include "../../kernel/material/constant_block.h"

namespace astroray::host {

// The linear-RGB mean of the image a TEX_IMAGE immediate names (the image's ar_key). False when
// the scene has no such image: the lookup then renders magenta, and the estimate uses magenta.
using ImageMeanFn = std::function<bool(uint32_t image, std::array<float, 3>* rgb)>;
// The image store's slot for a TEX_IMAGE key: false when no image is set under it. The graph
// names images by key; only the compiled program holds slots, and the kernel never searches.
using ImageSlotFn = std::function<bool(uint32_t image, uint32_t* slot)>;

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
  // by the entries' code in the order surface, emission, opacity; the opacity entry exists only for
  // a per-point geometry_opacity. Empty when the surface folded completely: CONSTANT_SURFACE.
  std::vector<uint32_t> program;
  bool constant_surface = false;
  ar::ConstantBlock block = {};  // OpenPBR defaults overridden by every constant store
  // ar::kRgbNegativeClamped / kRgbAboveOneClamped seen while packing constants, for the report.
  uint32_t clamp_flags = 0;
  bool opacity_clamped = false;  // a constant geometry_opacity outside [0, 1] was clamped
  uint64_t runtime_slots = 0;           // bit s: RecordSlot s is stored by the program, per point
  std::vector<EmissionTerm> emission;   // one term per EMIT_LE that does not fold to 0
  // Images the program samples or the emission estimate read, sorted: the material recompiles
  // when one of them is set, replaced or removed. Compiled without an ImageMeanFn they count as
  // mean 1, and without an ImageSlotFn they sample magenta, so the caller recompiles such a
  // result with both before using it.
  std::vector<uint32_t> images;
  std::vector<uint32_t> missing_images;  // sampled keys the ImageSlotFn did not know, sorted
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
  std::vector<uint32_t> images;                 // CompileResult::images; recompile when one changes
  std::vector<MaterialEntry> entries;           // one per (code, detail), never duplicated
  bool magenta = false;
};

// Compiles the SURFACE graph. `table` is the working space's RGB-to-spectrum table.
CompileResult compile_surface(const ar_graph_desc& desc, const ar::Rgb2SpecTable& table, const ImageMeanFn* means = nullptr,
                              const ImageSlotFn* slots = nullptr);

// compile_surface plus the policy of 0005 items 40 and 43: UNSUPPORTED or unknown ops give
// NODE_UNSUPPORTED (one per node type), a compile error (a NaN opacity among them) or a SpectrumRef
// kind M1a cannot resolve gives PROGRAM_REJECTED, and either makes the material magenta; a clamped
// colour or constant opacity gives NODE_APPROXIMATED; a sampled image key that is not set gives IMAGE_UNREADABLE and samples
// magenta. Never throws on bad input.
//
// It also calls set_medium_fields with `medium_priority`.
MaterialOutput compile_material(const ar_graph_desc& desc, const ar::Rgb2SpecTable& table, const ImageMeanFn* means = nullptr,
                                const ImageSlotFn* slots = nullptr, uint32_t medium_priority = 0);

// Fills the block's medium-stack fields (0006 C.17, 0005 item 2a): interior_ior is the constant
// specular_ior; a per-point one (runtime_slots) has no constant source, so it stores 1.0 and
// reports NODE_APPROXIMATED once. medium_priority is stored, above 255 clamped and reported.
void set_medium_fields(MaterialOutput* m, uint32_t medium_priority);

// The material_header.h flags of a compiled program (0005 item 43).
uint32_t material_flags(const CompileResult& r);

// The magenta material (0005 item 40) with one entry: for rejections found outside the graph.
MaterialOutput magenta_material(const ar::Rgb2SpecTable& table, uint32_t code, const std::string& detail);

}  // namespace astroray::host

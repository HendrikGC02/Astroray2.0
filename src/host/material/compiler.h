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
#include "../../kernel/material/ext_params.h"
#include "../../kernel/medium/medium_block.h"

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

// What one BSDF child of a Mix Shader graph stores: the OpenPBR defaults overridden by its constant
// stores (the program holds them as code; this copy is for the material flags) and the slots it
// stores per point.
struct ChildConstants {
  int id = 0;  // the graph's child id
  ar::ConstantBlock block = {};
  uint64_t runtime_slots = 0;
  bool reachable = true;  // false: its selection weight folded to 0, so no sample takes it
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
  uint32_t ext_kind = 0;         // ar::ExtKind; 0 for an OpenPBR material
  ar::ExtParams ext = {};        // the device words of ext_kind, derived from the validated authored block
  // GRATING: the table build_grating_table made from the authored inputs (grating_table.h's layout),
  // host only (0005 item 24); the routing task places it in the TableArena. Empty otherwise.
  std::vector<float> ext_table;
  // ar::kRgbNegativeClamped / kRgbAboveOneClamped seen while packing constants, for the report.
  uint32_t clamp_flags = 0;
  bool opacity_clamped = false;  // a constant geometry_opacity outside [0, 1] was clamped
  // The spectrum arena of this material: the TABLE refs of `block` index it by float offset (index of
  // the first wavelength, aux = n; the n wavelengths then the n values), deduplicated by content. The
  // material store appends it to the scene's arena and relocates the indices (material_store.cpp).
  std::vector<float> spectra;
  // A TABLE in a REFLECTANCE slot whose scaled value exceeds 1 (the kernel clamps it), and the
  // support of each TABLE that does not cover the observer range, for NODE_APPROXIMATED and
  // SPECTRUM_OUT_OF_RANGE (one entry each per material).
  bool spectrum_clamped = false;
  std::string spectrum_range;
  uint64_t runtime_slots = 0;           // bit s: RecordSlot s is stored by the program, per point
  std::vector<EmissionTerm> emission;   // one term per EMIT_LE that does not fold to 0
  // Images the program samples or the emission estimate read, sorted: the material recompiles
  // when one of them is set, replaced or removed. Compiled without an ImageMeanFn they count as
  // mean 1, and without an ImageSlotFn they sample magenta, so the caller recompiles such a
  // result with both before using it.
  std::vector<uint32_t> images;
  std::vector<uint32_t> missing_images;  // sampled keys the ImageSlotFn did not know, sorted
  // A Mix Shader graph's BSDF children in layout order (0005 item 38); empty without a Mix. Their
  // constants are code in their blocks, so `block` then holds only the OpenPBR defaults and a
  // constant effective opacity. Emission and Transparent children are pruned and not listed.
  std::vector<ChildConstants> children;
  // A Transparent child under a Mix has an opacity other than 0: the lowering's 1 - mean colour
  // (item 38), reported NODE_APPROXIMATED. The graph carries only that scalar, so a grey colour,
  // which is exact, cannot be told from a coloured one and is reported too.
  bool transparent_folded = false;
};

// A report entry the compiler owes for one material (subject = the material key).
struct MaterialEntry {
  uint32_t code = 0;  // AR_REPORT_*
  std::string detail;
};

// A compiled volume graph (0006 A.1): the medium's constant block and its volume entry. The
// store places the words in the medium program array and sets block.entry.
struct MediumOutput {
  ar::MediumBlock block = {};
  std::vector<uint32_t> program;
};

struct MaterialOutput {
  CompileResult compiled;  // ok is true: a material that failed is the magenta one
  bool has_medium = false;  // the material's volume graph compiled to a medium
  MediumOutput medium;
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

// What a VOLUME graph compiles to. No medium and no entries is a graph with no volume node; no
// medium with entries is a volume that is reported and not rendered (0006 A.4, D7), and the
// surface is unaffected.
struct VolumeOutput {
  bool has_medium = false;
  MediumOutput medium;
  std::vector<MaterialEntry> entries;
};

// Compiles the VOLUME graph: Volume Absorption and Volume Scatter terms (the VOLUME_COEFFS graph
// node) with constant inputs fold to one MediumBlock: one absorption term and up to two scatter
// lobes (0006 A.2). An op with no volume meaning or a non-constant input gives PROGRAM_REJECTED
// naming the node; a third lobe or a second absorber gives NODE_UNSUPPORTED; a clamped colour,
// density or anisotropy gives NODE_APPROXIMATED, once each. Never throws on bad input.
VolumeOutput compile_volume(const ar_graph_desc& desc, const ar::Rgb2SpecTable& table);

// An extension material (0005 items 14-16, 19): compile_material of its graph, then the kind's block.
// desc.ext_kind must be non-zero. Kind 1 (GRATING) gives EXTENSION, MAY_BE_DELTA and compiled.ext
// (the authored block of desc.ext_constants, grating_authored.h, parsed; its table is compiled.ext_table); a STORE_SLOT to a BSDF slot the kind does not keep is removed before compiling and
// reported INPUT_IGNORED once per slot. A kind that is not built (2-5) or not in the list is
// PROGRAM_REJECTED magenta. The store does not yet route the result (material_store.cpp).
MaterialOutput compile_extension(const ar_material_desc& desc, const ar_graph_desc& graph, const ar::Rgb2SpecTable& table,
                                 const ImageMeanFn* means = nullptr, const ImageSlotFn* slots = nullptr);

// The kind's check of its device words (GRATING: pitch, orders, mode, and in transmission the substrate
// ref); false and `*why` when they are bad. compile_extension runs it on the words it derives.
bool ext_params_valid(uint32_t kind, const ar::ExtParams& e, std::string* why);

// The material_header.h flags of a compiled program (0005 item 43).
uint32_t material_flags(const CompileResult& r);

// The magenta material (0005 item 40) with one entry: for rejections found outside the graph.
MaterialOutput magenta_material(const ar::Rgb2SpecTable& table, uint32_t code, const std::string& detail);

}  // namespace astroray::host

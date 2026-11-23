// SPDX-License-Identifier: Apache-2.0
// The scene's materials: the stored SURFACE graphs and material descriptors, their compiled
// outputs, and the arrays the kernels read (headers, program words, constant blocks). An edit
// stages one keyed item per graph or material (so keys are known and removable) and one internal
// pass that, at commit, collects the materials to compile: those set in the edit and those whose
// graph the edit set or removed. The pass compiles on a scene worker; activation installs the
// results, rebuilds the arrays (host memory, ledger category TABLES; the device upload is 102's)
// and raises each material's report entries. Recompiling a material changes data only, never a
// kernel.
// Ref: docs/decisions/0005-material-model.md items 2, 32, 40, 43; 0007 decisions 8, 9;
// 0001 D2.11 (entries, SCENE scope).
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "astroray/astroray.h"
#include "../../kernel/color/rgb2spec.h"
#include "../../kernel/material/constant_block.h"
#include "../../kernel/material/material_header.h"
#include "host/image/image_store.h"
#include "host/ledger.h"
#include "host/material/compiler.h"
#include "host/scene/world.h"

namespace astroray::host {

class Edit;
class Scene;

// An ar_graph_desc with its arrays owned. desc() points into this object.
struct StoredGraph {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f32;
  std::vector<int32_t> i32;
  std::vector<uint8_t> strings;
  ar_graph_desc desc() const;
};

// The arrays of the active materials. headers[i] belongs to keys[i]; program_offset counts words
// into `programs` (a fully constant material appends none: CONSTANT_SURFACE and CONSTANT_EMISSION
// say the entry is never run), const_offset indexes `constants`.
// headers, constants and pass_index are the device layout and hold one entry more than keys: the
// default material of an instance with no material slice (0007 appendix), last, at index
// keys.size() (where hit_material_id puts it). It is compiled from an empty surface graph, so
// every OpenPBR input is at its default, it emits nothing, its program is empty and its pass
// index is 0. emission_estimates and emission_sampling are per keyed material only.
struct MaterialArrays {
  std::vector<ar_key> keys;
  std::vector<ar::MaterialHeader> headers;
  std::vector<uint32_t> programs;
  std::vector<ar::ConstantBlock> constants;
  std::vector<std::vector<EmissionTerm>> emission_estimates;  // per header; empty = 0
  std::vector<uint32_t> emission_sampling;  // per header: ar_material_desc's AR_EMISSION_SAMPLING_*
  std::vector<uint32_t> pass_index;         // per header: ar_material_desc::pass_index (IndexMA)
  // Distinct surface programs among the keyed headers (sort.h sort_program_count): the material
  // sort's threshold (RULINGS 80), counted once here so no pass reads the headers back.
  uint32_t program_count = 0;
  uint64_t bytes() const;
};

class MaterialStore : public std::enable_shared_from_this<MaterialStore> {
 public:
  // Loads the working space's RGB-to-spectrum table (Rec.709, the only one in M1a); throws
  // std::runtime_error when the embedded table is missing or bad.
  explicit MaterialStore(std::shared_ptr<Ledger> ledger);
  ~MaterialStore();
  MaterialStore(const MaterialStore&) = delete;
  MaterialStore& operator=(const MaterialStore&) = delete;

  // Edit calls. A status other than AR_OK leaves its reason in edit.last_error().
  ar_status set_graph(Edit& edit, ar_key key, const ar_graph_desc* desc);
  ar_status set_material(Edit& edit, ar_key key, const ar_material_desc* desc);
  // Images live in their own store but activate with the material pass: a material that samples
  // an image (or whose emission estimate read one) is recompiled when that key is set, replaced
  // or removed, so its program always holds the slot of the image active under the key.
  ar_status set_image(Edit& edit, const ar_image_desc* desc);
  // PIXELS: stages the image's floats (ar_edit_image_begin); ar_edit_item_commit queues the decode.
  ar_status image_begin(Edit& edit, const ar_image_desc* desc, ar_image_buffers* out);
  ImageStore& images() { return *images_; }
  // ar_edit_set_world: the world recompiles in this edit's pass, and again whenever its graph is
  // set or removed (as a material whose graph changes).
  ar_status set_world(Edit& edit, const ar_world_desc* desc);
  // After a successful ar_edit_remove / ar_edit_reset of the open edit.
  ar_status note_remove(Edit& edit, uint32_t kind, ar_key key);
  ar_status note_reset(Edit& edit);
  // The open edit was committed or aborted.
  void end_edit();

  std::shared_ptr<const MaterialArrays> arrays() const;
  // The active world, null before the first set_world or after a reset.
  std::shared_ptr<const WorldOutput> world() const;
  const ar::Rgb2SpecTable& table() const { return table_; }

 private:
  struct Open;
  struct Pass;
  using GraphMap = std::map<ar_key, std::shared_ptr<const StoredGraph>>;
  using DescMap = std::map<ar_key, ar_material_desc>;
  // One edit's changes to a graph map and a material map: reset, removes, then sets.
  static void apply_open(const Open& o, GraphMap& graphs, DescMap& materials);
  // Whether the edit compiles this material: set in it, or its graph set or removed in it.
  static bool compiles(const Open& o, ar_key key, const ar_material_desc& d);
  // Whether the edit set, replaced or removed (or reset away) an image the material reads.
  static bool uses_changed_image(const MaterialOutput& out, const Open& o);
  // The world as of `o` applied to `world` (unset: none), and whether `o` recompiles it.
  static void apply_world(const Open& o, std::optional<ar_world_desc>& world);
  static bool world_compiles(const Open& o, const std::optional<ar_world_desc>& world);
  ar_status ensure_pass(Edit& edit);
  void bind(const std::shared_ptr<Pass>& pass);
  void dropped(const Pass& pass);
  void activate(Scene& scene, const Pass& pass);
  void rebuild_locked();

  ar::Rgb2SpecTable table_ = {};
  std::shared_ptr<Ledger> ledger_;
  std::shared_ptr<ImageStore> images_;
  std::shared_ptr<Open> open_;  // one open edit per scene

  mutable std::mutex mutex_;
  // As of the latest commit (queued generations included), for what the next commit compiles;
  // rebuilt from the active state plus the queued passes when a generation is dropped.
  GraphMap latest_graphs_;
  DescMap latest_materials_;
  std::optional<ar_world_desc> latest_world_, world_;  // latest committed; active
  std::shared_ptr<const WorldOutput> world_out_;
  std::deque<std::shared_ptr<Pass>> queued_;  // committed, neither active nor dropped, in order
  // As of the active generation.
  GraphMap graphs_;
  struct Active {
    ar_material_desc desc;
    std::shared_ptr<const MaterialOutput> out;
  };
  std::map<ar_key, Active> materials_;
  MaterialOutput default_;  // the default material's compiled output, appended by rebuild_locked
  std::shared_ptr<const MaterialArrays> arrays_;
  uint64_t ledger_id_ = 0;
};

}  // namespace astroray::host

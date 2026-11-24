// SPDX-License-Identifier: Apache-2.0
#include "host/material/material_store.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "host/data_resource.h"
#include "host/light/light_builder.h"
#include "host/scene/edit.h"
#include "kernel/stages/sort.h"

namespace astroray::host {
namespace {

template <class T>
bool copy_in(const T* p, uint32_t n, std::vector<T>* out) {
  if (n != 0 && p == nullptr) return false;
  out->assign(p, p + n);
  return true;
}

std::string key_text(ar_key k) { return "material " + std::to_string(k); }

// One material's outputs from its descriptor and the graph its surface key names (null: none).
MaterialOutput compile_one(const ar::Rgb2SpecTable& t, const ar_material_desc& d, const StoredGraph* g, const StoredGraph* vg,
                           const ImageMeanFn* means, const ImageSlotFn* slots) {
  MaterialOutput m;
  if (d.ext_kind != 0)
    m = magenta_material(t, AR_REPORT_PROGRAM_REJECTED, "extension kind " + std::to_string(d.ext_kind) + " is not built in M1a");
  else if (g == nullptr)
    m = magenta_material(t, AR_REPORT_PROGRAM_REJECTED,
                         d.surface_graph == 0 ? std::string("no surface graph")
                                              : "surface graph " + std::to_string(d.surface_graph) + " is not set");
  else
    m = compile_material(g->desc(), t, means, slots, d.medium_priority);
  // The volume graph compiles to a medium beside the surface; one that cannot is reported and not
  // rendered, and the surface still renders (0006 A.4). Displacement is a later milestone.
  if (d.volume_graph != 0 && vg == nullptr) {
    m.entries.push_back({AR_REPORT_PROGRAM_REJECTED, "volume graph " + std::to_string(d.volume_graph) + " is not set"});
  } else if (d.volume_graph != 0) {
    VolumeOutput v = compile_volume(vg->desc(), t);
    m.has_medium = v.has_medium;
    m.medium = std::move(v.medium);
    for (MaterialEntry& e : v.entries) m.entries.push_back(std::move(e));
  }
  if (d.displacement_graph != 0) m.entries.push_back({AR_REPORT_NODE_UNSUPPORTED, "displacement graph"});
  return m;
}

}  // namespace

ar_graph_desc StoredGraph::desc() const {
  ar_graph_desc d = {};
  d.node_count = uint32_t(nodes.size());
  d.link_count = uint32_t(links.size());
  d.f32_count = uint32_t(f32.size());
  d.i32_count = uint32_t(i32.size());
  d.string_bytes_count = uint32_t(strings.size());
  d.nodes = nodes.data();
  d.links = links.data();
  d.params_f32 = f32.data();
  d.params_i32 = i32.data();
  d.string_bytes = strings.data();
  return d;
}

uint64_t MaterialArrays::bytes() const {
  return headers.size() * sizeof(ar::MaterialHeader) + programs.size() * sizeof(uint32_t) +
         constants.size() * sizeof(ar::ConstantBlock) + pass_index.size() * sizeof(uint32_t) +
         media.size() * sizeof(ar::MediumBlock) + medium_programs.size() * sizeof(uint32_t);
}

struct MaterialStore::Open {
  bool reset = false;
  bool pass_staged = false;
  std::map<ar_key, std::shared_ptr<const StoredGraph>> graphs;
  std::map<ar_key, ar_material_desc> materials;
  std::set<ar_key> removed_graphs, removed_materials;
  std::set<ar_key> images, removed_images;  // image keys this edit set and removed
  std::optional<ar_world_desc> world;  // set in this edit
};

struct MaterialStore::Pass {
  std::shared_ptr<Open> open;
  uint64_t generation = 0;
  struct Item {
    ar_key key = 0;
    ar_material_desc desc = {};
    std::shared_ptr<const StoredGraph> graph, vgraph;  // surface graph and volume graph
    std::shared_ptr<const MaterialOutput> out;
  };
  std::vector<Item> items;  // compiled on the worker from the latest state as bind predicted it
  bool world = false;       // the world recompiles: predicted desc and graph, and the output
  ar_world_desc world_desc = {};
  std::shared_ptr<const StoredGraph> world_graph;
  std::shared_ptr<const WorldOutput> world_out;
};

MaterialStore::MaterialStore(std::shared_ptr<Ledger> ledger)
    : ledger_(std::move(ledger)), images_(std::make_shared<ImageStore>(ledger_)) {
  DataBlob blob;
  if (!find_data("spectral/rec709.jh", &blob)) throw std::runtime_error("material store: spectral/rec709.jh is not embedded");
  if (const char* e = ar::jh_view(blob.data, blob.size, &table_)) throw std::runtime_error(std::string("material store: ") + e);
  // The default material: an empty surface graph leaves every input at its OpenPBR default and
  // folds to a constant surface with no emission. Anything else is a compiler bug.
  default_ = compile_material(ar_graph_desc{}, table_, nullptr, nullptr);
  if (default_.magenta || !default_.compiled.ok || !default_.compiled.constant_surface ||
      !default_.compiled.program.empty() || !default_.emission_estimate.empty() || !default_.entries.empty())
    throw std::runtime_error("material store: the default material did not compile to a constant, non-emissive surface");
  std::lock_guard<std::mutex> lock(mutex_);
  rebuild_locked();
}

MaterialStore::~MaterialStore() {
  if (ledger_id_ != 0) ledger_->remove(ledger_id_);
}

ar_status MaterialStore::ensure_pass(Edit& edit) {
  if (!open_) open_ = std::make_shared<Open>();
  if (open_->pass_staged) return AR_OK;
  auto pass = std::make_shared<Pass>();
  pass->open = open_;
  auto item = std::make_shared<EditItem>();
  item->needs_device = false;
  std::shared_ptr<MaterialStore> self = shared_from_this();
  Scene* scene = &edit.scene();
  item->bind = [self, pass, scene](const std::function<bool(ar_key)>&) {
    pass->generation = scene->generations().pending() + 1;  // one open edit: the next number is this commit's
    self->bind(pass);
  };
  item->job = [self, pass] {
    for (Pass::Item& it : pass->items) {
      it.out = std::make_shared<const MaterialOutput>(compile_one(self->table_, it.desc, it.graph.get(), it.vgraph.get(), nullptr, nullptr));
      // An emission sampling value outside Blender's five, or an estimate the light table cannot
      // weigh, is a bad material and rejects the generation.
      try {
        emission_sidedness(it.desc.emission_sampling, it.out->emission_estimate);
      } catch (const std::invalid_argument& e) {
        throw ItemFailure(it.key, e.what());
      }
    }
    if (pass->world)
      pass->world_out = std::make_shared<const WorldOutput>(compile_world(pass->world_desc, pass->world_graph.get(), self->table_));
  };
  item->on_active = [self, pass, scene] { self->activate(*scene, *pass); };
  item->on_dropped = [self, pass] { self->dropped(*pass); };
  if (ar_status st = edit.stage_internal(item)) return st;
  open_->pass_staged = true;
  return AR_OK;
}

// The stored object is built and the pass staged before the key is committed, and open_ is
// written last, so a failure or a throw never leaves a committed key without its data.
ar_status MaterialStore::set_graph(Edit& edit, ar_key key, const ar_graph_desc* d) {
  if (d == nullptr) return edit.fail("invalid argument: ar_edit_set_graph desc is null");
  auto g = std::make_shared<StoredGraph>();
  if (!copy_in(d->nodes, d->node_count, &g->nodes) || !copy_in(d->links, d->link_count, &g->links) ||
      !copy_in(d->params_f32, d->f32_count, &g->f32) || !copy_in(d->params_i32, d->i32_count, &g->i32) ||
      !copy_in(d->string_bytes, d->string_bytes_count, &g->strings))
    return edit.fail("invalid argument: graph array pointer is null with a nonzero count");
  if (!g->strings.empty() && g->strings.back() != 0)
    return edit.fail("invalid argument: graph string_bytes does not end with NUL");
  auto item = std::make_shared<EditItem>();
  item->key = key;
  item->kind = AR_KIND_GRAPH;
  item->needs_device = false;
  item->job = [] {};  // the material pass compiles; a graph alone has nothing to build
  if (ar_status st = ensure_pass(edit)) return st;
  if (ar_status st = edit.stage(item)) return st;
  if (ar_status st = edit.item_commit(key)) return st;
  open_->graphs[key] = std::move(g);
  return AR_OK;
}

ar_status MaterialStore::set_material(Edit& edit, ar_key key, const ar_material_desc* d) {
  if (d == nullptr) return edit.fail("invalid argument: ar_edit_set_material desc is null");
  const ar_material_desc copy = *d;
  auto item = std::make_shared<EditItem>();
  item->key = key;
  item->kind = AR_KIND_MATERIAL;
  item->needs_device = false;
  item->job = [] {};  // compiled by the pass, which sees every graph of the edit
  if (ar_status st = ensure_pass(edit)) return st;
  if (ar_status st = edit.stage(item)) return st;
  if (ar_status st = edit.item_commit(key)) return st;
  open_->materials[key] = copy;
  return AR_OK;
}

// Keyless like the camera: the world is part of the material pass, which sees every graph.
ar_status MaterialStore::set_world(Edit& edit, const ar_world_desc* d) {
  if (d == nullptr) return edit.fail("invalid argument: ar_edit_set_world desc is null");
  const ar_world_desc copy = *d;
  if (ar_status st = ensure_pass(edit)) return st;
  open_->world = copy;
  return AR_OK;
}

ar_status MaterialStore::set_image(Edit& edit, const ar_image_desc* d) {
  if (d == nullptr) return edit.fail("invalid argument: ar_edit_set_image desc is null");
  if (ar_status st = ensure_pass(edit)) return st;
  if (ar_status st = images_->set_image(edit, d)) return st;
  open_->images.insert(d->key);
  return AR_OK;
}

ar_status MaterialStore::image_begin(Edit& edit, const ar_image_desc* d, ar_image_buffers* out) {
  if (d == nullptr) return edit.fail("invalid argument: ar_edit_image_begin desc is null");
  if (ar_status st = ensure_pass(edit)) return st;
  if (ar_status st = images_->begin_pixels(edit, d, out)) return st;
  open_->images.insert(d->key);
  return AR_OK;
}

bool MaterialStore::uses_changed_image(const MaterialOutput& out, const Open& o) {
  for (uint32_t k : out.images)
    if (o.reset || o.images.count(k) != 0 || o.removed_images.count(k) != 0) return true;
  return false;
}

ar_status MaterialStore::note_remove(Edit& edit, uint32_t kind, ar_key key) {
  if (kind != AR_KIND_MATERIAL && kind != AR_KIND_GRAPH && kind != AR_KIND_IMAGE) return AR_OK;
  if (ar_status st = ensure_pass(edit)) return st;
  (kind == AR_KIND_MATERIAL ? open_->removed_materials : kind == AR_KIND_GRAPH ? open_->removed_graphs : open_->removed_images).insert(key);
  return AR_OK;
}

ar_status MaterialStore::note_reset(Edit& edit) {
  if (ar_status st = ensure_pass(edit)) return st;
  open_->reset = true;
  return AR_OK;
}

void MaterialStore::end_edit() { open_.reset(); }

void MaterialStore::apply_open(const Open& o, GraphMap& graphs, DescMap& materials) {
  if (o.reset) graphs.clear(), materials.clear();
  for (ar_key k : o.removed_graphs)
    if (!o.graphs.count(k)) graphs.erase(k);
  for (ar_key k : o.removed_materials)
    if (!o.materials.count(k)) materials.erase(k);
  for (const auto& kv : o.graphs) graphs[kv.first] = kv.second;
  for (const auto& kv : o.materials) materials[kv.first] = kv.second;
}

void MaterialStore::apply_world(const Open& o, std::optional<ar_world_desc>& world) {
  if (o.reset) world.reset();
  if (o.world) world = o.world;
}

bool MaterialStore::world_compiles(const Open& o, const std::optional<ar_world_desc>& world) {
  if (o.world) return true;
  return world && (o.graphs.count(world->surface_graph) != 0 || o.removed_graphs.count(world->surface_graph) != 0);
}

bool MaterialStore::compiles(const Open& o, ar_key key, const ar_material_desc& d) {
  return o.materials.count(key) != 0 || o.graphs.count(d.surface_graph) != 0 || o.removed_graphs.count(d.surface_graph) != 0 ||
         (d.volume_graph != 0 && (o.graphs.count(d.volume_graph) != 0 || o.removed_graphs.count(d.volume_graph) != 0));
}

// At commit: what the edit compiles, predicted from the latest state (every queued generation
// assumed to activate): its own materials and the others whose graph it set or removed.
void MaterialStore::bind(const std::shared_ptr<Pass>& pass) {
  const Open& o = *pass->open;
  std::lock_guard<std::mutex> lock(mutex_);
  apply_open(o, latest_graphs_, latest_materials_);
  apply_world(o, latest_world_);
  queued_.push_back(pass);
  if (latest_world_ && world_compiles(o, latest_world_)) {
    pass->world = true;
    pass->world_desc = *latest_world_;
    auto gi = latest_graphs_.find(latest_world_->surface_graph);
    if (gi != latest_graphs_.end()) pass->world_graph = gi->second;
  }
  for (const auto& kv : latest_materials_) {
    if (!compiles(o, kv.first, kv.second)) continue;
    Pass::Item it;
    it.key = kv.first;
    it.desc = kv.second;
    auto gi = latest_graphs_.find(kv.second.surface_graph);
    if (gi != latest_graphs_.end()) it.graph = gi->second;
    auto vi = latest_graphs_.find(kv.second.volume_graph);
    if (kv.second.volume_graph != 0 && vi != latest_graphs_.end()) it.vgraph = vi->second;
    pass->items.push_back(std::move(it));
  }
}

// A dropped generation never happened: the latest state is the active one with the generations
// still queued replayed in order, as Scene rebuilds its known keys.
void MaterialStore::dropped(const Pass& pass) {
  std::lock_guard<std::mutex> lock(mutex_);
  queued_.erase(std::remove_if(queued_.begin(), queued_.end(), [&](const auto& p) { return p.get() == &pass; }),
                queued_.end());
  latest_graphs_ = graphs_;
  latest_materials_.clear();
  for (const auto& kv : materials_) latest_materials_[kv.first] = kv.second.desc;
  latest_world_ = world_;
  for (const auto& p : queued_) apply_open(*p->open, latest_graphs_, latest_materials_), apply_world(*p->open, latest_world_);
}

void MaterialStore::rebuild_locked() {
  auto a = std::make_shared<MaterialArrays>();
  for (const auto& kv : materials_) {
    const MaterialOutput& m = *kv.second.out;
    ar::MaterialHeader h = {};
    h.program_offset = uint32_t(a->programs.size());
    h.const_offset = uint32_t(a->constants.size());
    h.ext = 0;  // no extension kind compiles in M1a (compile_one rejects ext_kind != 0)
    h.flags = m.flags;
    a->keys.push_back(kv.first);
    a->headers.push_back(h);
    a->programs.insert(a->programs.end(), m.compiled.program.begin(), m.compiled.program.end());
    a->constants.push_back(m.compiled.block);
    a->emission_estimates.push_back(m.emission_estimate);
    a->emission_sampling.push_back(kv.second.desc.emission_sampling);
    a->pass_index.push_back(kv.second.desc.pass_index);
    // A medium's volume entry goes into its own program array; word 0 is reserved so that an entry
    // offset of 0 means none.
    uint32_t medium = MaterialArrays::kNoMedium;
    if (m.has_medium) {
      if (a->medium_programs.empty()) a->medium_programs.push_back(0u);
      ar::MediumBlock b = m.medium.block;
      b.entry = uint32_t(a->medium_programs.size());
      a->medium_programs.insert(a->medium_programs.end(), m.medium.program.begin(), m.medium.program.end());
      medium = uint32_t(a->media.size());
      a->media.push_back(b);
    }
    a->medium_index.push_back(medium);
  }
  // The default material, last (MaterialArrays): no key, no program words, pass index 0.
  ar::MaterialHeader h = {};
  h.program_offset = uint32_t(a->programs.size());
  h.const_offset = uint32_t(a->constants.size());
  h.flags = default_.flags;
  a->headers.push_back(h);
  a->constants.push_back(default_.compiled.block);
  a->pass_index.push_back(0u);
  a->program_count = ar::sort_program_count(a->headers.data(), uint32_t(a->keys.size()));
  const uint64_t bytes = a->bytes();
  const uint64_t id = bytes != 0 ? ledger_->add(Owner{}, LedgerCategory::TABLES, bytes) : 0;
  if (ledger_id_ != 0) ledger_->remove(ledger_id_);
  ledger_id_ = id;
  arrays_ = std::move(a);
}

void MaterialStore::activate(Scene& scene, const Pass& pass) {
  const Open& o = *pass.open;
  struct Done {
    ar_key key;
    bool own;
    std::shared_ptr<const MaterialOutput> out;
  };
  std::vector<Done> done;
  ar_key old_world = 0;
  std::shared_ptr<const WorldOutput> new_world;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queued_.erase(std::remove_if(queued_.begin(), queued_.end(), [&](const auto& p) { return p.get() == &pass; }),
                  queued_.end());
    DescMap descs;
    for (const auto& kv : materials_) descs[kv.first] = kv.second.desc;
    apply_open(o, graphs_, descs);
    for (auto it = materials_.begin(); it != materials_.end();) it = descs.count(it->first) ? std::next(it) : materials_.erase(it);
    // What to compile follows from the active state. The worker's output is used when it was
    // compiled from the same descriptor and graph; after a dropped generation the prediction can
    // be wrong (another graph, a material that no longer exists, one it missed), and then the
    // material is compiled here.
    // The edit's image removals and the images of every earlier generation are in place; this
    // generation's own images were installed by their items, which run before this pass.
    images_->remove(o.reset, o.removed_images, o.images);
    const ImageMeanFn means = [this](uint32_t image, std::array<float, 3>* rgb) { return images_->mean(image, rgb); };
    const ImageSlotFn slots = [this](uint32_t image, uint32_t* slot) { return images_->slot(image, slot); };
    for (const auto& kv : descs) {
      const bool own_pass = compiles(o, kv.first, kv.second);
      // A material that is not recompiled for its own change still is when an image it samples
      // or its emission estimate read was set, replaced or removed (a key that was not set when
      // it compiled sampled magenta; setting it now gives the material its slot).
      auto prev = materials_.find(kv.first);
      const bool image_dirty = !own_pass && prev != materials_.end() && uses_changed_image(*prev->second.out, o);
      if (!own_pass && !image_dirty) continue;
      auto gi = graphs_.find(kv.second.surface_graph);
      const std::shared_ptr<const StoredGraph> graph = gi != graphs_.end() ? gi->second : nullptr;
      auto vi = graphs_.find(kv.second.volume_graph);
      const std::shared_ptr<const StoredGraph> vgraph =
          kv.second.volume_graph != 0 && vi != graphs_.end() ? vi->second : nullptr;
      std::shared_ptr<const MaterialOutput> out;
      if (own_pass)
        for (const Pass::Item& it : pass.items)
          if (it.key == kv.first && it.graph == graph && it.vgraph == vgraph && std::memcmp(&it.desc, &kv.second, sizeof(ar_material_desc)) == 0)
            out = it.out;
      // The worker compiled without image means and slots (the images may not have been decoded
      // or installed yet); a result that reads images is compiled again here.
      if (out && !out->images.empty()) out.reset();
      if (!out) out = std::make_shared<const MaterialOutput>(compile_one(table_, kv.second, graph.get(), vgraph.get(), &means, &slots));
      materials_[kv.first] = Active{kv.second, out};
      done.push_back({kv.first, o.materials.count(kv.first) != 0, out});
    }
    rebuild_locked();
    // The world, by the same rule: the worker's output when its prediction holds, else compiled here.
    apply_world(o, world_);
    if (!world_) {
      if (world_out_) old_world = world_out_->subject;
      world_out_.reset();
    } else if (world_compiles(o, world_)) {
      auto gi = graphs_.find(world_->surface_graph);
      const std::shared_ptr<const StoredGraph> graph = gi != graphs_.end() ? gi->second : nullptr;
      std::shared_ptr<const WorldOutput> out = pass.world_out;
      if (!out || pass.world_graph != graph || std::memcmp(&pass.world_desc, &*world_, sizeof(ar_world_desc)) != 0)
        out = std::make_shared<const WorldOutput>(compile_world(*world_, graph.get(), table_));
      if (world_out_) old_world = world_out_->subject;
      world_out_ = out;
      new_world = out;
    }
  }
  // A material set in this edit had its old entries removed with its keyed item (the tracker
  // clears replaced subjects before on_active); a dependent is not a subject, so clear it here.
  std::vector<uint64_t> dependents;
  for (const Done& d : done)
    if (!d.own) dependents.push_back(d.key);
  // The world is no keyed item either: its previous entries go before the new ones are raised.
  if (old_world != 0) dependents.push_back(old_world);
  if (new_world && new_world->subject != 0) dependents.push_back(new_world->subject);
  if (!dependents.empty()) scene.report().on_generation_active(pass.generation, dependents, false);
  for (const Done& d : done)
    for (const MaterialEntry& e : d.out->entries) {
      scene.ctx().log.line("WARNING", key_text(d.key) + ": " + e.detail + (d.out->magenta ? " (rendered magenta)" : ""));
      scene.report().raise(e.code, d.key, e.detail, 1, nullptr, pass.generation);
    }
  images_->raise_entries(scene, pass.generation);
  if (new_world)
    for (const MaterialEntry& e : new_world->entries) {
      scene.ctx().log.line("WARNING", "world: " + e.detail + (new_world->magenta ? " (rendered magenta)" : ""));
      scene.report().raise(e.code, new_world->subject, e.detail, 1, nullptr, pass.generation);
    }
}

std::shared_ptr<const WorldOutput> MaterialStore::world() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return world_out_;
}

std::shared_ptr<const MaterialArrays> MaterialStore::arrays() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return arrays_;
}

}  // namespace astroray::host

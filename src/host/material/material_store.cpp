// SPDX-License-Identifier: Apache-2.0
#include "host/material/material_store.h"

#include <stdexcept>
#include <utility>

#include "host/data_resource.h"
#include "host/scene/edit.h"

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
MaterialOutput compile_one(const ar::Rgb2SpecTable& t, const ar_material_desc& d, const StoredGraph* g) {
  MaterialOutput m;
  if (d.ext_kind != 0)
    m = magenta_material(t, AR_REPORT_PROGRAM_REJECTED, "extension kind " + std::to_string(d.ext_kind) + " is not built in M1a");
  else if (g == nullptr)
    m = magenta_material(t, AR_REPORT_PROGRAM_REJECTED,
                         d.surface_graph == 0 ? std::string("no surface graph")
                                              : "surface graph " + std::to_string(d.surface_graph) + " is not set");
  else
    m = compile_material(g->desc(), t);
  // Volume and displacement programs are later milestones: reported, the surface still renders.
  if (d.volume_graph != 0) m.entries.push_back({AR_REPORT_NODE_UNSUPPORTED, "volume graph"});
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
         constants.size() * sizeof(ar::ConstantBlock);
}

struct MaterialStore::Open {
  bool reset = false;
  bool pass_staged = false;
  std::map<ar_key, std::shared_ptr<const StoredGraph>> graphs;
  std::map<ar_key, ar_material_desc> materials;
  std::set<ar_key> removed_graphs, removed_materials;
};

struct MaterialStore::Pass {
  std::shared_ptr<Open> open;
  uint64_t generation = 0;
  struct Item {
    ar_key key = 0;
    ar_material_desc desc = {};
    std::shared_ptr<const StoredGraph> graph;
    bool own = true;  // set in this edit (its keyed item replaces its entries); false: a dependent
    std::shared_ptr<const MaterialOutput> out;
  };
  std::vector<Item> items;
};

MaterialStore::MaterialStore(std::shared_ptr<Ledger> ledger)
    : ledger_(std::move(ledger)), arrays_(std::make_shared<MaterialArrays>()) {
  DataBlob blob;
  if (!find_data("spectral/rec709.jh", &blob)) throw std::runtime_error("material store: spectral/rec709.jh is not embedded");
  if (const char* e = ar::jh_view(blob.data, blob.size, &table_)) throw std::runtime_error(std::string("material store: ") + e);
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
    self->bind(*pass);
  };
  item->job = [self, pass] {
    for (Pass::Item& it : pass->items)
      it.out = std::make_shared<const MaterialOutput>(compile_one(self->table_, it.desc, it.graph.get()));
  };
  item->on_active = [self, pass, scene] { self->activate(*scene, *pass); };
  item->on_dropped = [self] {
    // The latest state falls back to the active one; generations still queued behind a dropped
    // one keep their own results, only the dependents found at a later commit may miss them.
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->latest_graphs_ = self->graphs_;
    self->latest_materials_.clear();
    for (const auto& kv : self->materials_) self->latest_materials_[kv.first] = kv.second.desc;
  };
  if (ar_status st = edit.stage_internal(item)) return st;
  open_->pass_staged = true;
  return AR_OK;
}

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
  if (ar_status st = edit.stage(item)) return st;
  if (ar_status st = edit.item_commit(key)) return st;
  if (ar_status st = ensure_pass(edit)) return st;
  open_->graphs[key] = std::move(g);
  return AR_OK;
}

ar_status MaterialStore::set_material(Edit& edit, ar_key key, const ar_material_desc* d) {
  if (d == nullptr) return edit.fail("invalid argument: ar_edit_set_material desc is null");
  auto item = std::make_shared<EditItem>();
  item->key = key;
  item->kind = AR_KIND_MATERIAL;
  item->needs_device = false;
  item->job = [] {};  // compiled by the pass, which sees every graph of the edit
  if (ar_status st = edit.stage(item)) return st;
  if (ar_status st = edit.item_commit(key)) return st;
  if (ar_status st = ensure_pass(edit)) return st;
  open_->materials[key] = *d;
  return AR_OK;
}

ar_status MaterialStore::note_remove(Edit& edit, uint32_t kind, ar_key key) {
  if (kind != AR_KIND_MATERIAL && kind != AR_KIND_GRAPH) return AR_OK;
  if (ar_status st = ensure_pass(edit)) return st;
  (kind == AR_KIND_MATERIAL ? open_->removed_materials : open_->removed_graphs).insert(key);
  return AR_OK;
}

ar_status MaterialStore::note_reset(Edit& edit) {
  if (ar_status st = ensure_pass(edit)) return st;
  open_->reset = true;
  return AR_OK;
}

void MaterialStore::end_edit() { open_.reset(); }

// At commit: the edit's final graphs and materials, and the materials it did not set whose
// graph it set or removed.
void MaterialStore::bind(Pass& pass) {
  const Open& o = *pass.open;
  std::lock_guard<std::mutex> lock(mutex_);
  if (o.reset) latest_graphs_.clear(), latest_materials_.clear();
  for (ar_key k : o.removed_graphs)
    if (!o.graphs.count(k)) latest_graphs_.erase(k);
  for (ar_key k : o.removed_materials)
    if (!o.materials.count(k)) latest_materials_.erase(k);
  for (const auto& kv : o.graphs) latest_graphs_[kv.first] = kv.second;
  for (const auto& kv : o.materials) latest_materials_[kv.first] = kv.second;
  for (const auto& kv : latest_materials_) {
    const ar_key g = kv.second.surface_graph;
    const bool own = o.materials.count(kv.first) != 0;
    if (!own && !o.graphs.count(g) && !o.removed_graphs.count(g)) continue;
    Pass::Item it;
    it.key = kv.first;
    it.desc = kv.second;
    it.own = own;
    auto gi = latest_graphs_.find(g);
    if (gi != latest_graphs_.end()) it.graph = gi->second;
    pass.items.push_back(std::move(it));
  }
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
  }
  const uint64_t bytes = a->bytes();
  const uint64_t id = bytes != 0 ? ledger_->add(Owner{}, LedgerCategory::TABLES, bytes) : 0;
  if (ledger_id_ != 0) ledger_->remove(ledger_id_);
  ledger_id_ = id;
  arrays_ = std::move(a);
}

void MaterialStore::activate(Scene& scene, const Pass& pass) {
  const Open& o = *pass.open;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (o.reset) graphs_.clear(), materials_.clear();
    for (ar_key k : o.removed_graphs)
      if (!o.graphs.count(k)) graphs_.erase(k);
    for (ar_key k : o.removed_materials)
      if (!o.materials.count(k)) materials_.erase(k);
    for (const auto& kv : o.graphs) graphs_[kv.first] = kv.second;
    for (const Pass::Item& it : pass.items) materials_[it.key] = Active{it.desc, it.out};
    rebuild_locked();
  }
  // A material set in this edit had its old entries removed with its keyed item (the tracker
  // clears replaced subjects before on_active); a dependent is not a subject, so clear it here.
  std::vector<uint64_t> dependents;
  for (const Pass::Item& it : pass.items)
    if (!it.own) dependents.push_back(it.key);
  if (!dependents.empty()) scene.report().on_generation_active(pass.generation, dependents, false);
  for (const Pass::Item& it : pass.items)
    for (const MaterialEntry& e : it.out->entries) {
      scene.ctx().log.line("WARNING", key_text(it.key) + ": " + e.detail + (it.out->magenta ? " (rendered magenta)" : ""));
      scene.report().raise(e.code, it.key, e.detail, 1, nullptr, pass.generation);
    }
}

std::shared_ptr<const MaterialArrays> MaterialStore::arrays() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return arrays_;
}

}  // namespace astroray::host

// SPDX-License-Identifier: Apache-2.0
#include "host/scene/mesh.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "host/geometry/triangulate.h"
#include "host/scene/instances.h"

namespace astroray::host {

namespace {

// A sanity cap on one staged array (elements), against overflow in the size arithmetic;
// it is not a physical limit.
constexpr uint64_t kMaxElements = uint64_t(1) << 40;

uint64_t mul(uint64_t a, uint64_t b, const char* what) {
  if (a != 0 && b > kMaxElements / a) throw std::invalid_argument(std::string("mesh descriptor: ") + what + " is too large");
  return a * b;
}

std::string label_of(const ar_mesh_desc& d) {
  size_t n = strnlen(reinterpret_cast<const char*>(d.name), sizeof(d.name));
  std::string name(reinterpret_cast<const char*>(d.name), n);
  std::string k = "key " + std::to_string(d.key);
  return name.empty() ? "mesh " + k : "mesh '" + name + "' (" + k + ")";
}

struct Counts {
  uint64_t positions, corner_vert, face_start, normals, uv, material_index, attributes;
};

Counts count_arrays(const ar_mesh_desc& d, const std::vector<ar_attribute_desc>& attrs) {
  Counts c{};
  c.positions = mul(mul(d.motion_steps, d.vertex_count, "positions"), 3, "positions");
  c.corner_vert = d.corner_count;
  c.face_start = d.face_count;
  c.normals = mul(d.corner_count, 3, "corner_normals");
  c.uv = mul(mul(d.uv_layer_count, d.corner_count, "uv"), 2, "uv");
  c.material_index = (d.flags & AR_MESH_HAS_MATERIAL_INDEX) ? d.face_count : 0;
  for (const ar_attribute_desc& a : attrs) {
    uint64_t n = a.domain == AR_DOMAIN_POINT ? d.vertex_count : a.domain == AR_DOMAIN_CORNER ? d.corner_count : d.face_count;
    c.attributes += mul(n, a.width, "attributes");
  }
  return c;
}

// The descriptor checks that need no array contents (task 035 Do 1).
void validate_desc(const ar_mesh_desc& d, const std::vector<ar_attribute_desc>& attrs) {
  const std::string who = label_of(d) + ": ";
  if (d.motion_steps < 1) throw std::invalid_argument(who + "motion_steps must be >= 1");
  if (d.flags & ~uint32_t(AR_MESH_HAS_MATERIAL_INDEX | AR_MESH_HAS_ATTRIBUTES))
    throw std::invalid_argument(who + "unknown flag bits");
  if (bool(d.flags & AR_MESH_HAS_ATTRIBUTES) != (d.attribute_count != 0))
    throw std::invalid_argument(who + "attribute_count and AR_MESH_HAS_ATTRIBUTES disagree");
  constexpr uint32_t kMaxCount = 0x7fffffffu;  // corner_vert and face_start are i32
  if (d.vertex_count > kMaxCount || d.corner_count > kMaxCount || d.face_count > kMaxCount)
    throw std::invalid_argument(who + "vertex_count, corner_count and face_count must not exceed INT32_MAX");
  if (d.face_count == 0 && d.corner_count != 0) throw std::invalid_argument(who + "corners without faces");
  if (uint64_t(d.corner_count) < 3 * uint64_t(d.face_count))
    throw std::invalid_argument(who + "corner_count is below 3 x face_count");
  for (size_t i = 0; i < attrs.size(); ++i) {
    if (attrs[i].domain > AR_DOMAIN_FACE) throw std::invalid_argument(who + "attribute " + std::to_string(i) + " has an unsupported domain");
    if (attrs[i].width < 1 || attrs[i].width > 4) throw std::invalid_argument(who + "attribute " + std::to_string(i) + " width must be 1 to 4");
  }
}

struct Staged {
  const float* positions = nullptr;
  const int32_t* corner_vert = nullptr;
  const int32_t* face_start = nullptr;
  const float* normals = nullptr;
  const float* uv = nullptr;
  const int32_t* material_index = nullptr;
  const float* attributes = nullptr;
};

void check_finite(const float* p, uint64_t n, const char* what, uint64_t per_step, const std::string& who) {
  for (uint64_t i = 0; i < n; ++i)
    if (!std::isfinite(p[i])) {
      std::string at = per_step ? " (step " + std::to_string(i / per_step) + ", element " + std::to_string((i % per_step) / 3) + ")"
                                : " (corner " + std::to_string(i / 3) + ")";
      throw std::runtime_error(who + what + "[" + std::to_string(i) + "] is not finite" + at);
    }
}

void compute_bounds(const std::vector<float>& pos, float* lo, float* hi) {
  if (pos.empty()) return;
  for (int a = 0; a < 3; ++a) lo[a] = hi[a] = pos[a];
  for (size_t i = 0; i < pos.size(); i += 3)
    for (int a = 0; a < 3; ++a) {
      lo[a] = std::min(lo[a], pos[i + a]);
      hi[a] = std::max(hi[a], pos[i + a]);
    }
}

template <class V>
uint64_t bytes_of(const V& v) {
  return uint64_t(v.size()) * sizeof(typename V::value_type);
}

std::string key_label(ar_key key) { return "mesh key " + std::to_string(key); }

// Positions and normals of a record, with their ledger entry (none for an empty mesh).
std::shared_ptr<MeshRecord> make_record(ar_key key, std::shared_ptr<const MeshTopology> topo, std::vector<float> positions,
                                        std::vector<float> normals, const std::shared_ptr<Ledger>& ledger) {
  auto r = std::make_shared<MeshRecord>();
  r->key = key;
  r->topo = std::move(topo);
  r->positions = std::move(positions);
  r->corner_normals = std::move(normals);
  compute_bounds(r->positions, r->bounds_min, r->bounds_max);
  r->host_bytes = bytes_of(r->positions) + bytes_of(r->corner_normals);
  if (r->host_bytes) {
    r->ledger = ledger;
    r->ledger_id = ledger->add(Owner{}, LedgerCategory::GEOMETRY, r->host_bytes);
  }
  return r;
}

// Worker job: O(n) checks, triangulation, the record (task 035 Do 2). Throws with the text
// of the first failed check.
std::shared_ptr<MeshRecord> build_record(const ar_mesh_desc& d, const std::vector<ar_attribute_desc>& attrs, const Counts& n,
                                         const Staged& s, uint64_t topo_id, const std::shared_ptr<Ledger>& ledger,
                                         ContextState& ctx) {
  const std::string who = label_of(d) + ": ";
  const uint32_t V = d.vertex_count, C = d.corner_count, F = d.face_count;
  for (uint32_t f = 0; f < F; ++f) {
    int64_t start = s.face_start[f];
    int64_t next = f + 1 < F ? s.face_start[f + 1] : int64_t(C);
    if (f == 0 && start != 0) throw std::runtime_error(who + "face_start[0] = " + std::to_string(start) + ", must be 0");
    if (f + 1 < F && next <= start)
      throw std::runtime_error(who + "face_start[" + std::to_string(f + 1) + "] = " + std::to_string(next) +
                               " is not greater than face_start[" + std::to_string(f) + "] = " + std::to_string(start));
    if (next - start < 3)
      throw std::runtime_error(who + "face " + std::to_string(f) + " has " + std::to_string(next - start) + " corners (< 3)" +
                               (f + 1 == F ? "; the last face_start must be below corner_count" : ""));
  }
  for (uint32_t c = 0; c < C; ++c)
    if (s.corner_vert[c] < 0 || uint32_t(s.corner_vert[c]) >= V)
      throw std::runtime_error(who + "corner_vert[" + std::to_string(c) + "] = " + std::to_string(s.corner_vert[c]) +
                               " is outside [0, " + std::to_string(V) + ")");
  check_finite(s.positions, n.positions, "positions", uint64_t(V) * 3, who);
  check_finite(s.normals, n.normals, "corner_normals", 0, who);
  int64_t max_mat = -1;
  for (uint32_t f = 0; f < n.material_index; ++f) {
    if (s.material_index[f] < 0)
      throw std::runtime_error(who + "material_index[" + std::to_string(f) + "] = " + std::to_string(s.material_index[f]) + " is negative");
    max_mat = std::max<int64_t>(max_mat, s.material_index[f]);
  }

  auto t = std::make_shared<MeshTopology>();
  t->id = topo_id;
  t->key = d.key;
  t->name = label_of(d);
  t->vertex_count = V, t->corner_count = C, t->face_count = F;
  t->uv_layer_count = d.uv_layer_count, t->motion_steps = d.motion_steps;
  t->corner_vert.assign(s.corner_vert, s.corner_vert + C);
  t->face_start.assign(s.face_start, s.face_start + F);
  t->face_start.push_back(C);
  t->uv.assign(s.uv, s.uv + n.uv);
  t->attributes.assign(s.attributes, s.attributes + n.attributes);
  t->attribute_descs = attrs;
  if (F) t->max_material_index = (d.flags & AR_MESH_HAS_MATERIAL_INDEX) ? uint32_t(max_mat) : 0u;

  // Motion centre: the same representative step as the instances (ruling 68), until M8 blends.
  const float* centre = s.positions + size_t(representative_step(d.motion_steps)) * V * 3;
  Triangulation tri = triangulate(centre, t->corner_vert.data(), t->face_start.data(), F);
  t->tri_corners = std::move(tri.corners);
  t->tri_face = std::move(tri.face);
  t->skipped_zero_area = tri.skipped_zero_area, t->fan_fallback = tri.fan_fallback;
  if (tri.skipped_zero_area || tri.fan_fallback)
    ctx.log.line("WARNING", who + std::to_string(tri.skipped_zero_area) + " zero-area face(s) skipped, " +
                                std::to_string(tri.fan_fallback) + " face(s) triangulated as a fan from the first corner");
  t->tri_material.reserve(t->tri_face.size());
  for (uint32_t f : t->tri_face) t->tri_material.push_back((d.flags & AR_MESH_HAS_MATERIAL_INDEX) ? s.material_index[f] : 0);
  uint64_t tb = bytes_of(t->corner_vert) + bytes_of(t->face_start) + bytes_of(t->uv) + bytes_of(t->attributes) +
                bytes_of(t->tri_corners) + bytes_of(t->tri_face) + bytes_of(t->tri_material);
  t->ledger = ledger;
  t->ledger_id = ledger->add(Owner{}, LedgerCategory::GEOMETRY, tb);  // face_start is never empty
  return make_record(d.key, t, std::vector<float>(s.positions, s.positions + n.positions),
                     std::vector<float>(s.normals, s.normals + n.normals), ledger);
}

// A shared slot the job fills and on_active installs.
using ResultSlot = std::shared_ptr<std::shared_ptr<const MeshRecord>>;

struct Blocks {
  std::vector<Arena::Block> list;
};

template <class T>
T** stage_array(Scene& scene, EditItem& item, Blocks& blocks, uint64_t count, const T** raw) {
  T* p = nullptr;
  if (count) {
    Arena::Block b = scene.arena().alloc(size_t(count) * sizeof(T));
    blocks.list.push_back(b);
    p = static_cast<T*>(b.ptr);
  }
  *raw = p;
  return reinterpret_cast<T**>(item.new_slot(p));
}

ar_status stage_item(Scene& scene, Edit& edit, const std::shared_ptr<EditItem>& item, const std::shared_ptr<Blocks>& blocks) {
  Scene* sc = &scene;
  item->release = [sc, blocks] {
    for (const Arena::Block& b : blocks->list) sc->arena().release(b);
    blocks->list.clear();
  };
  ar_status st = edit.stage(item);
  if (st != AR_OK) item->release();
  return st;
}

}  // namespace

namespace {
void release_ledger(const std::shared_ptr<Ledger>& l, uint64_t id) {
  if (l && id) {
    try {
      l->remove(id);
    } catch (...) {
    }
  }
}
}  // namespace

void TopoGate::resolve(std::shared_ptr<const MeshTopology> t) {
  std::vector<std::function<void()>> run;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (done_) return;
    done_ = true;
    topo_ = std::move(t);
    run.swap(waiting_);
  }
  for (auto& fn : run) {
    try {
      fn();  // callbacks handle their own failures; one throwing must not strand the rest
    } catch (...) {
    }
  }
}
void TopoGate::when_done(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!done_) {
      waiting_.push_back(std::move(fn));
      return;
    }
  }
  fn();
}
std::shared_ptr<const MeshTopology> TopoGate::topology() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return topo_;
}

MeshTopology::~MeshTopology() { release_ledger(ledger, ledger_id); }
MeshRecord::~MeshRecord() { release_ledger(ledger, ledger_id); }

void check_position_counts(const MeshTopology& base, uint64_t positions, uint64_t corner_normals) {
  const uint64_t want_p = uint64_t(base.motion_steps) * base.vertex_count * 3, want_n = uint64_t(base.corner_count) * 3;
  if (positions != want_p || corner_normals != want_n)
    throw std::runtime_error(base.name + ": positions-only update has " + std::to_string(positions) + " position and " +
                             std::to_string(corner_normals) + " normal floats, the mesh has " + std::to_string(want_p) + " and " +
                             std::to_string(want_n));
}

std::shared_ptr<const MeshRecord> MeshStore::find(ar_key key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = active_.find(key);
  return it == active_.end() ? nullptr : it->second;
}
size_t MeshStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_.size();
}
std::optional<MeshDeclaration> MeshStore::latest(ar_key key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = declared_.find(key);
  if (it == declared_.end() || it->second.empty()) return std::nullopt;
  return it->second.back();
}
void MeshStore::declare(ar_key key, const MeshDeclaration& d) {
  std::lock_guard<std::mutex> lock(mutex_);
  declared_[key].push_back(d);
}
void MeshStore::undeclare(ar_key key, uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = declared_.find(key);
  if (it == declared_.end()) return;
  auto& v = it->second;
  v.erase(std::remove_if(v.begin(), v.end(), [&](const MeshDeclaration& d) { return d.id == id; }), v.end());
  if (v.empty()) declared_.erase(it);
}
void MeshStore::install(const std::shared_ptr<const MeshRecord>& rec) {
  std::shared_ptr<const MeshRecord> old;  // freed outside the lock
  std::lock_guard<std::mutex> lock(mutex_);
  old = std::move(active_[rec->key]);
  active_[rec->key] = rec;
  auto& v = declared_[rec->key];  // older declarations are resolved
  v.erase(std::remove_if(v.begin(), v.end(), [&](const MeshDeclaration& d) { return d.id < rec->topo->id; }), v.end());
}
bool MeshStore::install_positions(ar_key key, uint64_t base_topology, std::vector<float> positions,
                                  std::vector<float> corner_normals) {
  std::shared_ptr<const MeshRecord> old;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = active_.find(key);
  if (it == active_.end() || it->second->topo->id != base_topology) return false;
  // check_position_counts ran on the worker against the same topology; sizes are final here.
  old = it->second;
  it->second = make_record(key, old->topo, std::move(positions), std::move(corner_normals), ledger_);
  return true;
}
void MeshStore::remove(bool reset, const std::set<std::pair<uint32_t, uint64_t>>& removed, const std::set<ar_key>& keep) {
  std::vector<std::shared_ptr<const MeshRecord>> gone;  // freed outside the lock
  std::lock_guard<std::mutex> lock(mutex_);
  auto drop = [&](ar_key key) {
    auto it = active_.find(key);
    if (it == active_.end()) return;
    uint64_t id = it->second->topo->id;
    gone.push_back(std::move(it->second));
    active_.erase(it);
    auto& v = declared_[key];  // only declarations of later generations survive
    v.erase(std::remove_if(v.begin(), v.end(), [&](const MeshDeclaration& d) { return d.id <= id; }), v.end());
    if (v.empty()) declared_.erase(key);
  };
  if (reset) {
    std::vector<ar_key> all;
    for (const auto& kv : active_) all.push_back(kv.first);
    for (ar_key k : all)
      if (!keep.count(k)) drop(k);
  }
  for (const auto& r : removed)
    if (r.first == AR_KIND_GEOMETRY && !keep.count(r.second)) drop(r.second);
}

ar_status mesh_begin(Scene& scene, Edit& edit, const ar_mesh_desc* desc, ar_mesh_buffers* out) {
  if (!scene.meshes) throw std::logic_error("mesh_begin: the scene has no mesh store");
  if (!desc || !out) throw std::invalid_argument("ar_edit_mesh_begin: descriptor or buffers is null");
  if (desc->attribute_count != 0 && !desc->attributes) throw std::invalid_argument(label_of(*desc) + ": attributes is null with a nonzero count");
  ar_mesh_desc d = *desc;
  std::vector<ar_attribute_desc> attrs(d.attributes, d.attributes + d.attribute_count);
  d.attributes = nullptr;
  validate_desc(d, attrs);
  const Counts n = count_arrays(d, attrs);
  std::shared_ptr<MeshStore> store = scene.meshes;

  auto item = std::make_shared<EditItem>();
  item->key = d.key;
  item->kind = AR_KIND_GEOMETRY;
  item->needs_device = false;  // host record only; upload is 037
  auto blocks = std::make_shared<Blocks>();
  auto staged = std::make_shared<Staged>();
  ar_mesh_buffers b{};
  try {
    b.positions = stage_array<float>(scene, *item, *blocks, n.positions, &staged->positions);
    b.corner_vert = stage_array<int32_t>(scene, *item, *blocks, n.corner_vert, &staged->corner_vert);
    b.face_start = stage_array<int32_t>(scene, *item, *blocks, n.face_start, &staged->face_start);
    b.corner_normals = stage_array<float>(scene, *item, *blocks, n.normals, &staged->normals);
    b.uv = stage_array<float>(scene, *item, *blocks, n.uv, &staged->uv);
    b.material_index = stage_array<int32_t>(scene, *item, *blocks, n.material_index, &staged->material_index);
    b.attributes = stage_array<float>(scene, *item, *blocks, n.attributes, &staged->attributes);
  } catch (...) {
    for (const Arena::Block& blk : blocks->list) scene.arena().release(blk);
    throw;
  }
  b.positions_count = n.positions, b.corner_vert_count = n.corner_vert, b.face_start_count = n.face_start;
  b.corner_normals_count = n.normals, b.uv_count = n.uv, b.material_index_count = n.material_index;
  b.attributes_count = n.attributes;

  const uint64_t id = store->next_id();
  auto gate = std::make_shared<TopoGate>();
  MeshDeclaration decl;
  decl.id = id, decl.vertex_count = d.vertex_count, decl.corner_count = d.corner_count, decl.motion_steps = d.motion_steps;
  decl.gate = gate;
  auto result = std::make_shared<std::shared_ptr<const MeshRecord>>();
  auto ledger = store->shared_ledger();
  ContextState* ctx = &scene.ctx();
  const ar_key key = d.key;
  item->job = [d, attrs, n, staged, id, ledger, ctx, gate, result] {
    std::shared_ptr<MeshRecord> rec;
    try {
      rec = build_record(d, attrs, n, *staged, id, ledger, *ctx);
    } catch (...) {
      gate->resolve(nullptr);
      throw;
    }
    gate->resolve(rec->topo);
    *result = rec;
  };
  item->on_active = [store, result] { store->install(*result); };
  item->on_dropped = [store, key, id, gate, result] {
    gate->resolve(nullptr);  // an item that never ran must not strand a waiting positions job
    store->undeclare(key, id);
    result->reset();
  };
  ar_status st = stage_item(scene, edit, item, blocks);
  if (st == AR_OK) {
    store->declare(key, decl);
    *out = b;
  }
  return st;
}

ar_status positions_begin(Scene& scene, Edit& edit, ar_key key, ar_positions_buffers* out) {
  if (!scene.meshes) throw std::logic_error("positions_begin: the scene has no mesh store");
  if (!out) throw std::invalid_argument("ar_edit_positions_begin: buffers is null");
  std::shared_ptr<MeshStore> store = scene.meshes;
  std::optional<MeshDeclaration> base = store->latest(key);
  if (!base || !scene.known(AR_KIND_GEOMETRY, key))
    throw std::invalid_argument("ar_edit_positions_begin: no committed mesh with key " + std::to_string(key));

  auto item = std::make_shared<EditItem>();
  item->key = key;
  item->kind = AR_KIND_GEOMETRY;
  item->needs_device = false;
  item->replaces_subject = false;  // the mesh's other scene entries stay valid
  auto blocks = std::make_shared<Blocks>();
  auto staged = std::make_shared<Staged>();
  const uint64_t np = uint64_t(base->motion_steps) * base->vertex_count * 3, nn = uint64_t(base->corner_count) * 3;
  ar_positions_buffers b{};
  try {
    b.positions = stage_array<float>(scene, *item, *blocks, np, &staged->positions);
    b.corner_normals = stage_array<float>(scene, *item, *blocks, nn, &staged->normals);
  } catch (...) {
    for (const Arena::Block& blk : blocks->list) scene.arena().release(blk);
    throw;
  }
  b.positions_count = np, b.corner_normals_count = nn;

  struct Update {
    std::vector<float> positions, normals;
  };
  auto update = std::make_shared<Update>();
  Scene* sc = &scene;
  const uint64_t base_id = base->id;
  auto gate = base->gate;
  // Ref: 0007 d22. The job needs the base topology only to check counts; the record is
  // linked to the active topology at activation, never copied here.
  // The job is scheduled when the base ingest resolves (never a worker waiting): a base that
  // failed or was dropped resolves to null, the job still runs and its generation resolves.
  item->schedule = [gate](std::function<void()> queue) { gate->when_done(std::move(queue)); };
  item->job = [gate, np, nn, staged, update, key] {
    std::shared_ptr<const MeshTopology> t = gate->topology();
    if (t) check_position_counts(*t, np, nn);
    const std::string who = (t ? t->name : key_label(key)) + ": ";
    const uint64_t V3 = t ? uint64_t(t->vertex_count) * 3 : 0;
    check_finite(staged->positions, np, "positions", V3 ? V3 : np, who);
    check_finite(staged->normals, nn, "corner_normals", 0, who);
    update->positions.assign(staged->positions, staged->positions + np);
    update->normals.assign(staged->normals, staged->normals + nn);
  };
  item->on_active = [store, sc, update, key, base_id] {
    if (store->install_positions(key, base_id, std::move(update->positions), std::move(update->normals))) return;
    const std::string why = key_label(key) + ": positions-only update skipped, the topology it was based on is not the active one "
                                             "(its generation was rejected, the mesh was removed or its topology changed)";
    sc->ctx().log.line("WARNING", why);
    sc->report().raise(AR_REPORT_INPUT_IGNORED, key, why, 1, nullptr, 0);
  };
  item->on_dropped = [update] { *update = Update{}; };
  ar_status st = stage_item(scene, edit, item, blocks);
  if (st == AR_OK) *out = b;
  return st;
}

}  // namespace astroray::host

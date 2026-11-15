// SPDX-License-Identifier: Apache-2.0
#include "host/scene/mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "host/geometry/triangulate.h"

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

void compute_bounds(MeshRecord& r) {
  if (r.positions.empty()) return;
  for (int a = 0; a < 3; ++a) r.bounds_min[a] = r.bounds_max[a] = r.positions[a];
  for (size_t i = 0; i < r.positions.size(); i += 3)
    for (int a = 0; a < 3; ++a) {
      r.bounds_min[a] = std::min(r.bounds_min[a], r.positions[i + a]);
      r.bounds_max[a] = std::max(r.bounds_max[a], r.positions[i + a]);
    }
}

void account(MeshRecord& r, const std::shared_ptr<Ledger>& ledger) {
  auto bytes = [](const auto& v) { return uint64_t(v.size() * sizeof(v[0])); };
  r.host_bytes = bytes(r.positions) + bytes(r.corner_vert) + bytes(r.face_start) + bytes(r.corner_normals) + bytes(r.uv) +
                 bytes(r.attributes) + bytes(r.tri_corners) + bytes(r.tri_face) + bytes(r.tri_material);
  r.ledger = ledger;
  r.ledger_id = ledger->add(Owner{}, LedgerCategory::GEOMETRY, r.host_bytes);
}

// Worker job: O(n) checks, triangulation, the record (task 035 Do 2). Throws with the text
// of the first failed check.
std::shared_ptr<MeshRecord> build_record(const ar_mesh_desc& d, const std::vector<ar_attribute_desc>& attrs, const Counts& n,
                                         const Staged& s, const std::shared_ptr<Ledger>& ledger) {
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

  auto r = std::make_shared<MeshRecord>();
  r->key = d.key;
  r->name = label_of(d);
  r->vertex_count = V, r->corner_count = C, r->face_count = F;
  r->uv_layer_count = d.uv_layer_count, r->motion_steps = d.motion_steps;
  r->positions.assign(s.positions, s.positions + n.positions);
  r->corner_vert.assign(s.corner_vert, s.corner_vert + C);
  r->face_start.assign(s.face_start, s.face_start + F);
  r->face_start.push_back(C);
  r->corner_normals.assign(s.normals, s.normals + n.normals);
  r->uv.assign(s.uv, s.uv + n.uv);
  r->attributes.assign(s.attributes, s.attributes + n.attributes);
  r->attribute_descs = attrs;
  compute_bounds(*r);
  if (F) r->max_material_index = (d.flags & AR_MESH_HAS_MATERIAL_INDEX) ? uint32_t(max_mat) : 0u;

  const float* centre = r->positions.data() + size_t(d.motion_steps / 2) * V * 3;
  try {
    r->tri_corners = triangulate(centre, r->corner_vert.data(), r->face_start.data(), F);
  } catch (const std::exception& e) {
    throw std::runtime_error(who + e.what());
  }
  // Every face of n corners yields n - 2 triangles, in face order.
  r->tri_face.reserve(r->tri_corners.size() / 3);
  for (uint32_t f = 0; f < F; ++f) {
    uint32_t tris = r->face_start[f + 1] - r->face_start[f] - 2;
    for (uint32_t t = 0; t < tris; ++t) {
      r->tri_face.push_back(f);
      r->tri_material.push_back((d.flags & AR_MESH_HAS_MATERIAL_INDEX) ? s.material_index[f] : 0);
    }
  }
  if (r->tri_face.size() * 3 != r->tri_corners.size())
    throw std::runtime_error(who + "triangulation produced an unexpected triangle count");
  account(*r, ledger);
  return r;
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

MeshRecord::~MeshRecord() {
  if (ledger && ledger_id) {
    try {
      ledger->remove(ledger_id);
    } catch (...) {
    }
  }
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
void MeshStore::install(const std::shared_ptr<const MeshRecord>& rec) {
  std::shared_ptr<const MeshRecord> old;  // freed outside the lock
  std::lock_guard<std::mutex> lock(mutex_);
  old = std::move(active_[rec->key]);
  active_[rec->key] = rec;
}

ar_status mesh_begin(Scene& scene, Edit& edit, MeshStore& store, const ar_mesh_desc* desc, ar_mesh_buffers* out) {
  if (!desc || !out) throw std::invalid_argument("ar_edit_mesh_begin: descriptor or buffers is null");
  if (desc->attribute_count != 0 && !desc->attributes) throw std::invalid_argument(label_of(*desc) + ": attributes is null with a nonzero count");
  ar_mesh_desc d = *desc;
  std::vector<ar_attribute_desc> attrs(d.attributes, d.attributes + d.attribute_count);
  d.attributes = nullptr;
  validate_desc(d, attrs);
  const Counts n = count_arrays(d, attrs);

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

  auto result = std::make_shared<std::shared_ptr<const MeshRecord>>();
  auto ledger = store.shared_ledger();
  item->job = [d, attrs, n, staged, ledger, result] { *result = build_record(d, attrs, n, *staged, ledger); };
  item->on_active = [&store, result] { store.install(*result); };
  item->on_dropped = [result] { result->reset(); };
  ar_status st = stage_item(scene, edit, item, blocks);
  if (st == AR_OK) *out = b;
  return st;
}

ar_status positions_begin(Scene& scene, Edit& edit, MeshStore& store, ar_key key, ar_positions_buffers* out) {
  if (!out) throw std::invalid_argument("ar_edit_positions_begin: buffers is null");
  std::shared_ptr<const MeshRecord> base = store.find(key);
  if (!base) throw std::invalid_argument("ar_edit_positions_begin: no active mesh with key " + std::to_string(key));

  auto item = std::make_shared<EditItem>();
  item->key = key;
  item->kind = AR_KIND_GEOMETRY;
  item->needs_device = false;
  auto blocks = std::make_shared<Blocks>();
  auto staged = std::make_shared<Staged>();
  const uint64_t np = base->positions.size(), nn = base->corner_normals.size();
  ar_positions_buffers b{};
  try {
    b.positions = stage_array<float>(scene, *item, *blocks, np, &staged->positions);
    b.corner_normals = stage_array<float>(scene, *item, *blocks, nn, &staged->normals);
  } catch (...) {
    for (const Arena::Block& blk : blocks->list) scene.arena().release(blk);
    throw;
  }
  b.positions_count = np, b.corner_normals_count = nn;

  auto result = std::make_shared<std::shared_ptr<const MeshRecord>>();
  auto ledger = store.shared_ledger();
  // Ref: 0007 d22: topology, UVs, materials and triangles stay; positions and normals are
  // replaced and bounds recomputed.
  item->job = [base, np, nn, staged, ledger, result] {
    const std::string who = base->name + ": ";
    if (np != base->positions.size() || nn != base->corner_normals.size())
      throw std::runtime_error(who + "positions-only update has a different vertex or corner count");
    check_finite(staged->positions, np, "positions", uint64_t(base->vertex_count) * 3, who);
    check_finite(staged->normals, nn, "corner_normals", 0, who);
    auto r = std::make_shared<MeshRecord>();
    r->key = base->key, r->name = base->name;
    r->vertex_count = base->vertex_count, r->corner_count = base->corner_count, r->face_count = base->face_count;
    r->uv_layer_count = base->uv_layer_count, r->motion_steps = base->motion_steps;
    r->corner_vert = base->corner_vert, r->face_start = base->face_start, r->uv = base->uv;
    r->attributes = base->attributes, r->attribute_descs = base->attribute_descs;
    r->tri_corners = base->tri_corners, r->tri_face = base->tri_face, r->tri_material = base->tri_material;
    r->max_material_index = base->max_material_index;
    r->positions.assign(staged->positions, staged->positions + np);
    r->corner_normals.assign(staged->normals, staged->normals + nn);
    compute_bounds(*r);
    account(*r, ledger);
    *result = r;
  };
  item->on_active = [&store, result] { store.install(*result); };
  item->on_dropped = [result] { result->reset(); };
  ar_status st = stage_item(scene, edit, item, blocks);
  if (st == AR_OK) *out = b;
  return st;
}

}  // namespace astroray::host

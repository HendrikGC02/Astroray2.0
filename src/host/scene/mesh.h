// SPDX-License-Identifier: Apache-2.0
// Mesh ingestion: staged arrays, worker-side validation and triangulation, and the host
// mesh record (0007 decisions 9, 10, 12, 22; appendix `ar_mesh_desc`). Device upload and
// acceleration structures are later tasks and read the records kept here.
// Ref: 0007 d9 (O(n) validation on workers), d10 (geometry record, corner-indexed
// lookups), d22 (positions-only path), V15 (invalid indices, NaN positions).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../../../include/astroray/astroray.h"
#include "host/ledger.h"
#include "host/scene/edit.h"

namespace astroray::host {

// Everything a positions-only update keeps. Immutable once built; a positions-only record
// shares its topology with the record it replaces.
struct MeshTopology {
  uint64_t id = 0;  // unique per ingest; a positions-only update names the id it was based on
  ar_key key = 0;
  std::string name;
  uint32_t vertex_count = 0, corner_count = 0, face_count = 0, uv_layer_count = 0, motion_steps = 1;
  std::vector<uint32_t> corner_vert, face_start;  // face_start has F + 1 entries (last = C)
  std::vector<float> uv;                          // U x C x 2
  std::vector<float> attributes;                  // as staged
  std::vector<ar_attribute_desc> attribute_descs;
  // Triangles as corner triples: values index corner_vert and corner_normals, so a hit
  // reads normals and UVs by corner (0007 d10). tri_face and tri_material have one entry
  // per triangle.
  std::vector<uint32_t> tri_corners;
  std::vector<uint32_t> tri_face;
  std::vector<int32_t> tri_material;
  // Largest face material_index; none for an empty mesh. Without HAS_MATERIAL_INDEX every
  // face uses slot 0. A later check against an instance's material_counts reads it.
  std::optional<uint32_t> max_material_index;
  uint32_t skipped_zero_area = 0, fan_fallback = 0;  // triangulation counts (ruling 67)
  uint32_t triangle_count() const { return static_cast<uint32_t>(tri_corners.size() / 3); }

  ~MeshTopology();  // releases the ledger record
  std::shared_ptr<Ledger> ledger;
  uint64_t ledger_id = 0;
};

// One committed mesh state, immutable once active.
struct MeshRecord {
  ar_key key = 0;
  std::shared_ptr<const MeshTopology> topo;
  std::vector<float> positions;  // S x V x 3; only representative_step(S) is used until M8 (ruling 68)
  std::vector<float> corner_normals;  // C x 3
  float bounds_min[3] = {0, 0, 0}, bounds_max[3] = {0, 0, 0};  // over every motion step
  uint64_t host_bytes = 0;  // positions + normals; the topology counts its own

  ~MeshRecord();
  std::shared_ptr<Ledger> ledger;
  uint64_t ledger_id = 0;
};

// Resolution of one ingest: its topology, or null when it failed, was dropped or never ran.
// Dependent jobs are continuations, never waits: when_done runs the callback at once if resolved,
// otherwise it runs on whichever thread resolves the gate, and must not block.
class TopoGate {
 public:
  void resolve(std::shared_ptr<const MeshTopology> t);  // first call wins
  void when_done(std::function<void()> fn);
  std::shared_ptr<const MeshTopology> topology() const;  // valid once resolved

 private:
  mutable std::mutex mutex_;
  bool done_ = false;
  std::shared_ptr<const MeshTopology> topo_;
  std::vector<std::function<void()>> waiting_;
};

// A mesh ingest that has been staged and not yet resolved: what a later positions-only
// edit validates against, pending generations included.
struct MeshDeclaration {
  uint64_t id = 0;  // equals the topology id it will produce
  uint32_t vertex_count = 0, corner_count = 0, motion_steps = 1;
  std::shared_ptr<TopoGate> gate;
};

// Checks a positions-only update's staged counts against its base topology; throws
// std::runtime_error naming the mesh.
void check_position_counts(const MeshTopology& base, uint64_t positions, uint64_t corner_normals);

// The active mesh records of one scene, the pending declarations and the ledger that
// counts their host bytes. The scene's context has no ledger of its own, so this one
// is the scene's host geometry ledger.
class MeshStore {
 public:
  MeshStore() : ledger_(std::make_shared<Ledger>()) {}
  std::shared_ptr<const MeshRecord> find(ar_key key) const;
  size_t size() const;
  uint64_t geometry_bytes() const { return ledger_->category_total(LedgerCategory::GEOMETRY); }
  std::shared_ptr<Ledger> shared_ledger() const { return ledger_; }

  uint64_t next_id() { return next_id_.fetch_add(1); }
  // Latest declaration of `key`, pending ones included; nullopt if none.
  std::optional<MeshDeclaration> latest(ar_key key) const;
  void declare(ar_key key, const MeshDeclaration& d);
  void undeclare(ar_key key, uint64_t id);

  // Generation activation (called under the tracker lock; they never call back into it).
  void install(const std::shared_ptr<const MeshRecord>& rec);
  // Replaces K's positions when `base_topology` is still K's active topology; false (and
  // nothing changed) otherwise.
  bool install_positions(ar_key key, uint64_t base_topology, std::vector<float> positions,
                         std::vector<float> corner_normals);
  // Removals of a generation: `reset` clears every key not in `keep`.
  void remove(bool reset, const std::set<std::pair<uint32_t, uint64_t>>& removed, const std::set<ar_key>& keep);

 private:
  mutable std::mutex mutex_;
  std::map<ar_key, std::shared_ptr<const MeshRecord>> active_;
  std::map<ar_key, std::vector<MeshDeclaration>> declared_;
  std::atomic<uint64_t> next_id_{1};
  std::shared_ptr<Ledger> ledger_;
};

// ar_edit_mesh_begin: validates the descriptor, stages the arrays in the scene's arena and
// registers the item. Throws std::invalid_argument for a bad descriptor (the text is the
// reason); a duplicate key is returned as the edit's status with its last_error.
// The scene's `meshes` store must exist.
ar_status mesh_begin(Scene& scene, Edit& edit, const ar_mesh_desc* desc, ar_mesh_buffers* out);
// ar_edit_positions_begin: staged positions and normals for the latest committed mesh of
// `key` (pending generations included).
ar_status positions_begin(Scene& scene, Edit& edit, ar_key key, ar_positions_buffers* out);

}  // namespace astroray::host

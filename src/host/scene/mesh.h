// SPDX-License-Identifier: Apache-2.0
// Mesh ingestion: staged arrays, worker-side validation and triangulation, and the host
// mesh record (0007 decisions 9, 10, 12, 22; appendix `ar_mesh_desc`). Device upload and
// acceleration structures are later tasks and read the records kept here.
// Ref: 0007 d9 (O(n) validation on workers), d10 (geometry record, corner-indexed
// lookups), d22 (positions-only path), V15 (invalid indices, NaN positions).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../../../include/astroray/astroray.h"
#include "host/ledger.h"
#include "host/scene/edit.h"

namespace astroray::host {

// One committed mesh, immutable once active. A new generation replaces the whole record.
struct MeshRecord {
  ar_key key = 0;
  std::string name;
  uint32_t vertex_count = 0, corner_count = 0, face_count = 0, uv_layer_count = 0, motion_steps = 1;
  std::vector<float> positions;  // S x V x 3; only step S/2 (the shutter centre) is used until M8
  std::vector<uint32_t> corner_vert, face_start;  // face_start has F + 1 entries (last = C)
  std::vector<float> corner_normals;              // C x 3
  std::vector<float> uv;                          // U x C x 2
  std::vector<float> attributes;                  // as staged
  std::vector<ar_attribute_desc> attribute_descs;
  // Triangles as corner triples: values index corner_vert and corner_normals, so a hit
  // reads normals and UVs by corner (0007 d10). tri_face and tri_material have one entry
  // per triangle.
  std::vector<uint32_t> tri_corners;
  std::vector<uint32_t> tri_face;
  std::vector<int32_t> tri_material;
  float bounds_min[3] = {0, 0, 0}, bounds_max[3] = {0, 0, 0};  // over every motion step
  // Largest face material_index; none for an empty mesh. Without HAS_MATERIAL_INDEX every
  // face uses slot 0. A later check against an instance's material_counts reads it.
  std::optional<uint32_t> max_material_index;
  uint64_t host_bytes = 0;
  uint32_t triangle_count() const { return static_cast<uint32_t>(tri_corners.size() / 3); }

  ~MeshRecord();  // releases the ledger record
  std::shared_ptr<Ledger> ledger;
  uint64_t ledger_id = 0;
};

// The active mesh records of one scene and the ledger that counts their host bytes.
class MeshStore {
 public:
  MeshStore() : ledger_(std::make_shared<Ledger>()) {}
  std::shared_ptr<const MeshRecord> find(ar_key key) const;
  size_t size() const;
  uint64_t geometry_bytes() const { return ledger_->category_total(LedgerCategory::GEOMETRY); }
  Ledger& ledger() { return *ledger_; }
  std::shared_ptr<Ledger> shared_ledger() const { return ledger_; }
  void install(const std::shared_ptr<const MeshRecord>& rec);  // generation became active

 private:
  mutable std::mutex mutex_;
  std::map<ar_key, std::shared_ptr<const MeshRecord>> active_;
  std::shared_ptr<Ledger> ledger_;
};

// ar_edit_mesh_begin: validates the descriptor, stages the arrays in the scene's arena and
// registers the item. Throws std::invalid_argument for a bad descriptor (the text is the
// reason); a duplicate key is returned as the edit's status with its last_error.
ar_status mesh_begin(Scene& scene, Edit& edit, MeshStore& store, const ar_mesh_desc* desc,
                     ar_mesh_buffers* out);
// ar_edit_positions_begin: staged positions and normals for the active record of `key`.
ar_status positions_begin(Scene& scene, Edit& edit, MeshStore& store, ar_key key,
                          ar_positions_buffers* out);

}  // namespace astroray::host

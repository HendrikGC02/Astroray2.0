// SPDX-License-Identifier: Apache-2.0
// Scene tables on the CUDA device (task 102): kg_checksum.cu reads the count, first and last
// element of every table through the device KernelGlobals the builder made, and the host checks
// them against its own copy of each table (and the store arrays against the stores); a material
// edit re-uploads only the material tables; the ledger holds exactly the tables' bytes and is
// empty once the set is dropped; lamp instances upload without a mesh.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D13; 0007 d9, d14; RULINGS 70.
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "device/cuda/scene_upload.h"
#include "host/data_resource.h"
#include "host/device.h"
#include "host/image/image_store.h"
#include "host/light/lamps.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kElemMax = 432;  // kg_checksum.cu kKgElemMax
constexpr uint32_t kRow = 8 + 2 * kElemMax;
constexpr uint32_t kRows = static_cast<uint32_t>(TableId::E_DIEL) + 1u;

struct Graph {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  int add(ar::Op op, std::vector<float> fp) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size()), n.f32_count = uint32_t(fp.size());
    f.insert(f.end(), fp.begin(), fp.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  // A constant Emission of colour (r, 0.5, 0.25), or with `positional` the shading position as
  // colour, which compiles to a program.
  Graph(float r, float strength, bool positional = false) {
    const int c = positional ? add(ar::Op::LOAD_CTX_P, {}) : add(ar::Op::CONST_F32X3, {r, 0.5f, 0.25f});
    const int s = add(ar::Op::CONST_F32, {strength});
    const int e = add(ar::Op::EMIT_LE, {});
    links.push_back({uint32_t(c), 0u, uint32_t(e), 0u});
    links.push_back({uint32_t(s), 0u, uint32_t(e), 1u});
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size()), d.f32_count = uint32_t(f.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data();
    return d;
  }
};

// The scene's device, created on the init thread as ar_scene_create does, on a ledger of its own
// so the test sees only device records.
struct Held {
  std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
  std::unique_ptr<ModuleDevice> device;
  std::unique_ptr<AccelBuilder> accel;
};

struct Rig {
  ContextState ctx;
  std::shared_ptr<Held> held = std::make_shared<Held>();
  std::unique_ptr<Scene> scene;
  Rig() {
    SceneConfig cfg;
    cfg.workers = 2;
    auto h = held;
    cfg.init = [h]() -> std::shared_ptr<void> {
      h->device = make_cuda_device(*h->ledger, 0);
      h->accel = make_optix_accel_builder(*h->device, 0, std::string());  // no OptiX disk cache
      return h;
    };
    cfg.device_of = [](void* p) -> Device* { return static_cast<Held*>(p)->device.get(); };
    cfg.accel_of = [](void* p) -> AccelBuilder* { return static_cast<Held*>(p)->accel.get(); };
    scene = std::make_unique<Scene>(ctx, std::move(cfg));
    scene->meshes = std::make_shared<MeshStore>();
    scene->materials = std::make_shared<MaterialStore>(scene->meshes->shared_ledger());
  }
  ModuleDevice& dev() {
    scene->wait_init();
    return *held->device;
  }
  template <class F>
  void commit(F&& body) {
    auto e = scene->begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    scene->materials->end_edit();
    e.reset();
    scene->generations().wait_idle();
    REQUIRE(scene->generations().failed() == 0);
  }
  // Material 43 is referenced by no instance, so editing it moves no light.
  void set_material_43(float strength) {
    const Graph g(1.0f, strength, true);
    commit([&](Edit& e) {
      const ar_graph_desc d = g.desc();
      REQUIRE(scene->materials->set_graph(e, 44, &d) == AR_OK);
      ar_material_desc m = {};
      m.surface_graph = 44;
      REQUIRE(scene->materials->set_material(e, 43, &m) == AR_OK);
    });
  }
  // One object of each kind: lit world, emissive material on an instanced quad, an image (the
  // magenta stand-in of a missing file), a point lamp, and material 43.
  void build() {
    const Graph gw(1.0f, 0.5f), ge(1.0f, 2.0f);
    commit([&](Edit& e) {
      const ar_graph_desc dw = gw.desc(), de = ge.desc();
      ar_world_desc w = {};
      w.surface_graph = 40;
      REQUIRE(scene->materials->set_graph(e, 40, &dw) == AR_OK);
      REQUIRE(scene->materials->set_world(e, &w) == AR_OK);
      REQUIRE(scene->materials->set_graph(e, 41, &de) == AR_OK);
      ar_material_desc m = {};
      m.surface_graph = 41;
      m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
      REQUIRE(scene->materials->set_material(e, 42, &m) == AR_OK);
      ar_image_desc im = {};
      im.key = 7, im.source = AR_IMAGE_SOURCE_FILE, im.color_interop = AR_COLOR_INTEROP_LIN_REC709;
      im.path = "no_such_image_102.png";
      REQUIRE(scene->materials->set_image(e, &im) == AR_OK);
      ar_light_desc l = {};
      l.type = AR_LIGHT_POINT;
      l.color.kind = AR_SPECTRUM_RGB;
      l.color.rgb[0] = l.color.rgb[1] = l.color.rgb[2] = 1.0f;
      l.xf.m[0] = l.xf.m[5] = l.xf.m[10] = 1.0f, l.xf.m[3] = 2.0f;
      l.visibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE;
      l.power = 10.0f, l.normalize = 1, l.cast_shadow = 1;
      REQUIRE(set_light(e, 200, &l, scene->materials->table()) == AR_OK);
    });
    set_material_43(0.3f);
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    commit([&](Edit& e) {
      ar_mesh_desc d = {};
      d.key = 100, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(*scene, e, &d, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(100) == AR_OK);
    });
    std::vector<ar_key> geom{100}, object{1}, pool{42};
    std::vector<ar_xform> xf(1);
    xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
    std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA), moff{0}, mcount{1};
    std::vector<ar_rgba> colors(1, ar_rgba{{1, 1, 1, 1}});
    std::vector<uint8_t> lg(1, 0);
    ar_instances_desc d = {};
    d.instance_count = 1, d.motion_steps = 1, d.transform_count = 1, d.material_key_count = 1;
    d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
    d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
    d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
    d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = pool.data();
    commit([&](Edit& e) { REQUIRE(set_instances(e, &d) == AR_OK); });
  }
};

uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// What a table should hold, from the host sources: its count and its first and last element.
struct HostRow {
  uint32_t count = 0;
  std::vector<uint8_t> first, last;
};

template <class T>
HostRow row_of(const T* data, size_t count) {
  HostRow r;
  r.count = uint32_t(count);
  if (count == 0) return r;
  const uint8_t* a = reinterpret_cast<const uint8_t*>(data);
  r.first.assign(a, a + sizeof(T));
  r.last.assign(a + (count - 1) * sizeof(T), a + count * sizeof(T));
  return r;
}

uint64_t addr(const std::shared_ptr<const DeviceTable>& t) { return t ? uint64_t(reinterpret_cast<uintptr_t>(t->ptr())) : 0; }

// Every table's expected row, built from the scene's own arrays (store arrays as they are, derived
// records rebuilt here); only device addresses inside records come from the set's copies.
std::vector<HostRow> host_rows(Scene& scene, const SceneTableSet& set) {
  std::vector<HostRow> rows(kRows);
  const auto at = [&](TableId id) -> HostRow& { return rows[static_cast<uint32_t>(id)]; };
  const auto base = scene.instances();
  const auto lights = scene.lights();
  const auto mats = scene.materials->arrays();
  const auto imgs = scene.materials->images().arrays();
  // Instances: the scene's, then one per lamp; mesh index = rank of the key among referenced keys.
  const auto table = append_lamp_instances(base.get(), lights->lamp_instances);
  std::map<ar_key, uint32_t> rank;
  for (const Instance& in : base->instances) rank[in.geom_key] = 0;
  uint32_t k = 0;
  for (auto& kv : rank) kv.second = k++;
  std::vector<ar::DeviceInstanceRecord> inst;
  for (uint32_t i = 0; i < table->instances.size(); ++i)
    inst.push_back(instance_record(*table, i, i < base->instances.size() ? rank.at(table->instances[i].geom_key) : kNoMeshIndex));
  at(TableId::INSTANCES) = row_of(inst.data(), inst.size());
  std::vector<ar::DeviceMeshView> views;
  for (auto& kv : rank) {
    const auto rec = scene.meshes->find(kv.first);
    const MeshCopy& c = *set.meshes.at(kv.first);
    ar::DeviceMeshView v{};
    v.positions = addr(c.positions), v.tri_vertex = addr(c.tri_vertex), v.tri_corners = addr(c.tri_corners);
    v.corner_normals = addr(c.corner_normals), v.uv = addr(c.uv), v.tri_material = addr(c.tri_material);
    v.vertex_count = rec->topo->vertex_count, v.triangle_count = rec->topo->triangle_count();
    v.corner_count = rec->topo->corner_count, v.uv_layer_count = rec->topo->uv_layer_count;
    views.push_back(v);
  }
  at(TableId::MESHES) = row_of(views.data(), views.size());
  std::vector<uint32_t> slots;
  for (ar_key key : base->material_pool)
    slots.push_back(uint32_t(std::lower_bound(mats->keys.begin(), mats->keys.end(), key) - mats->keys.begin()));
  at(TableId::MATERIAL_SLOTS) = row_of(slots.data(), slots.size());
  at(TableId::MATERIAL_HEADERS) = row_of(mats->headers.data(), mats->headers.size());
  at(TableId::MATERIAL_PROGRAMS) = row_of(mats->programs.data(), mats->programs.size());
  at(TableId::MATERIAL_CONSTANTS) = row_of(mats->constants.data(), mats->constants.size());
  at(TableId::MATERIAL_PASS_INDEX) = row_of(mats->pass_index.data(), mats->pass_index.size());
  std::vector<ar::DeviceImageView> iv = imgs->views;
  for (size_t i = 0; i < iv.size(); ++i) iv[i].texels = addr(set.texels[i]);
  at(TableId::IMAGES) = row_of(iv.data(), iv.size());
  at(TableId::LIGHTS) = row_of(lights->lights.data(), lights->lights.size());
  at(TableId::LIGHT_BINS) = row_of(lights->bins.data(), lights->bins.size());
  at(TableId::LIGHT_BIN_LIGHT) = row_of(lights->bin_light.data(), lights->bin_light.size());
  at(TableId::LIGHT_PMF) = row_of(lights->pmf.data(), lights->pmf.size());
  at(TableId::LIGHT_INDEX) = row_of(lights->light_index.data(), lights->light_index.size());
  const ar::Rgb2SpecTable& jh = scene.materials->table();
  const size_t res = size_t(jh.res);
  at(TableId::RGB2SPEC_NODES) = row_of(jh.z_nodes, res);
  at(TableId::RGB2SPEC_COEFFS) = row_of(jh.coeffs, 9 * res * res * res);
  static ObserverTables obs;
  DataBlob blob;
  REQUIRE(find_data("spectral/observer.bin", &blob));
  REQUIRE(parse_observer(blob.data, blob.size, &obs).empty());
  at(TableId::OBSERVER_CMF) = row_of(&obs.cmf[0][0], 3 * size_t(kObserverSamples));
  at(TableId::OBSERVER_ILLUM) = row_of(obs.d65, size_t(kObserverSamples));
  static float srgb[ar::kSrgbTableSize];
  ar::srgb_decode_table(srgb);
  at(TableId::SRGB_DECODE) = row_of(srgb, ar::kSrgbTableSize);
  REQUIRE(find_data("tables/e_diel_refl.bin", &blob));
  ar::EDielTable e;
  REQUIRE(ar::e_diel_view(blob.data, blob.size, &e) == nullptr);
  at(TableId::E_DIEL) = row_of(e.e, size_t(ar::kEDielN) * ar::kEDielN * ar::kEDielN);
  return rows;
}

// Every unique copy the set holds, with its bytes.
uint64_t set_bytes(const SceneTableSet& s) {
  std::set<const DeviceTable*> seen;
  uint64_t total = 0;
  const auto add = [&](const std::shared_ptr<const DeviceTable>& t) {
    if (t && seen.insert(t.get()).second) total += t->buffer().bytes;
  };
  for (const auto& t : s.tables) add(t);
  for (const auto& t : s.texels) add(t);
  for (const auto& kv : s.meshes)
    for (const auto* t : {&kv.second->tri_vertex, &kv.second->tri_corners, &kv.second->uv, &kv.second->tri_material,
                          &kv.second->positions, &kv.second->corner_normals})
      add(*t);
  return total;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("scene_upload/checksums match host") {
  Rig r;
  r.build();
  ModuleDevice& dev = r.dev();
  auto set = acquire_scene_tables(*r.scene);
  KgSession in;  // the session part is not read by kg_checksum
  const ar::KernelGlobals kg = build_kernel_globals(set.get(), in);
  DeviceBuffer kgb = dev.allocate(LedgerCategory::TABLES, sizeof kg, Owner{});
  DeviceBuffer out = dev.allocate(LedgerCategory::TABLES, uint64_t(kRows) * kRow, Owner{});
  dev.upload(kgb, 0, &kg, sizeof kg);
  dev.memset(out, 0, 0, out.bytes);
  const uint32_t module = dev.load_module_file(std::string(AR_TEST_KERNEL_DIR) + "/kg_checksum.cubin");
  void* kg_p = kgb.ptr;
  void* out_p = out.ptr;
  void* args[2] = {&kg_p, &out_p};
  dev.launch_module_kernel(module, "ar_kg_checksum", 1, 1, args);
  dev.synchronize();
  std::vector<uint8_t> rows(out.bytes);
  dev.download(rows.data(), out, 0, out.bytes);

  const std::vector<HostRow> want = host_rows(*r.scene, *set);
  for (uint32_t t = 0; t < kRows; ++t) {
    const TableId id = TableId(t);
    const uint8_t* row = rows.data() + uint64_t(t) * kRow;
    uint32_t count = 0, elem = 0;
    std::memcpy(&count, row, 4), std::memcpy(&elem, row + 4, 4);
    const HostRow& h = want[t];
    REQUIRE(elem <= kElemMax);
    uint64_t gpu = fnv(14695981039346656037ull, row, 4), host = fnv(14695981039346656037ull,
                                                                   reinterpret_cast<const uint8_t*>(&h.count), 4);
    if (count != 0) gpu = fnv(fnv(gpu, row + 8, elem), row + 8 + kElemMax, elem);
    if (h.count != 0) host = fnv(fnv(host, h.first.data(), h.first.size()), h.last.data(), h.last.size());
    const uint64_t bytes = set->tables[t]->bytes();
    std::printf("scene_upload table %-22s count %8u bytes %10llu checksum gpu %016llx host %016llx\n", table_name(id),
                count, (unsigned long long)bytes, (unsigned long long)gpu, (unsigned long long)host);
    CHECK_MESSAGE(count == h.count, table_name(id));
    CHECK_MESSAGE(gpu == host, table_name(id));
    CHECK_MESSAGE(bytes == uint64_t(h.count) * elem, table_name(id));
  }
  CHECK(want[static_cast<uint32_t>(TableId::INSTANCES)].count == 2u);
  CHECK(want[static_cast<uint32_t>(TableId::MATERIAL_PROGRAMS)].count > 0u);  // material 43 runs a program
  // The emissive quad's index: N = 1, its slice offset, C = 2, then lights 0 and 1.
  CHECK(want[static_cast<uint32_t>(TableId::LIGHT_INDEX)].count == 5u);
  dev.free(kgb);
  dev.free(out);
}

TEST_CASE("scene_upload/material edit uploads only material tables") {
  Rig r;
  r.build();
  ModuleDevice& dev = r.dev();
  auto lights = r.scene->lights();
  auto first = acquire_scene_tables(*r.scene);
  std::printf("scene_upload first upload: %llu bytes\n", (unsigned long long)first->bytes_moved);
  // The ledger holds exactly the set's copies.
  CHECK(r.held->ledger->total() == set_bytes(*first) + first->accel->bytes);  // the traversal structures are ledgered too

  r.set_material_43(0.9f);
  lights = r.scene->lights();
  REQUIRE((lights == first->lights));  // the edit moved no light
  auto second = acquire_scene_tables(*r.scene);
  REQUIRE((second != first));
  std::printf("scene_upload material edit: %llu bytes re-uploaded\n", (unsigned long long)second->bytes_moved);
  uint64_t material_bytes = 0;
  for (uint32_t t = 0; t < kTableCount; ++t) {
    const TableId id = TableId(t);
    const bool material = id == TableId::MATERIAL_SLOTS || id == TableId::MATERIAL_HEADERS ||
                          id == TableId::MATERIAL_PROGRAMS || id == TableId::MATERIAL_CONSTANTS ||
                          id == TableId::MATERIAL_PASS_INDEX;
    // The header block holds the new tables' addresses, so it is always rewritten.
    CHECK_MESSAGE(second->uploaded[t] == (material || id == TableId::HEADER), table_name(id));
    if (second->uploaded[t]) material_bytes += second->tables[t]->bytes();
    if (!material && id != TableId::HEADER) CHECK_MESSAGE((second->tables[t] == first->tables[t]), table_name(id));
  }
  CHECK(second->bytes_moved == material_bytes);
  CHECK((second->meshes == first->meshes));
  CHECK((second->texels == first->texels));

  // Retirement: the old copies go with their last holder, the cache now holds only the second set.
  first.reset();
  CHECK(r.held->ledger->total() == set_bytes(*second) + second->accel->bytes);
  second.reset();
  r.scene->table_cache().current.reset();
  CHECK(r.held->ledger->total() == 0);
  (void)dev;
}

TEST_CASE("scene_upload/lamp instances upload without a mesh") {
  Ledger ledger;
  std::unique_ptr<ModuleDevice> dev = make_cuda_device(ledger, 0);
  {
    SceneUpload up(*dev, Owner{1, 0});
    LampInstanceIn lamp;
    lamp.key = 9, lamp.visibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_SHADOW, lamp.light_index = 5;
    lamp.xf.m[0] = lamp.xf.m[5] = lamp.xf.m[10] = 1.0f;
    const auto table = append_lamp_instances(nullptr, {lamp});
    up.upload_instances(*table);
    REQUIRE(up.instance_count() == 1u);
    DeviceInstanceRecord rec{};
    dev->download(&rec, up.instance_records(), 0, sizeof rec);
    CHECK(rec.mesh == kNoMeshIndex);
    CHECK(rec.light_index == 5u);
    CHECK(rec.mask == uint32_t(visibility_mask(AR_VISIBILITY_CAMERA)));  // never the shadow bit (0004 D8)
  }
  CHECK(ledger.total() == 0);
}

}  // TEST_SUITE

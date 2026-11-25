// SPDX-License-Identifier: Apache-2.0
// Scene table sets: see scene_tables.h for ownership and lifetime.
// Ref: docs/decisions/0002-kernel-architecture.md D13; 0007 d9, d10, d14; RULINGS 70.
#include "host/scene/scene_tables.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "host/data_resource.h"
#include "host/log.h"
#include "host/image/image_store.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/instances.h"
#include "host/scene/media.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/world.h"
#include "host/session/test_hooks.h"
#include "kernel/material/lobes/tables.h"
#include "kernel/vm/context.h"

namespace astroray::host {
namespace {

constexpr uint64_t align16(uint64_t v) { return (v + 15) / 16 * 16; }

template <class T>
const T* as(const std::shared_ptr<const DeviceTable>& t) {
  return t ? static_cast<const T*>(t->ptr()) : nullptr;
}

uint64_t addr(const std::shared_ptr<const DeviceTable>& t) {
  return t ? uint64_t(reinterpret_cast<uintptr_t>(t->ptr())) : 0;
}

// Embedded data lives as long as the module: a pin that owns nothing.
std::shared_ptr<const void> static_pin(const void* p) { return std::shared_ptr<const void>(p, [](const void*) {}); }

}  // namespace

const char* table_name(TableId id) {
  static const char* const names[kTableCount] = {
      "instances",          "mesh table",      "material slots", "material headers", "material programs",
      "material constants", "material pass indices", "image views",     "light records",  "light alias bins", "light bin lights",
      "light pmf",          "light index",     "rgb2spec nodes",  "rgb2spec coefficients", "observer cmf", "observer illuminant",
      "srgb decode",        "e_diel table",    "material spectra", "e_spec table", "media table", "world program",
      "world constants", "environment table", "table header"};
  const uint32_t i = static_cast<uint32_t>(id);
  if (i >= kTableCount) throw std::invalid_argument("table_name: no table " + std::to_string(i));
  return names[i];
}

std::shared_ptr<const DeviceTable> DeviceTable::put(const UploadTarget& t, LedgerCategory category, const void* data,
                                                    uint64_t bytes, std::shared_ptr<const void> pin) {
  auto d = std::make_shared<DeviceTable>();
  d->bytes_ = bytes;
  if (bytes == 0) return d;
  if (t.device) {
    d->buffer_ = t.device->allocate(category, bytes, t.owner);
    d->device_ = t.device;  // from here the destructor frees the buffer, also if the upload throws
    t.device->upload(d->buffer_, 0, data, bytes);
    d->ptr_ = d->buffer_.ptr;
    d->moved_ = bytes;
  } else if (pin) {
    d->pin_ = std::move(pin);
    d->ptr_ = data;
  } else {
    d->host_.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
    if (t.host_ledger) d->ledger_id_ = t.host_ledger->add(t.owner, category, bytes), d->ledger_ = t.host_ledger;
    d->ptr_ = d->host_.data();
    d->moved_ = bytes;
  }
  return d;
}

DeviceTable::~DeviceTable() {
  // Teardown cannot throw; a failure is printed, never dropped (as CudaDevice teardown does). The
  // scene's device outlives every set: sessions drop theirs before CLOSED.
  try {
    if (device_) device_->free(buffer_);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "astroray: freeing a scene table failed: %s\n", e.what());
  }
  try {
    if (ledger_ && ledger_id_) ledger_->remove(ledger_id_);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "astroray: releasing a scene table's ledger record failed: %s\n", e.what());
  }
}

std::shared_ptr<const MeshCopy> upload_mesh(const UploadTarget& t, const std::shared_ptr<const MeshRecord>& rec,
                                            const MeshCopy* prev) {
  const MeshTopology& topo = *rec->topo;
  const uint32_t V = topo.vertex_count;
  // Ruling 68: the representative motion step.
  const size_t step = size_t(representative_step(topo.motion_steps));
  auto m = std::make_shared<MeshCopy>();
  m->topology_id = topo.id;
  if (prev && prev->topology_id == topo.id) {
    m->tri_vertex = prev->tri_vertex, m->tri_corners = prev->tri_corners;
    m->uv = prev->uv, m->tri_material = prev->tri_material;
  } else {
    std::vector<uint32_t> tri_vertex(topo.tri_corners.size());
    for (size_t i = 0; i < tri_vertex.size(); ++i) tri_vertex[i] = topo.corner_vert[topo.tri_corners[i]];
    const std::shared_ptr<const void> tp = rec->topo;
    m->tri_vertex = DeviceTable::put(t, LedgerCategory::GEOMETRY, tri_vertex.data(), tri_vertex.size() * 4);
    m->tri_corners = DeviceTable::put(t, LedgerCategory::GEOMETRY, topo.tri_corners.data(), topo.tri_corners.size() * 4, tp);
    m->uv = DeviceTable::put(t, LedgerCategory::GEOMETRY, topo.uv.data(), topo.uv.size() * 4, tp);
    m->tri_material = DeviceTable::put(t, LedgerCategory::GEOMETRY, topo.tri_material.data(), topo.tri_material.size() * 4, tp);
  }
  const std::shared_ptr<const void> rp = rec;
  m->positions = DeviceTable::put(t, LedgerCategory::GEOMETRY, rec->positions.data() + step * V * 3, uint64_t(V) * 12, rp);
  m->corner_normals =
      DeviceTable::put(t, LedgerCategory::GEOMETRY, rec->corner_normals.data(), rec->corner_normals.size() * 4, rp);
  ar::DeviceMeshView& v = m->view;
  v.positions = addr(m->positions), v.tri_vertex = addr(m->tri_vertex), v.tri_corners = addr(m->tri_corners);
  v.corner_normals = addr(m->corner_normals), v.uv = addr(m->uv), v.tri_material = addr(m->tri_material);
  v.vertex_count = V, v.triangle_count = topo.triangle_count(), v.corner_count = topo.corner_count;
  v.uv_layer_count = topo.uv_layer_count;
  return m;
}

MediumIndex medium_index(const std::vector<std::shared_ptr<const Medium>>& media) {
  MediumIndex out;
  for (uint32_t i = 0; i < media.size(); ++i) out[media[i]->key] = i;
  return out;
}

void check_medium_table(const ar::MediumDesc* descs, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t p = descs[i].program;
    if (p == Medium::kNoMediumProgram)
      throw std::invalid_argument("medium table entry " + std::to_string(i) + " was never placed (its program is the unplaced sentinel)");
    if (p >= count)
      throw std::invalid_argument("medium table entry " + std::to_string(i) + " names block " + std::to_string(p) +
                                  ", but the table holds " + std::to_string(count));
  }
}

std::vector<ar::MediumDesc> medium_table(const std::vector<std::shared_ptr<const Medium>>& media) {
  std::vector<ar::MediumDesc> out(media.size());
  for (uint32_t i = 0; i < media.size(); ++i) {
    out[i] = media[i]->desc;
    out[i].program = i;  // the block index: the block array is laid out in this order
  }
  check_medium_table(out.data(), uint32_t(out.size()));
  return out;
}

ar::DeviceInstanceRecord instance_record(const InstanceTable& table, uint32_t i, uint32_t mesh_index,
                                         const MediumIndex* media) {
  const Instance& s = table.instances[i];
  ar::DeviceInstanceRecord d{};
  d.mesh = mesh_index;
  if (s.medium_key != 0) {
    const auto it = media ? media->find(s.medium_key) : MediumIndex::const_iterator();
    if (!media || it == media->end())
      throw std::logic_error("instance " + std::to_string(i) + " bears medium key " + std::to_string(s.medium_key) +
                             ", which the medium table does not hold");
    d.medium = it->second + 1u;
  }
  d.random_id = s.random_id, d.pass_index = s.pass_index, d.crypto_object = s.crypto_object;
  d.crypto_asset = s.crypto_asset, d.flags = s.flags;
  d.material_offset = s.material_offset, d.material_count = s.material_count, d.light_index = s.light_index;
  d.mask = s.mask, d.light_group = s.light_group;
  std::memcpy(d.color, s.color, sizeof d.color);
  std::memcpy(d.object_to_world, table.centre(i).m, sizeof d.object_to_world);
  return d;
}

namespace {

// Builds `next` from the scene snapshot it names, reusing `prev`'s copies of unchanged sources.
// Inside a catch: a retryable failure becomes TableUploadFailed naming what failed; a sticky
// device error, host out-of-memory and anything already named keep their type (0001 D2.5).
[[noreturn]] void rethrow_upload(const std::string& what, uint64_t generation) {
  try {
    throw;
  } catch (const TableUploadFailed&) {
    throw;
  } catch (const DeviceError& e) {
    if (e.sticky()) throw;
    throw TableUploadFailed(what + ": " + e.what(), generation);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& e) {
    throw TableUploadFailed(what + ": " + e.what(), generation);
  }
}

class SetBuilder {
 public:
  SetBuilder(const UploadTarget& t, SceneTableSet& next, const SceneTableSet* prev) : t_(t), n_(next), p_(prev) {}

  // Keeps prev's copy of `id` when `same`, else makes one with `make`. A failure names the table.
  template <class F>
  void table(TableId id, bool same, uint32_t count, F&& make) {
    const uint32_t i = static_cast<uint32_t>(id);
    n_.counts[i] = count;
    if (same && p_ && p_->tables[i]) {
      n_.tables[i] = p_->tables[i];
      return;
    }
    try {
      n_.tables[i] = make();
    } catch (...) {
      rethrow_upload(std::string("uploading the ") + table_name(id), n_.generation);
    }
    n_.uploaded[i] = true;
    n_.bytes_moved += n_.tables[i]->moved();
  }
  std::shared_ptr<const DeviceTable> put(LedgerCategory c, const void* data, uint64_t bytes,
                                         std::shared_ptr<const void> pin = nullptr) {
    return DeviceTable::put(t_, c, data, bytes, std::move(pin));
  }

 private:
  const UploadTarget& t_;
  SceneTableSet& n_;
  const SceneTableSet* p_;
};

}  // namespace

void env_table_floats(const EnvTable& table, std::vector<float>* out) {
  out->clear();
  for (const EnvDistribution* d : {&table.plain, &table.compensated})
    for (const std::vector<float>* a : {&d->func, &d->cdf_u, &d->marginal_func, &d->marginal_cdf})
      out->insert(out->end(), a->begin(), a->end());
}

namespace {

// The bake of the world program in `n` (its world, image and constant tables already placed): a
// scratch SceneData and SceneTables naming them and a KernelGlobals naming those, on the scene's
// device, or on a CPU context on the cache's own CPU device, where every table is host memory read in
// place. The scratch buffers go when the bake returns; the baker caches its result under `key`.
BakedEnvironment bake_environment(Scene& scene, SceneTableCache& cache, const SceneTableSet& n, Device* scene_device,
                                  const std::vector<uint8_t>& key, std::shared_ptr<const void> key_pin) {
  Device* dev = scene_device;
  if (!dev) {
    if (!cache.bake_device) {
      if (!scene.meshes) throw std::logic_error("environment bake: a CPU context without a mesh store has no host ledger");
      const uint32_t forced = read_test_hooks().threads;
      cache.bake_ledger = scene.meshes->shared_ledger();
      cache.bake_pool = std::make_unique<ThreadPool>(scene.ctx(), forced ? forced : std::max(1u, scene.pool().workers()));
      cache.bake_device = make_cpu_device(*cache.bake_ledger, *cache.bake_pool);
    }
    dev = cache.bake_device.get();
  }
  const auto at = [&](TableId id) { return n.tables[static_cast<uint32_t>(id)]; };
  const auto cnt = [&](TableId id) { return n.counts[static_cast<uint32_t>(id)]; };
  ar::SceneData sd{};
  sd.world_program = as<uint32_t>(at(TableId::WORLD_PROGRAM)), sd.world_program_words = cnt(TableId::WORLD_PROGRAM);
  sd.world_constants = as<ar::ConstantBlock>(at(TableId::WORLD_CONSTANTS));
  sd.material_spectra = as<float>(at(TableId::MATERIAL_SPECTRA)), sd.material_spectra_count = cnt(TableId::MATERIAL_SPECTRA);
  ar::Rgb2SpecTable jt{};
  if (scene.materials) jt = scene.materials->table();
  jt.z_nodes = as<float>(at(TableId::RGB2SPEC_NODES)), jt.coeffs = as<float>(at(TableId::RGB2SPEC_COEFFS));
  const uint64_t o_vm = align16(sizeof(ar::SceneData)), o_jt = o_vm + align16(sizeof(ar::SceneTables));
  const uint64_t bytes = o_jt + sizeof(ar::Rgb2SpecTable);
  struct Scratch {
    Device& d;
    DeviceBuffer header, kg;
    ~Scratch() {
      for (DeviceBuffer* b : {&header, &kg}) try {
          d.free(*b);
        } catch (const std::exception& e) {
          std::fprintf(stderr, "astroray: freeing the environment bake's scratch failed: %s\n", e.what());
        }
    }
  } scratch{*dev, {}, {}};
  scratch.header = dev->allocate(LedgerCategory::TABLES, bytes, Owner{});
  scratch.kg = dev->allocate(LedgerCategory::TABLES, sizeof(ar::KernelGlobals), Owner{});
  const char* base = static_cast<const char*>(scratch.header.ptr);
  ar::SceneTables vt{};
  vt.rgb2spec = scene.materials ? reinterpret_cast<const ar::Rgb2SpecTable*>(base + o_jt) : nullptr;
  vt.observer = {as<float>(at(TableId::OBSERVER_CMF)), as<float>(at(TableId::OBSERVER_ILLUM))};
  vt.images = as<ar::DeviceImageView>(at(TableId::IMAGES)), vt.image_count = cnt(TableId::IMAGES);
  vt.srgb_decode = as<float>(at(TableId::SRGB_DECODE));
  std::vector<uint8_t> block(bytes, 0);
  std::memcpy(block.data(), &sd, sizeof sd);
  std::memcpy(block.data() + o_vm, &vt, sizeof vt);
  std::memcpy(block.data() + o_jt, &jt, sizeof jt);
  dev->upload(scratch.header, 0, block.data(), bytes);
  ar::KernelGlobals g{};
  g.scene = reinterpret_cast<const ar::SceneData*>(base);
  g.tables = reinterpret_cast<const ar::SceneTables*>(base + o_vm);
  dev->upload(scratch.kg, 0, &g, sizeof g);
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t before = cache.baker.rebuild_count();
  BakedEnvironment out = cache.baker.get(*dev, scratch.kg, key, key_pin, cache.bake_size);
  if (cache.baker.rebuild_count() != before) {
    char msg[200];
    std::snprintf(msg, sizeof msg, "environment bake: %u x %u cells on the %s device in %.1f ms (max %.6g, mean %.6g)",
                  cache.bake_size, cache.bake_size, scene_device ? "scene" : "CPU",
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), out.max, out.mean);
    scene.ctx().log.line("INFO", msg);
  }
  return out;
}

}  // namespace

std::shared_ptr<const SceneTableSet> acquire_scene_tables(Scene& scene) {
  // Lock order: the cache, then the tracker (inside snapshot), then the stores.
  SceneTableCache& cache = scene.table_cache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  SceneSnapshot snap = scene.snapshot(true);
  auto next = std::make_shared<SceneTableSet>();
  SceneTableSet& n = *next;
  n.generation = snap.generation;
  n.materials = std::move(snap.materials), n.images = std::move(snap.images);
  n.lights = std::move(snap.lights);
  const LightPlan plan = std::move(snap.light_plan);
  n.base_instances = std::move(snap.instances);
  n.mesh_records = std::move(snap.meshes);
  for (const auto& kv : n.mesh_records)
    if (!kv.second)
      throw TableUploadFailed("uploading the instances: an instance references geometry key " + std::to_string(kv.first) +
                                  ", which has no active mesh",
                              n.generation);
  n.media = std::move(snap.media);
  const SceneTableSet* p = cache.current.get();
  if (p && !plan.inputs && p->materials == n.materials && p->images == n.images && p->lights == n.lights &&
      p->base_instances == n.base_instances && p->mesh_records == n.mesh_records && p->media == n.media)
    return cache.current;

  Device* device = scene.device();  // waits for device init; its failure is not retryable
  const UploadTarget t{device, scene.meshes ? scene.meshes->shared_ledger() : nullptr, Owner{}};
  SetBuilder b(t, n, p);
  const auto sp = [](const auto& s) { return std::shared_ptr<const void>(s); };

  // Materials (059): the arrays in place, and the pool's keys resolved to header indices.
  const MaterialArrays* mats = n.materials.get();
  const bool mats_same = p && p->materials == n.materials;
  const uint32_t pool_count = n.base_instances ? uint32_t(n.base_instances->material_pool.size()) : 0u;
  b.table(TableId::MATERIAL_SLOTS, mats_same && p->base_instances == n.base_instances, pool_count, [&] {
    std::vector<uint32_t> slots(pool_count);
    for (uint32_t i = 0; i < pool_count; ++i) {
      const ar_key k = n.base_instances->material_pool[i];
      const auto it = mats ? std::lower_bound(mats->keys.begin(), mats->keys.end(), k) : std::vector<ar_key>::const_iterator();
      if (!mats || it == mats->keys.end() || *it != k)
        throw std::runtime_error("material pool entry " + std::to_string(i) + " names material key " +
                                 std::to_string(k) + ", which has no compiled material");
      slots[i] = uint32_t(it - mats->keys.begin());
    }
    return b.put(LedgerCategory::TABLES, slots.data(), slots.size() * 4);
  });
  const auto mp = mats ? sp(n.materials) : nullptr;
  // Headers, constants and pass indices hold the default material last (MaterialArrays), so
  // their counts are the scene's materials plus one (scene_view.h).
  b.table(TableId::MATERIAL_HEADERS, mats_same, mats ? uint32_t(mats->headers.size()) : 0u, [&] {
    return mats ? b.put(LedgerCategory::TABLES, mats->headers.data(), mats->headers.size() * sizeof(ar::MaterialHeader), mp)
                : b.put(LedgerCategory::TABLES, nullptr, 0);
  });
  b.table(TableId::MATERIAL_PROGRAMS, mats_same, mats ? uint32_t(mats->programs.size()) : 0u, [&] {
    return b.put(LedgerCategory::TABLES, mats ? mats->programs.data() : nullptr, mats ? mats->programs.size() * 4 : 0, mp);
  });
  b.table(TableId::MATERIAL_CONSTANTS, mats_same, mats ? uint32_t(mats->constants.size()) : 0u, [&] {
    return b.put(LedgerCategory::TABLES, mats ? mats->constants.data() : nullptr,
                 mats ? mats->constants.size() * sizeof(ar::ConstantBlock) : 0, mp);
  });
  b.table(TableId::MATERIAL_PASS_INDEX, mats_same, mats ? uint32_t(mats->pass_index.size()) : 0u, [&] {
    return b.put(LedgerCategory::TABLES, mats ? mats->pass_index.data() : nullptr, mats ? mats->pass_index.size() * 4 : 0, mp);
  });
  // The TABLE spectra the constant blocks index: empty, so a null pointer, when no material has one.
  b.table(TableId::MATERIAL_SPECTRA, mats_same, mats ? uint32_t(mats->spectra.size()) : 0u, [&] {
    return b.put(LedgerCategory::TABLES, mats ? mats->spectra.data() : nullptr, mats ? mats->spectra.size() * 4 : 0, mp);
  });

  // The media (181's store): one MediumDesc each, null exactly when the scene has none.
  b.table(TableId::MEDIA, p && p->media == n.media, uint32_t(n.media.size()), [&] {
    const std::vector<ar::MediumDesc> medium_descs = medium_table(n.media);
    return b.put(LedgerCategory::TABLES, medium_descs.data(), medium_descs.size() * sizeof(ar::MediumDesc));
  });
  // The world program and its block (0005 item 32): carried by the material arrays, so they are
  // kept exactly while those are.
  b.table(TableId::WORLD_PROGRAM, mats_same, mats ? uint32_t(mats->world_program.size()) : 0u, [&] {
    return b.put(LedgerCategory::TABLES, mats ? mats->world_program.data() : nullptr, mats ? mats->world_program.size() * 4 : 0, mp);
  });
  b.table(TableId::WORLD_CONSTANTS, mats_same, mats ? uint32_t(mats->world_constants.size()) : 0u, [&] {
    return b.put(LedgerCategory::TABLES, mats ? mats->world_constants.data() : nullptr,
                 mats ? mats->world_constants.size() * sizeof(ar::ConstantBlock) : 0, mp);
  });

  // Images (060): texels per slot, kept while the slot holds the same image.
  const ImageArrays* imgs = n.images.get();
  const size_t slots = imgs ? imgs->views.size() : 0;
  bool texels_same = p && p->texels.size() == slots;
  std::vector<ar::DeviceImageView> image_views(slots);
  for (size_t i = 0; i < slots; ++i) {
    const Image* src = imgs->images[i].get();
    if (p && i < p->texel_sources.size() && p->texel_sources[i] == src) {
      n.texels.push_back(p->texels[i]);
    } else {
      texels_same = false;
      try {
        n.texels.push_back(b.put(LedgerCategory::TEXTURES, src->texels.data(), src->texels.size(), sp(imgs->images[i])));
      } catch (...) {
        rethrow_upload("uploading the texels of image slot " + std::to_string(i), n.generation);
      }
      n.bytes_moved += n.texels.back()->moved();
    }
    n.texel_sources.push_back(src);
    image_views[i] = imgs->views[i];
    image_views[i].texels = addr(n.texels.back());
  }
  b.table(TableId::IMAGES, texels_same, uint32_t(slots),
          [&] { return b.put(LedgerCategory::TABLES, image_views.data(), slots * sizeof(ar::DeviceImageView)); });

  // Constant tables: made once per scene, then shared by every set.
  const ar::Rgb2SpecTable* jh = scene.materials ? &scene.materials->table() : nullptr;
  const auto store = jh ? std::shared_ptr<const void>(scene.materials) : nullptr;
  const uint64_t jh_res = jh ? uint64_t(jh->res) : 0;
  b.table(TableId::RGB2SPEC_NODES, true, uint32_t(jh_res),
          [&] { return b.put(LedgerCategory::TABLES, jh ? jh->z_nodes : nullptr, jh_res * 4, store); });
  b.table(TableId::RGB2SPEC_COEFFS, true, uint32_t(9 * jh_res * jh_res * jh_res),
          [&] { return b.put(LedgerCategory::TABLES, jh ? jh->coeffs : nullptr, 36 * jh_res * jh_res * jh_res, store); });
  if (!cache.observer) {
    DataBlob blob;
    if (!find_data("spectral/observer.bin", &blob)) throw std::runtime_error("scene tables: spectral/observer.bin is not embedded");
    auto o = std::make_shared<ObserverTables>();
    const std::string err = parse_observer(blob.data, blob.size, o.get());
    if (!err.empty()) throw std::runtime_error("scene tables: " + err);
    cache.observer = std::move(o);
  }
  const auto op = sp(cache.observer);
  b.table(TableId::OBSERVER_CMF, true, 3 * kObserverSamples,
          [&] { return b.put(LedgerCategory::TABLES, cache.observer->cmf, sizeof(cache.observer->cmf), op); });
  b.table(TableId::OBSERVER_ILLUM, true, kObserverSamples,
          [&] { return b.put(LedgerCategory::TABLES, cache.observer->d65, sizeof(cache.observer->d65), op); });
  // The host-built decode table (color/srgb.h), the one every ImageArrays carries.
  std::array<float, ar::kSrgbTableSize> srgb{};
  ar::srgb_decode_table(srgb.data());
  b.table(TableId::SRGB_DECODE, true, ar::kSrgbTableSize,
          [&] { return b.put(LedgerCategory::TABLES, srgb.data(), sizeof srgb); });
  DataBlob eblob;
  ar::EDielTable ev;
  if (!find_data("tables/e_diel_refl.bin", &eblob)) throw std::runtime_error("scene tables: tables/e_diel_refl.bin is not embedded");
  if (const char* err = ar::e_diel_view(eblob.data, eblob.size, &ev)) throw std::runtime_error(std::string("scene tables: ") + err);
  b.table(TableId::E_DIEL, true, ar::kEDielN * ar::kEDielN * ar::kEDielN, [&] {
    return b.put(LedgerCategory::TABLES, ev.e, uint64_t(ar::kEDielN) * ar::kEDielN * ar::kEDielN * 4, static_pin(eblob.data));
  });
  DataBlob sblob;
  ar::ESpecTable sv;
  if (!find_data("tables/e_spec.bin", &sblob)) throw std::runtime_error("scene tables: tables/e_spec.bin is not embedded");
  if (const char* err = ar::e_spec_view(sblob.data, sblob.size, &sv)) throw std::runtime_error(std::string("scene tables: ") + err);
  b.table(TableId::E_SPEC, true, ar::kESpecN * ar::kESpecN, [&] {
    return b.put(LedgerCategory::TABLES, sv.e, uint64_t(ar::kESpecN) * ar::kESpecN * 4, static_pin(sblob.data));
  });

  // The environment bake (0004 D11): after the world program, its images and the constant tables are
  // where the kernels read them, before the light table, which is built once with the bake in hand.
  if (plan.inputs) {
    const BakedEnvironment* baked = nullptr;
    BakedEnvironment result;
    bool refused = false;
    if (!plan.bake_key.empty()) {
      try {
        result = bake_environment(scene, cache, n, device, plan.bake_key, plan.images);
        baked = &result;
      } catch (const BakeRefused& e) {
        // Rule 5: the world renders black with one entry, never as a constant or the stale grid.
        refused = true;
        const uint64_t subject = plan.world ? plan.world->subject : 0u;
        const std::string why = std::string("world: ") + e.what() + "; the world renders black";
        scene.ctx().log.line("WARNING", why);
        scene.report().raise(AR_REPORT_PROGRAM_REJECTED, subject, why, 1, nullptr, n.generation);
      } catch (...) {
        rethrow_upload("baking the environment table", n.generation);
      }
    }
    n.lights = scene.build_lights(plan, baked, refused);
  }

  // Geometry: one copy per key, kept while its record is.
  bool meshes_same = p && p->mesh_records.size() == n.mesh_records.size();
  for (const auto& kv : n.mesh_records) {
    const MeshCopy* old = nullptr;
    if (p) {
      const auto it = p->meshes.find(kv.first);
      if (it != p->meshes.end()) {
        old = it->second.get();
        if (p->mesh_records.at(kv.first) == kv.second) {
          n.meshes[kv.first] = it->second;
          continue;
        }
      }
    }
    meshes_same = false;
    try {
      auto copy = upload_mesh(t, kv.second, old);
      n.bytes_moved += copy->positions->moved() + copy->corner_normals->moved();
      if (!old || old->topology_id != copy->topology_id)
        n.bytes_moved += copy->tri_vertex->moved() + copy->tri_corners->moved() + copy->uv->moved() + copy->tri_material->moved();
      n.meshes[kv.first] = std::move(copy);
    } catch (...) {
      rethrow_upload("uploading the geometry of key " + std::to_string(kv.first), n.generation);
    }
  }
  std::vector<ar::DeviceMeshView> views;
  std::map<ar_key, uint32_t> mesh_index;
  for (auto& kv : n.meshes) mesh_index[kv.first] = uint32_t(views.size()), views.push_back(kv.second->view);
  b.table(TableId::MESHES, meshes_same, uint32_t(views.size()),
          [&] { return b.put(LedgerCategory::TABLES, views.data(), views.size() * sizeof(ar::DeviceMeshView)); });

  // Instances: the scene's plus one record per lamp of the latched light table (068).
  const uint32_t base_count = n.base_instances ? uint32_t(n.base_instances->instances.size()) : 0u;
  const std::vector<ar::LightRecord>* lt_records = n.lights ? &n.lights->lights : nullptr;
  const std::shared_ptr<const InstanceTable> instances =
      n.lights ? append_lamp_instances(n.base_instances.get(), n.lights->lamp_instances) : n.base_instances;
  const uint32_t inst_count = instances ? uint32_t(instances->instances.size()) : 0u;
  // The records hold medium indices, which move with the media list.
  const MediumIndex media_of = medium_index(n.media);
  const bool inst_same =
      p && p->base_instances == n.base_instances && p->lights == n.lights && meshes_same && p->media == n.media;
  b.table(TableId::INSTANCES, inst_same, inst_count, [&] {
    std::vector<ar::DeviceInstanceRecord> r(inst_count);
    for (uint32_t i = 0; i < inst_count; ++i) {
      const bool lamp = i >= base_count;
      r[i] = instance_record(*instances, i, lamp ? kNoMeshIndex : mesh_index.at(instances->instances[i].geom_key), &media_of);
    }
    return b.put(LedgerCategory::TABLES, r.data(), r.size() * sizeof(ar::DeviceInstanceRecord));
  });

  // Traversal structures (114): the previous snapshot's when instances, lamps and mesh records are
  // all unchanged (a material or image edit), else built for this snapshot with every mesh
  // structure of an unchanged record carried over. An empty scene gets an empty valid structure.
  if (p && p->accel && p->base_instances == n.base_instances && p->lights == n.lights &&
      p->mesh_records == n.mesh_records) {
    n.accel = p->accel;
  } else {
    // A missing builder is a configuration error and must not turn into a retryable failure.
    AccelBuilder& builder = scene.accel_builder();
    try {
      AccelInput in;
      in.instances = instances.get();
      in.lights = lt_records;
      in.host_ledger = t.host_ledger;
      for (const auto& kv : n.mesh_records) in.meshes[kv.first] = AccelMesh{kv.second, n.meshes.at(kv.first)};
      n.accel = builder.build(in, p ? p->accel.get() : nullptr);
    } catch (...) {
      rethrow_upload("building the traversal structures", n.generation);
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "traversal structures of generation %llu: %u built, %u reused, %.2f ms",
                  static_cast<unsigned long long>(n.generation), n.accel->gas_built, n.accel->gas_reused,
                  n.accel->build_ms);
    scene.ctx().log.line("INFO", msg);
  }

  // The light table (067, 068): the session's epoch snapshot.
  const LightTable* lt = n.lights.get();
  const bool lights_same = p && p->lights == n.lights;
  const auto lp = lt ? sp(n.lights) : nullptr;
  const auto light_table = [&](TableId id, const void* data, size_t count, size_t elem) {
    b.table(id, lights_same, uint32_t(count), [&] { return b.put(LedgerCategory::TABLES, data, count * elem, lp); });
  };
  light_table(TableId::LIGHTS, lt ? lt->lights.data() : nullptr, lt ? lt->lights.size() : 0, sizeof(ar::LightRecord));
  light_table(TableId::LIGHT_BINS, lt ? lt->bins.data() : nullptr, lt ? lt->bins.size() : 0, sizeof(ar::AliasBin));
  light_table(TableId::LIGHT_BIN_LIGHT, lt ? lt->bin_light.data() : nullptr, lt ? lt->bin_light.size() : 0, 4);
  light_table(TableId::LIGHT_PMF, lt ? lt->pmf.data() : nullptr, lt ? lt->pmf.size() : 0, 4);
  light_table(TableId::LIGHT_INDEX, lt ? lt->light_index.data() : nullptr, lt ? lt->light_index.size() : 0, 4);

  // The environment table (0004 D11): the light table's bake, kept while it is the same table.
  const EnvTable* env = lt ? lt->env.get() : nullptr;
  uint64_t env_floats = 0;
  if (env)
    for (const EnvDistribution* d : {&env->plain, &env->compensated})
      env_floats += d->func.size() + d->cdf_u.size() + d->marginal_func.size() + d->marginal_cdf.size();
  b.table(TableId::ENV_TABLE, p && p->lights && lt && p->lights->env == lt->env, uint32_t(env_floats), [&] {
    std::vector<float> floats;
    if (env) env_table_floats(*env, &floats);
    return b.put(LedgerCategory::TABLES, floats.data(), floats.size() * 4);
  });

  // The header block: the structs KernelGlobals points to, holding the addresses above.
  const auto at = [&](TableId id) { return n.tables[static_cast<uint32_t>(id)]; };
  const auto cnt = [&](TableId id) { return n.counts[static_cast<uint32_t>(id)]; };
  ar::SceneData& sd = n.scene_host;
  sd.instances = as<ar::DeviceInstanceRecord>(at(TableId::INSTANCES)), sd.instance_count = cnt(TableId::INSTANCES);
  sd.meshes = as<ar::DeviceMeshView>(at(TableId::MESHES)), sd.mesh_count = cnt(TableId::MESHES);
  sd.material_slots = as<uint32_t>(at(TableId::MATERIAL_SLOTS));
  sd.material_headers = as<ar::MaterialHeader>(at(TableId::MATERIAL_HEADERS));
  // The header table's last entry is the default material, one past the counted headers.
  sd.material_header_count = cnt(TableId::MATERIAL_HEADERS) ? cnt(TableId::MATERIAL_HEADERS) - 1u : 0u;
  n.material_program_count = mats ? mats->program_count : 0u;
  sd.material_programs = as<uint32_t>(at(TableId::MATERIAL_PROGRAMS));
  sd.material_constants = as<ar::ConstantBlock>(at(TableId::MATERIAL_CONSTANTS));
  sd.material_pass_index = as<uint32_t>(at(TableId::MATERIAL_PASS_INDEX));
  sd.material_spectra = as<float>(at(TableId::MATERIAL_SPECTRA)), sd.material_spectra_count = cnt(TableId::MATERIAL_SPECTRA);
  sd.medium_descs = as<ar::MediumDesc>(at(TableId::MEDIA)), sd.medium_count = cnt(TableId::MEDIA);
  sd.world_program = as<uint32_t>(at(TableId::WORLD_PROGRAM)), sd.world_program_words = cnt(TableId::WORLD_PROGRAM);
  sd.world_constants = as<ar::ConstantBlock>(at(TableId::WORLD_CONSTANTS));
  sd.accel = n.accel->kernel_accel();
  ar::Rgb2SpecTable jt{};
  if (jh) jt = *jh;
  jt.z_nodes = as<float>(at(TableId::RGB2SPEC_NODES)), jt.coeffs = as<float>(at(TableId::RGB2SPEC_COEFFS));
  ar::LightSamplerView lv{as<ar::AliasBin>(at(TableId::LIGHT_BINS)), as<uint32_t>(at(TableId::LIGHT_BIN_LIGHT)),
                          as<float>(at(TableId::LIGHT_PMF)), cnt(TableId::LIGHT_BINS), cnt(TableId::LIGHTS)};
  const ar::EDielTable et{as<float>(at(TableId::E_DIEL))};
  const ar::ESpecTable st{as<float>(at(TableId::E_SPEC))};
  const uint64_t o_vm = align16(sizeof(ar::SceneData)), o_lv = o_vm + align16(sizeof(ar::SceneTables));
  const uint64_t o_et = o_lv + align16(sizeof(ar::LightSamplerView)), o_st = o_et + align16(sizeof(ar::EDielTable));
  const uint64_t o_jt = o_st + align16(sizeof(ar::ESpecTable));
  const uint64_t header_bytes = o_jt + sizeof(ar::Rgb2SpecTable);
  // The block's address is known only once it exists, and SceneTables points into it (rgb2spec):
  // allocate, then write the finished block over it.
  b.table(TableId::HEADER, false, 1, [&] {
    std::vector<uint8_t> block(header_bytes, 0);
    auto h = b.put(LedgerCategory::TABLES, block.data(), header_bytes);
    const char* base = static_cast<const char*>(h->ptr());
    ar::SceneTables vt{};
    vt.rgb2spec = jh ? reinterpret_cast<const ar::Rgb2SpecTable*>(base + o_jt) : nullptr;
    vt.observer = {as<float>(at(TableId::OBSERVER_CMF)), as<float>(at(TableId::OBSERVER_ILLUM))};
    vt.images = as<ar::DeviceImageView>(at(TableId::IMAGES)), vt.image_count = cnt(TableId::IMAGES);
    vt.srgb_decode = as<float>(at(TableId::SRGB_DECODE));
    std::memcpy(block.data(), &sd, sizeof sd);
    std::memcpy(block.data() + o_vm, &vt, sizeof vt);
    std::memcpy(block.data() + o_lv, &lv, sizeof lv);
    std::memcpy(block.data() + o_et, &et, sizeof et);
    std::memcpy(block.data() + o_st, &st, sizeof st);
    std::memcpy(block.data() + o_jt, &jt, sizeof jt);
    if (device) device->upload(h->buffer(), 0, block.data(), header_bytes);
    else std::memcpy(const_cast<void*>(h->ptr()), block.data(), header_bytes);  // our own host copy
    return h;
  });
  const char* base = static_cast<const char*>(at(TableId::HEADER)->ptr());
  n.scene = reinterpret_cast<const ar::SceneData*>(base);
  n.vm = reinterpret_cast<const ar::SceneTables*>(base + o_vm);
  n.light_sampler = reinterpret_cast<const ar::LightSamplerView*>(base + o_lv);
  n.e_diel = reinterpret_cast<const ar::EDielTable*>(base + o_et);
  n.e_spec = reinterpret_cast<const ar::ESpecTable*>(base + o_st);
  cache.current = std::move(next);
  return cache.current;
}

}  // namespace astroray::host

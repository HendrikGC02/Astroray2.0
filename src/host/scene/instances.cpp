// SPDX-License-Identifier: Apache-2.0
#include "host/scene/instances.h"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

#include "host/scene/edit.h"

namespace astroray::host {

namespace {

// Reserved item keys: an instance item has no key of its own but the edit needs one. They
// sort in call order, which is the order the generation's activation callbacks run in.
constexpr uint64_t kItemKeyBase = 0xF000000000000000ull;

constexpr uint32_t kAllVisibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE | AR_VISIBILITY_GLOSSY |
                                    AR_VISIBILITY_TRANSMISSION | AR_VISIBILITY_SHADOW | AR_VISIBILITY_VOLUME;
constexpr uint32_t kAllFlags = AR_INSTANCE_HOLDOUT | AR_INSTANCE_SHADOW_CATCHER | AR_INSTANCE_CASTER_OPT_OUT;

bool finite_xf(const ar_xform& x) {
  for (float f : x.m)
    if (!std::isfinite(f)) return false;
  return true;
}

// Owned copy of ar_instances_desc (the caller's arrays are copy-in).
struct DescCopy {
  uint32_t n = 0, steps = 1;
  std::vector<ar_key> geom, object, materials;
  std::vector<ar_xform> xf;
  std::vector<uint32_t> random_ids, pass_indices, crypto_objects, crypto_assets, visibility, flags, mat_offset,
      mat_count;
  std::vector<ar_rgba> colors;
  std::vector<uint8_t> light_groups;
  std::vector<bool> geom_declared;  // per instance, decided at the call (see set_instances)
};

template <class T>
bool take(const T* p, size_t n, std::vector<T>* out) {
  if (n != 0 && !p) return false;
  out->assign(p, p + n);
  return true;
}

std::string at(uint32_t i, ar_key object) {
  return "instance " + std::to_string(i) + " (object key " + std::to_string(object) + "): ";
}

std::shared_ptr<InstanceTable> build(const DescCopy& d, EditItem* item, uint64_t item_key) {
  auto t = std::make_shared<InstanceTable>();
  t->motion_steps = d.steps;
  t->instances.resize(d.n);
  t->xf = d.xf;
  t->material_pool = d.materials;
  for (uint32_t i = 0; i < d.n; ++i) {
    Instance& in = t->instances[i];
    in.geom_key = d.geom[i];
    in.object_key = d.object[i];
    // The report entry of a rejection names the instance's object key; the failing
    // instance leaves it in place when it throws.
    item->key = in.object_key;
    const std::string where = at(i, in.object_key);
    if (!d.geom_declared[i]) throw std::runtime_error(where + "unknown geometry key " + std::to_string(in.geom_key));
    for (uint32_t s = 0; s < d.steps; ++s)
      if (!finite_xf(d.xf[size_t(i) * d.steps + s]))
        throw std::runtime_error(where + "transform is not finite (motion step " + std::to_string(s) + ")");
    if (d.visibility[i] & ~kAllVisibility) throw std::runtime_error(where + "visibility has bits outside AR_VISIBILITY_*");
    if (d.flags[i] & ~kAllFlags) throw std::runtime_error(where + "flags has bits outside AR_INSTANCE_*");
    uint64_t end = uint64_t(d.mat_offset[i]) + d.mat_count[i];
    if (end > d.materials.size())
      throw std::runtime_error(where + "material slice [" + std::to_string(d.mat_offset[i]) + ", " + std::to_string(end) +
                               ") exceeds the material key table of " + std::to_string(d.materials.size()));
    for (float c : d.colors[i].v)
      if (!std::isfinite(c)) throw std::runtime_error(where + "colour is not finite");
    in.random_id = d.random_ids[i];
    in.pass_index = d.pass_indices[i];
    in.crypto_object = d.crypto_objects[i];
    in.crypto_asset = d.crypto_assets[i];
    for (int c = 0; c < 4; ++c) in.color[c] = d.colors[i].v[c];
    in.visibility = d.visibility[i];
    in.mask = visibility_mask(in.visibility);
    in.flags = d.flags[i];
    in.light_group = d.light_groups[i];
    in.material_offset = d.mat_offset[i];
    in.material_count = d.mat_count[i];
  }
  item->key = item_key;  // nothing failed
  return t;
}

}  // namespace

// Ref: 0002 D11.
uint8_t visibility_mask(uint32_t v) {
  uint8_t m = 0;
  if (v & AR_VISIBILITY_CAMERA) m |= 1u << 0;
  if (v & AR_VISIBILITY_DIFFUSE) m |= 1u << 1;
  if (v & AR_VISIBILITY_GLOSSY) m |= 1u << 2;
  if (v & AR_VISIBILITY_TRANSMISSION) m |= 1u << 3;
  if (v & AR_VISIBILITY_SHADOW) m |= 1u << 4;
  if (v & AR_VISIBILITY_VOLUME) m |= 1u << 5;
  return m;
}

ar_status set_instances(Edit& edit, const ar_instances_desc* desc) {
  if (!desc) return edit.fail("invalid argument: instances desc is null");
  if (desc->motion_steps == 0) return edit.fail("invalid argument: instances motion_steps is 0");
  uint64_t want_xf = uint64_t(desc->instance_count) * desc->motion_steps;
  if (desc->transform_count != want_xf)
    return edit.fail("invalid argument: instances transform_count " + std::to_string(desc->transform_count) +
                     " is not instance_count * motion_steps (" + std::to_string(want_xf) + ")");
  auto d = std::make_shared<DescCopy>();
  d->n = desc->instance_count;
  d->steps = desc->motion_steps;
  size_t n = d->n;
  if (!(take(desc->geom_keys, n, &d->geom) && take(desc->xf, want_xf, &d->xf) && take(desc->object_keys, n, &d->object) &&
        take(desc->random_ids, n, &d->random_ids) && take(desc->pass_indices, n, &d->pass_indices) &&
        take(desc->crypto_objects, n, &d->crypto_objects) && take(desc->crypto_assets, n, &d->crypto_assets) &&
        take(desc->colors, n, &d->colors) && take(desc->visibility, n, &d->visibility) &&
        take(desc->flags, n, &d->flags) && take(desc->light_groups, n, &d->light_groups) &&
        take(desc->material_offsets, n, &d->mat_offset) && take(desc->material_counts, n, &d->mat_count) &&
        take(desc->material_keys, desc->material_key_count, &d->materials)))
    return edit.fail("invalid argument: instances array pointer is null with a nonzero count");
  // Geometry declared by this edit or an earlier generation is resolved here, where the
  // edit's own staged keys are visible; the worker reports the first unknown one.
  d->geom_declared.resize(n);
  for (size_t i = 0; i < n; ++i) d->geom_declared[i] = edit.declares(AR_KIND_GEOMETRY, d->geom[i]);

  auto item = std::make_shared<EditItem>();
  const uint64_t item_key = kItemKeyBase + edit.instance_stage().next++;
  item->key = item_key;
  item->kind = AR_KIND_GEOMETRY;
  item->needs_device = false;
  auto result = std::make_shared<std::shared_ptr<InstanceTable>>();
  EditItem* raw = item.get();  // the pool's lambda keeps the item alive while the job runs
  item->job = [d, result, raw, item_key] { *result = build(*d, raw, item_key); };
  Scene* scene = &edit.scene();
  item->on_active = [scene, result] {
    if (*result) scene->publish_instances(*result);
  };
  item->on_dropped = [result] { result->reset(); };
  if (ar_status st = edit.stage(item)) return st;
  edit.instance_stage().count = d->n;
  edit.instance_stage().steps = d->steps;
  edit.instance_stage().has_set = true;
  return edit.item_commit(item_key);
}

ar_status update_transforms(Edit& edit, uint32_t n, const uint32_t* idx, const ar_xform* xf) {
  if (n != 0 && (!idx || !xf)) return edit.fail("invalid argument: update_transforms array pointer is null with n > 0");
  auto& stage = edit.instance_stage();
  uint32_t count = 0, steps = 1;
  if (stage.has_set) {
    count = stage.count;
    steps = stage.steps;
  } else {
    auto t = edit.scene().instances();
    if (!t) return edit.fail("invalid argument: update_transforms with no instances in the scene");
    count = static_cast<uint32_t>(t->instances.size());
    steps = t->motion_steps;
  }
  if (steps != 1) return edit.fail("invalid argument: update_transforms needs motion_steps == 1; use set_instances");
  for (uint32_t k = 0; k < n; ++k) {
    if (idx[k] >= count)
      return edit.fail("invalid argument: update_transforms index " + std::to_string(idx[k]) + " (entry " +
                       std::to_string(k) + ") is out of range, instance count " + std::to_string(count));
    if (!finite_xf(xf[k]))
      return edit.fail("invalid argument: update_transforms transform for instance " + std::to_string(idx[k]) +
                       " is not finite");
  }
  auto indices = std::make_shared<std::vector<uint32_t>>(idx, idx + n);
  auto xfs = std::make_shared<std::vector<ar_xform>>(xf, xf + n);
  auto item = std::make_shared<EditItem>();
  item->key = kItemKeyBase + stage.next++;
  item->kind = AR_KIND_GEOMETRY;
  item->needs_device = false;
  item->job = [] {};
  Scene* scene = &edit.scene();
  // Applied to whatever table is active then, so a patch behind a pipelined set_instances
  // lands on the new table; an entry that no longer fits is logged, never dropped silently.
  item->on_active = [scene, indices, xfs] {
    auto cur = scene->instances();
    if (!cur) {
      scene->ctx().log.line("WARNING", "update_transforms: no instances at activation, patch ignored");
      return;
    }
    auto t = std::make_shared<InstanceTable>(*cur);
    for (size_t k = 0; k < indices->size(); ++k) {
      if ((*indices)[k] >= t->instances.size() || t->motion_steps != 1) {
        scene->ctx().log.line("WARNING", "update_transforms: instance " + std::to_string((*indices)[k]) +
                                             " no longer exists at activation, patch entry ignored");
        continue;
      }
      t->xf[(*indices)[k]] = (*xfs)[k];
    }
    scene->publish_instances(std::move(t));
  };
  if (ar_status st = edit.stage(item)) return st;
  return edit.item_commit(item->key);
}

}  // namespace astroray::host

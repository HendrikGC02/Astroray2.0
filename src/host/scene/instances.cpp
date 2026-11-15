// SPDX-License-Identifier: Apache-2.0
#include "host/scene/instances.h"

#include <cmath>
#include <memory>
#include <set>
#include <string>

#include "host/scene/edit.h"

namespace astroray::host {

namespace {

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
  std::vector<bool> geom_exists;  // per instance, bound at commit against the edit's final key set
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

// A rejection names the instance's object key as the report subject.
[[noreturn]] void reject(uint32_t i, ar_key object, const std::string& why) {
  throw ItemFailure(object, at(i, object) + why);
}

std::shared_ptr<InstanceTable> build(const DescCopy& d) {
  auto t = std::make_shared<InstanceTable>();
  t->motion_steps = d.steps;
  t->instances.resize(d.n);
  t->xf = d.xf;
  t->material_pool = d.materials;
  for (uint32_t i = 0; i < d.n; ++i) {
    Instance& in = t->instances[i];
    in.geom_key = d.geom[i];
    in.object_key = d.object[i];
    if (!d.geom_exists[i]) reject(i, in.object_key, "unknown geometry key " + std::to_string(in.geom_key));
    for (uint32_t s = 0; s < d.steps; ++s)
      if (!finite_xf(d.xf[size_t(i) * d.steps + s]))
        reject(i, in.object_key, "transform is not finite (motion step " + std::to_string(s) + ")");
    if (d.visibility[i] & ~kAllVisibility) reject(i, in.object_key, "visibility has bits outside AR_VISIBILITY_*");
    if (d.flags[i] & ~kAllFlags) reject(i, in.object_key, "flags has bits outside AR_INSTANCE_*");
    // A count of 0 means the default material; its offset is not looked at.
    uint64_t end = uint64_t(d.mat_offset[i]) + d.mat_count[i];
    if (d.mat_count[i] != 0 && end > d.materials.size())
      reject(i, in.object_key, "material slice [" + std::to_string(d.mat_offset[i]) + ", " + std::to_string(end) +
                                   ") exceeds the material key table of " + std::to_string(d.materials.size()));
    for (float c : d.colors[i].v)
      if (!std::isfinite(c)) reject(i, in.object_key, "colour is not finite");
    in.random_id = d.random_ids[i];
    in.pass_index = d.pass_indices[i];
    in.crypto_object = d.crypto_objects[i];
    in.crypto_asset = d.crypto_assets[i];
    for (int c = 0; c < 4; ++c) in.color[c] = d.colors[i].v[c];
    in.visibility = d.visibility[i];
    in.mask = visibility_mask(in.visibility);
    in.flags = d.flags[i];
    in.light_group = d.light_groups[i];
    in.material_offset = d.mat_count[i] ? d.mat_offset[i] : 0;
    in.material_count = d.mat_count[i];
  }
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
  if (v & AR_VISIBILITY_SHADOW) m |= kShadowMaskBit;
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
  d->geom_exists.assign(n, false);

  auto item = std::make_shared<EditItem>();
  auto result = std::make_shared<std::shared_ptr<InstanceTable>>();
  // Resolved at commit, when this edit's geometry and removes are final.
  item->bind = [d](const std::function<bool(ar_key)>& exists) {
    for (size_t i = 0; i < d->n; ++i) d->geom_exists[i] = exists(d->geom[i]);
  };
  item->needs_device = false;
  item->job = [d, result] { *result = build(*d); };
  Scene* scene = &edit.scene();
  item->on_active = [scene, result] {
    if (*result) scene->publish_instances(*result);
  };
  item->on_dropped = [result] { result->reset(); };
  if (ar_status st = edit.stage_internal(item)) return st;
  edit.instance_stage().count = d->n;
  edit.instance_stage().steps = d->steps;
  edit.instance_stage().has_set = true;
  auto sum = std::make_shared<InstanceSummary>();
  sum->steps = d->steps;
  sum->geom = d->geom;
  sum->object = d->object;
  edit.instance_stage().summary = std::move(sum);
  return AR_OK;
}

ar_status update_transforms(Edit& edit, uint32_t n, const uint32_t* idx, const ar_xform* xf) {
  if (n != 0 && (!idx || !xf)) return edit.fail("invalid argument: update_transforms array pointer is null with n > 0");
  auto& stage = edit.instance_stage();
  uint32_t count = 0, steps = 1;
  if (stage.has_set) {
    count = stage.count;
    steps = stage.steps;
  } else {
    if (edit.reset_requested())
      return edit.fail("invalid argument: update_transforms after ar_edit_reset needs a set_instances in the same edit");
    auto t = edit.scene().latest_instances();
    if (!t) return edit.fail("invalid argument: update_transforms with no instances in the scene");
    count = static_cast<uint32_t>(t->geom.size());
    steps = t->steps;
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
  item->needs_device = false;
  item->job = [] {};
  Scene* scene = &edit.scene();
  // Applied to whatever table is active then, so a patch behind a pipelined set_instances
  // lands on the new table. An entry that no longer fits cannot reject a generation that
  // is already active: it is logged and put in the report, never dropped silently.
  item->on_active = [scene, indices, xfs] {
    auto cur = scene->instances();
    auto t = cur ? std::make_shared<InstanceTable>(*cur) : nullptr;
    for (size_t k = 0; k < indices->size(); ++k) {
      if (!t || (*indices)[k] >= t->instances.size() || t->motion_steps != 1) {
        const std::string why = "update_transforms: instance index " + std::to_string((*indices)[k]) +
                                " no longer exists at activation, patch entry ignored";
        scene->ctx().log.line("WARNING", why);
        const double value[2] = {0.0, 0.0};
        scene->report().raise(AR_REPORT_INPUT_IGNORED, (*indices)[k], why, 1, value, 0);
        continue;
      }
      t->xf[(*indices)[k]] = (*xfs)[k];
    }
    if (t) scene->publish_instances(std::move(t));
  };
  return edit.stage_internal(item);
}

void apply_instance_summary(std::shared_ptr<const InstanceSummary>& cur, const Pending& p) {
  if (p.inst_set)
    cur = p.inst_set;
  else if (p.inst_reset)
    cur.reset();
}

void queue_instance_check(Edit& edit) {
  if (edit.instance_stage().has_set || edit.reset_requested()) return;  // new table, or cleared
  auto removed = std::make_shared<std::set<uint64_t>>(edit.removed_geometry());
  if (removed->empty()) return;
  Scene* scene = &edit.scene();
  // The latest committed table, pending generations included, as of this commit.
  auto latest = scene->latest_instances();
  auto item = std::make_shared<EditItem>();
  item->needs_device = false;
  item->job = [scene, removed, latest] {
    auto fail = [&](uint32_t i, ar_key object, ar_key geom) {
      reject(i, object, "geometry key " + std::to_string(geom) + " is removed by this edit but still instanced");
    };
    if (latest)
      for (uint32_t i = 0; i < latest->geom.size(); ++i)
        if (removed->count(latest->geom[i])) fail(i, latest->object[i], latest->geom[i]);
    // Also the table active now: if a pending generation that set the latest one is
    // dropped, the active table is what remains.
    if (auto t = scene->instances())
      for (uint32_t i = 0; i < t->instances.size(); ++i)
        if (removed->count(t->instances[i].geom_key)) fail(i, t->instances[i].object_key, t->instances[i].geom_key);
  };
  edit.stage_internal(item);
}

}  // namespace astroray::host

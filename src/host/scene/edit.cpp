// SPDX-License-Identifier: Apache-2.0
#include "host/scene/edit.h"
#include "host/scene/mesh.h"

#include "host/scene/instances.h"

#include <cmath>
#include <exception>

namespace astroray::host {

namespace {
// Copies an AR_IN_ARRAY into an owned vector; a null pointer with a nonzero count is bad.
template <class T>
bool copy_in(const T* p, uint32_t n, std::vector<T>* out) {
  if (n != 0 && !p) return false;
  out->assign(p, p + n);
  return true;
}
}  // namespace

Edit::Edit(Scene& scene) : scene_(scene), pending_(std::make_shared<Pending>()) {}

Edit::~Edit() {
  if (!finished_) abort();
  scene_.close_edit();
}

ar_status Edit::bad(const std::string& why) {
  last_error_ = why;
  scene_.ctx().log.line("WARNING", why);
  return AR_ERR_INVALID_ARGUMENT;
}

ar_status Edit::touch() {
  if (finished_) return bad("invalid argument: the edit is already committed or aborted");
  any_call_ = true;
  return AR_OK;
}

ar_status Edit::reset() {
  if (finished_) return touch();
  if (any_call_) return bad("invalid argument: ar_edit_reset must be the first call of an edit");
  any_call_ = true;
  reset_ = true;
  return AR_OK;
}

ar_status Edit::stage(std::shared_ptr<EditItem> item) {
  if (ar_status st = touch()) return st;
  if (!item || !item->job) return bad("invalid argument: staged item without a job");
  if (item->kind > AR_KIND_ASTRO) return bad("invalid argument: edit kind out of range");
  if (staged_.count(item->key)) return bad("invalid argument: key " + std::to_string(item->key) + " is already staged in this edit");
  staged_[item->key].item = std::move(item);
  return AR_OK;
}

ar_status Edit::item_commit(ar_key key) {
  if (ar_status st = touch()) return st;
  auto it = staged_.find(key);
  if (it == staged_.end()) return bad("invalid argument: no staged item for key " + std::to_string(key));
  if (it->second.committed) return bad("invalid argument: key " + std::to_string(key) + " is already committed");
  it->second.committed = true;
  try {
    submit(it->second.item);
  } catch (...) {
    it->second.committed = false;
    throw;
  }
  return AR_OK;
}

bool Edit::final_geometry(ar_key key) const {
  auto it = staged_.find(key);
  if (it != staged_.end() && it->second.committed && it->second.item->kind == AR_KIND_GEOMETRY) return true;
  if (removed_.count({AR_KIND_GEOMETRY, key})) return false;
  return !reset_ && scene_.known(AR_KIND_GEOMETRY, key);
}

std::set<uint64_t> Edit::removed_geometry() const {
  std::set<uint64_t> out;
  for (const auto& r : removed_) {
    if (r.first != AR_KIND_GEOMETRY) continue;
    auto it = staged_.find(r.second);
    if (it != staged_.end() && it->second.item->kind == AR_KIND_GEOMETRY) continue;
    out.insert(r.second);
  }
  return out;
}

ar_status Edit::stage_internal(std::shared_ptr<EditItem> item) {
  if (ar_status st = touch()) return st;
  if (!item || !item->job) return bad("invalid argument: internal item without a job");
  internal_.push_back(std::move(item));
  return AR_OK;
}

void Edit::submit(const std::shared_ptr<EditItem>& item) {
  Scene* scene = &scene_;
  std::shared_ptr<Pending> pending = pending_;
  scene_.generations().job_started(*pending);
  try {
    auto task = [scene, pending, item] {
      std::string fail;
      uint64_t subject = item->key;
      try {
        if (item->needs_device) scene->wait_init();  // re-raises a stored init failure
        item->job();
      } catch (const ItemFailure& e) {
        fail = e.what()[0] ? e.what() : "job failed";
        subject = e.subject;
      } catch (const std::exception& e) {
        fail = e.what()[0] ? e.what() : "job failed";
      } catch (...) {
        fail = "job failed: unknown exception";
      }
      try {
        if (item->release) item->release();
      } catch (...) {
        // A throwing release must not hide the job's own outcome.
      }
      scene->generations().job_done(pending, subject, fail);
    };
    auto queue = [scene, task] { scene->pool().submit(task); };
    if (item->schedule) item->schedule(queue);
    else queue();
  } catch (...) {
    scene_.generations().job_done(pending, item->key, "");  // undo job_started
    throw;
  }
}

ar_status Edit::remove(uint32_t kind, ar_key key) {
  if (ar_status st = touch()) return st;
  if (kind > AR_KIND_ASTRO) return bad("invalid argument: edit kind out of range");
  bool known = false;
  if (!reset_) known = scene_.known(kind, key);
  if (!known) {
    auto it = staged_.find(key);
    known = (it != staged_.end() && it->second.committed && it->second.item->kind == kind) ||
            (kind == AR_KIND_ASTRO && astro_.count(key));
  }
  if (!known) return bad("invalid argument: remove of unknown key " + std::to_string(key));
  removed_.insert({kind, key});
  return AR_OK;
}

ar_status Edit::set_settings(const ar_settings* s) {
  if (ar_status st = touch()) return st;
  if (!s) return bad("invalid argument: settings is null");
  auto st = std::make_shared<StoredSettings>();
  st->s = *s;
  st->curve_nm.resize(s->channel_count);
  st->curve_value.resize(s->channel_count);
  if (!copy_in(s->passes, s->pass_count, &st->passes) || !copy_in(s->aov_names, s->aov_count, &st->aovs) ||
      !copy_in(s->channels, s->channel_count, &st->channels))
    return bad("invalid argument: settings array pointer is null with a nonzero count");
  for (uint32_t i = 0; i < s->channel_count; ++i) {
    ar_response_channel& c = st->channels[i];
    if (!copy_in(c.wavelength_nm, c.curve_count, &st->curve_nm[i]) || !copy_in(c.value, c.curve_count, &st->curve_value[i]))
      return bad("invalid argument: response channel curve pointer is null with a nonzero count");
    c.wavelength_nm = st->curve_nm[i].data();
    c.value = st->curve_value[i].data();
  }
  st->s.passes = st->passes.data();
  st->s.aov_names = st->aovs.data();
  st->s.channels = st->channels.data();
  settings_ = std::move(st);
  return AR_OK;
}

ar_status Edit::set_camera(const ar_camera_desc* c) {
  if (ar_status st = touch()) return st;
  if (!c) return bad("invalid argument: camera is null");
  auto sc = std::make_shared<StoredCamera>();
  sc->c = *c;
  if (!copy_in(c->motion_xf, c->motion_steps > 1 ? c->motion_steps : 0, &sc->motion))
    return bad("invalid argument: camera motion_xf is null with motion_steps > 1");
  sc->c.motion_xf = sc->motion.data();
  camera_ = std::move(sc);
  return AR_OK;
}

// Ref: 0007 decision 41 (stored, no effect until M10), task 032 Do 3 (the checks).
ar_status Edit::set_astro(ar_key key, const ar_astro_desc* a) {
  if (ar_status st = touch()) return st;
  if (!a) return bad("invalid argument: astro desc is null");
  if (a->astro_kind != AR_ASTRO_BLACK_HOLE) return bad("invalid argument: astro_kind out of range");
  for (float f : a->xf.m)
    if (!std::isfinite(f)) return bad("invalid argument: astro transform is not finite");
  if (!(std::isfinite(a->mass) && a->mass > 0)) return bad("invalid argument: astro mass must be finite and > 0");
  if (!(std::isfinite(a->spin) && std::fabs(a->spin) < 1)) return bad("invalid argument: astro |spin| must be < 1");
  if (!(std::isfinite(a->meters_per_rg) && a->meters_per_rg > 0))
    return bad("invalid argument: astro meters_per_rg must be finite and > 0");
  astro_[key] = *a;
  return AR_OK;
}

ar_status Edit::commit(uint64_t* generation) {
  if (ar_status st = touch()) return st;
  if (!generation) return bad("invalid argument: generation output is null");
  for (const auto& kv : staged_)
    if (!kv.second.committed) return bad("invalid argument: staged item for key " + std::to_string(kv.first) + " was not committed");
  if (settings_ && settings_->s.pixel_filter > AR_FILTER_BLACKMAN_HARRIS) return bad("invalid argument: pixel_filter out of range");
  if (camera_ && camera_->c.type > AR_CAMERA_PANO) return bad("invalid argument: camera type out of range");

  queue_instance_check(*this);
  Pending& p = *pending_;
  p.reset = reset_;
  Pending delta;  // just the key-set changes, copied into the activation callback
  delta.known_reset = reset_;
  delta.known_removed = removed_;
  for (const auto& kv : staged_) delta.known_added.insert({kv.second.item->kind, kv.first});
  for (const auto& kv : astro_) delta.known_added.insert({AR_KIND_ASTRO, kv.first});
  p.known_reset = delta.known_reset;
  p.known_removed = delta.known_removed;
  p.known_added = delta.known_added;
  delta.inst_reset = p.inst_reset = reset_;
  delta.inst_set = p.inst_set = instances_.summary;
  Scene::State& st = scene_.state_;
  for (const auto& kv : staged_) {
    if (kv.second.item->replaces_subject) p.subjects.push_back(kv.first);
    if (kv.second.item->on_active) p.on_active.push_back(kv.second.item->on_active);
    if (kv.second.item->on_dropped) p.on_dropped.push_back(kv.second.item->on_dropped);
  }
  for (const auto& r : removed_) p.subjects.push_back(r.second);
  for (const auto& kv : astro_) p.subjects.push_back(kv.first);
  // Geometry removals and reset take effect with the generation; keys staged in this edit stay.
  std::set<ar_key> keep;
  for (const auto& kv : staged_)
    if (kv.second.item->kind == AR_KIND_GEOMETRY && kv.second.item->replaces_subject) keep.insert(kv.first);
  if (scene_.meshes)
    p.on_active.push_back([meshes = scene_.meshes, reset = reset_, removed = removed_, keep] { meshes->remove(reset, removed, keep); });
  // The stored descriptors take effect with the generation, not before.
  p.on_active.push_back([&st, reset = reset_, settings = settings_, camera = camera_, astro = astro_,
                         removed = removed_, delta] {
    std::lock_guard<std::mutex> lock(st.mutex);
    apply_known(st.known_active, delta);
    apply_instance_summary(st.inst_active, delta);
    if (reset) {
      st.settings.reset();
      st.camera.reset();
      st.astro.clear();
      st.instances.reset();
      ++st.tlas_epoch;
    }
    for (const auto& r : removed)
      if (r.first == AR_KIND_ASTRO) st.astro.erase(r.second);
    if (settings) st.settings = settings;
    if (camera) st.camera = camera;
    for (const auto& kv : astro) st.astro[kv.first] = kv.second;
  });
  // Internal items run after the generation's own state callbacks (a reset clears first).
  for (const auto& item : internal_) {
    if (item->bind) item->bind([this](ar_key k) { return final_geometry(k); });
    if (item->on_active) p.on_active.push_back(item->on_active);
    if (item->on_dropped) p.on_dropped.push_back(item->on_dropped);
  }
  for (const auto& item : internal_) submit(item);
  *generation = scene_.generations().commit(pending_, [&] {
    std::lock_guard<std::mutex> lock(st.mutex);
    apply_known(st.known, p);
    apply_instance_summary(st.inst_latest, p);
  });
  finished_ = true;  // only now: nothing after the commit can throw
  return AR_OK;
}

ar_status Edit::abort() {
  if (finished_) return touch();
  finished_ = true;
  Pending& p = *pending_;
  for (auto& kv : staged_) {
    if (!kv.second.committed) {
      // Never handed to a worker: its blocks can go now.
      if (kv.second.item->release) kv.second.item->release();
      if (kv.second.item->on_dropped) kv.second.item->on_dropped();  // undo what staging declared
    } else if (kv.second.item->on_dropped) {
      p.on_dropped.push_back(kv.second.item->on_dropped);
    }
  }
  scene_.generations().abort(pending_);
  return AR_OK;
}

}  // namespace astroray::host

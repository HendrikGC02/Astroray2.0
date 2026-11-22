// SPDX-License-Identifier: Apache-2.0
// ar_edit_set_instances and ar_edit_update_transforms (0007 decision 5, edit map d12).
// Ref: 0007 decisions 5 and 12.
#include <exception>

#include "astroray/astroray.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "recorder_hooks.h"

namespace {
template <class F>
ar_status edit_call(ar_edit h, F&& f) {
  if (!h || !h->edit) return AR_ERR_INVALID_ARGUMENT;
  try {
    ar_status st = f(*h->edit);
    if (st != AR_OK) h->last_error = h->edit->last_error();
    return st;
  } catch (const std::exception& e) {
    try {
      h->last_error = e.what();
    } catch (...) {
      // a second failure while one is already being reported; the status code carries it.
    }
    return AR_ERR_INTERNAL;
  } catch (...) {
    try {
      h->last_error = "unknown exception";
    } catch (...) {
      // a second failure while one is already being reported; the status code carries it.
    }
    return AR_ERR_INTERNAL;
  }
}
}  // namespace

extern "C" {

AR_API ar_status ar_edit_set_instances(ar_edit h, const ar_instances_desc* desc) {
  AR_RECORD(ar_edit_set_instances, ar_rec_of(h), h, desc);
  AR_RECORD_BODY({
  return edit_call(h, [&](astroray::host::Edit& e) { return astroray::host::set_instances(e, desc); });
  });
}
AR_API ar_status ar_edit_update_transforms(ar_edit h, uint32_t n, const uint32_t* idx, const ar_xform* xf) {
  AR_RECORD(ar_edit_update_transforms, ar_rec_of(h), h, n, idx, xf);
  AR_RECORD_BODY({
  return edit_call(h, [&](astroray::host::Edit& e) { return astroray::host::update_transforms(e, n, idx, xf); });
  });
}

}  // extern "C"

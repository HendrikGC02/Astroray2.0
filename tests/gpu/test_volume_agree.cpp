// SPDX-License-Identifier: Apache-2.0
// The tracking cases of 0006 V1 on CUDA: the chromatic absorbing slab against its spectral truth and the
// grey scattering slab against the independent random walk, with the specs and checks the CPU twins in
// tests/cpu/test_shade_volume.cpp use (common/slab_rig.h). Each device is checked against the truth, not
// against the other, so a fault shared by both targets still fails; the gate runs the two files in one
// concurrent group (CPU and GPU gate tests).
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D8, V1; 0002 D12.
#include <doctest.h>

#include <memory>
#include <string>

#include "common/slab_rig.h"
#include "device/cuda/cuda_device.h"
#include "device/cuda/optix_accel.h"

using namespace astroray;
using namespace astroray::host;

namespace {

struct Held {
  std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
  std::unique_ptr<ModuleDevice> device;
  std::unique_ptr<AccelBuilder> accel;
};

SceneConfig cuda_config(const std::shared_ptr<Held>& h) {
  SceneConfig cfg;
  cfg.workers = 2;
  cfg.init = [h]() -> std::shared_ptr<void> {
    h->device = make_cuda_device(*h->ledger, 0);
    h->accel = make_optix_accel_builder(*h->device, 0, std::string());  // no OptiX disk cache
    return h;
  };
  cfg.device_of = [](void* p) -> Device* { return static_cast<Held*>(p)->device.get(); };
  cfg.accel_of = [](void* p) -> AccelBuilder* { return static_cast<Held*>(p)->accel.get(); };
  return cfg;
}

slab::Render render_cuda(slab::SlabRig& rig) {
  Ledger ledger;
  auto device = make_cuda_device(ledger, 0);
  slab::SlabSession session(rig, *device, Owner{0, 1});
  return session.converge();
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("volume_agree/chromatic slab cuda") {
  const auto held = std::make_shared<Held>();
  slab::SlabRig rig(cuda_config(held), slab::chromatic_spec());
  const slab::Render r = render_cuda(rig);
  slab::check_chromatic(rig, r, "volume_agree/chromatic slab cuda");
}

TEST_CASE("volume_agree/scattering slab cuda") {
  const auto held = std::make_shared<Held>();
  slab::SlabRig rig(cuda_config(held), slab::scattering_spec());
  const slab::Render r = render_cuda(rig);
  slab::check_scattering(r, "volume_agree/scattering slab cuda");
}

}  // TEST_SUITE

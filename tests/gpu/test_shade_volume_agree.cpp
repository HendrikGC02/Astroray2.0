// SPDX-License-Identifier: Apache-2.0
// The sigma = 0 slab on CUDA: a render with the medium is bit for bit the render of the same scene with
// the medium taken out (the shade_volume stage forwards every item unchanged), and the stage launches once
// per medium segment. One scene and one session device serve both renders; the second follows an edit
// that removes the medium. The CPU twin is tests/cpu/test_shade_volume.cpp; the picture is that of
// common/slab_rig.h.
// Ref: docs/decisions/0002-kernel-architecture.md D8, D11; 0006 V1.
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

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("shade_volume_agree/zero sigma bit identical on cuda") {
  const auto held = std::make_shared<Held>();
  slab::SlabRig rig(cuda_config(held), true);
  Ledger ledger;
  auto device = make_cuda_device(ledger, 0);
  const slab::Render with = slab::render_slab(rig, *device, Owner{0, 1});
  rig.drop_medium();
  const slab::Render without = slab::render_slab(rig, *device, Owner{0, 1});
  slab::check_identity(with, without);
}

}  // TEST_SUITE

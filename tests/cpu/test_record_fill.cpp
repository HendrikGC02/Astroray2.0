// SPDX-License-Identifier: Apache-2.0
// The record fill's spectral slots (resolve_constant_record, stages/shade_common.h): it resolves the
// nine slots in one loop with each slot's semantic as a runtime value, and every field must equal,
// bit for bit, the compile-time form resolve_slot<SEM> of that slot's own ref. Each slot holds a
// different ref, so a slot that reads another slot's ref, loses its semantic or is skipped differs:
// a CAUCHY_ABBE specular_ior (resolved only for an IOR slot), TABLE refs in REFLECTANCE slots and in
// the UNBOUNDED slot, RGB and CONSTANT refs (one above 1 and one below 0, which REFLECTANCE clamps).
// Ref: docs/decisions/0005-material-model.md items 2(c), 9, 10, 35; 0003 d8.
#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "stages/shade_common.h"

namespace {

using SK = ar::SpectrumKind;
using SS = ar::SpectrumSemantic;

const astroray::ObserverTables& observer() {
  static const astroray::ObserverTables t = [] {
    astroray::DataBlob blob;
    REQUIRE(astroray::find_data("spectral/observer.bin", &blob));
    astroray::ObserverTables o;
    const std::string err = astroray::parse_observer(blob.data, blob.size, &o);
    REQUIRE_MESSAGE(err.empty(), err);
    return o;
  }();
  return t;
}

ar::SpectrumRef ref(SK kind, SS sem, float p0, float p1 = 0.0f, float p2 = 0.0f, float p3 = 0.0f) {
  ar::SpectrumRef r{};
  r.kind = uint8_t(kind);
  r.semantic = uint8_t(sem);
  r.p[0] = p0, r.p[1] = p1, r.p[2] = p2, r.p[3] = p3;
  return r;
}

bool same_bits(const ar::Spec& a, const ar::Spec& b) { return std::memcmp(&a, &b, sizeof(ar::Spec)) == 0; }

}  // namespace

TEST_SUITE("fast") {
TEST_CASE("record_fill/every spectral slot equals its compile-time resolve") {
  const ar::ObserverView obs{&observer().cmf[0][0], observer().d65};
  // Two tables in one arena (n wavelengths, then n values): the second starts at index 8.
  const std::vector<float> arena = {400.0f, 480.0f, 560.0f, 700.0f, 0.2f, 0.9f, 0.4f, 0.7f,
                                    380.0f, 500.0f, 620.0f, 780.0f, 1.0f, 0.3f, 0.8f, 0.5f};
  ar::ConstantBlock b{};
  b.base_color = ref(SK::RGB, SS::REFLECTANCE, -2e-4f, 0.21f, -28.0f, 1.0f);
  b.specular_color = ref(SK::CONSTANT, SS::REFLECTANCE, 1.3f);
  b.specular_ior = ref(SK::CAUCHY_ABBE, SS::IOR, 1.52f, 30.0f, 1.0f);
  b.transmission_color = ar::make_table_ref(SS::REFLECTANCE, 0u, 4u, 1.5f);
  b.transmission_scatter = ref(SK::CONSTANT, SS::REFLECTANCE, -0.25f);
  b.subsurface_color = ref(SK::RGB, SS::REFLECTANCE, 1e-4f, -0.09f, 18.0f, 1.0f);
  b.subsurface_radius_scale = ar::make_table_ref(SS::UNBOUNDED, 8u, 4u, 2.0f);
  b.fuzz_color = ref(SK::CONSTANT, SS::REFLECTANCE, 0.4f);
  b.coat_color = ar::make_table_ref(SS::REFLECTANCE, 8u, 4u, 0.75f);
  ar::ShadeContext ctx{};
  ctx.N = ar::Real3{0.0f, 0.0f, 1.0f};
  ctx.lambda_t.nm[0] = 412.0f, ctx.lambda_t.nm[1] = 517.0f, ctx.lambda_t.nm[2] = 603.0f, ctx.lambda_t.nm[3] = 689.0f;
  ar::OpenPBRParams rec;
  std::memset(&rec, 0xff, sizeof(rec));  // NaN everywhere: a slot the fill skips cannot match
  ar::resolve_constant_record(b, ctx, obs, arena.data(), &rec);
  // The refs differ per lane where a slot can differ: Cauchy dispersion and the tables are not flat.
  const ar::Spec ior = ar::resolve_slot<SS::IOR>(b.specular_ior, ctx.lambda_t, obs, arena.data());
  CHECK(ior.v[0] > ior.v[3]);
  CHECK(ior.v[3] > 1.5f);
#define AR_X(n, s, sem)                                                                              \
  {                                                                                                  \
    INFO(#n);                                                                                        \
    CHECK(same_bits(rec.n, ar::resolve_slot<SS::sem>(b.n, ctx.lambda_t, obs, arena.data())));        \
  }
  AR_RECORD_SPECTRAL(AR_X)
#undef AR_X
}
}

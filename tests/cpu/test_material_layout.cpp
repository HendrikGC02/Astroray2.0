// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// 0005 items 2, 2a, 3, 9, 10: every record's size, field offsets and types, the constant block's
// order and defaults, and slot resolution of CONSTANT and RGB sources.
// Ref: docs/decisions/0005-material-model.md; OpenPBR Surface 1.1.2 (inputs and defaults).
#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#include "../../src/host/data_resource.h"
#include "color/observer.h"
#include "color/rgb2spec.h"
#include "material/constant_block.h"
#include "material/ext_params.h"
#include "material/material_header.h"
#include "material/openpbr_params.h"
#include "material/slot_resolve.h"
#include "material/spectrum_ref.h"
#include "vm/vm_regs.h"

namespace {

using ar::Lambda;
using ar::Real;
using ar::Rgb;
using ar::Spec;
using ar::SpectrumSemantic;

std::vector<uint8_t> read_data(const std::string& rel) {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/" + rel, std::ios::binary);
  if (!f) f.open("data/" + rel, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/" << rel << " not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Fixture {
  std::vector<uint8_t> jh_bytes;
  astroray::ObserverTables obs;
  ar::Rgb2SpecTable table;
  ar::ObserverView view;
  Fixture() {
    auto ob = read_data("spectral/observer.bin");
    std::string err = astroray::parse_observer(ob.data(), ob.size(), &obs);
    REQUIRE_MESSAGE(err.empty(), err);
    jh_bytes = read_data("spectral/rec709.jh");
    const char* e = ar::jh_view(jh_bytes.data(), jh_bytes.size(), &table);
    REQUIRE_MESSAGE(e == nullptr, (e ? e : ""));
    view.cmf = &obs.cmf[0][0];
    view.illum = obs.d65;
  }
};

Lambda lanes(Real a, Real b, Real c, Real d) { return Lambda{{a, b, c, d}}; }

// One record field as the record states it: its offset and size are compared with the running
// layout, and its type with the expected one (a mismatch is a bool, so every field reports).
struct Field {
  const char* name;
  size_t offset, size;
  bool type_ok;
};
#define FIELD(S, n, T) Field{#n, offsetof(S, n), sizeof(S::n), std::is_same<decltype(S::n), T>::value}

// Fields laid end to end from `base`, no padding between them.
void check_sequence(const Field* f, size_t count, size_t base, size_t* end) {
  size_t off = base;
  for (size_t i = 0; i < count; ++i) {
    INFO(f[i].name);
    CHECK(f[i].offset == off);
    CHECK(f[i].type_ok);
    off += f[i].size;
  }
  *end = off;
}

// Worst |got - want| over lanes, NaN kept (never skipped).
double lane_error(const Spec& s, double want) {
  double worst = 0;
  for (int i = 0; i < ar::kLanes; ++i) {
    const double e = std::fabs(double(s.v[i]) - want);
    if (!(e <= worst)) worst = e;
  }
  return worst;
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("material_layout/record size") {
    using ar::MaterialHeader;
    using ar::OpenPBRParams;
    using ar::SpectrumRef;
    size_t end = 0;

    CHECK(sizeof(MaterialHeader) == 16);
    const Field header[] = {FIELD(MaterialHeader, program_offset, uint32_t), FIELD(MaterialHeader, const_offset, uint32_t),
                            FIELD(MaterialHeader, ext, uint32_t), FIELD(MaterialHeader, flags, uint32_t)};
    check_sequence(header, 4, 0, &end);
    CHECK(end == 16);

    CHECK(sizeof(SpectrumRef) == 24);
    const Field ref[] = {FIELD(SpectrumRef, kind, uint8_t), FIELD(SpectrumRef, semantic, uint8_t),
                         FIELD(SpectrumRef, aux, uint16_t), FIELD(SpectrumRef, index, uint32_t),
                         FIELD(SpectrumRef, p, float[4])};
    check_sequence(ref, 5, 0, &end);
    CHECK(end == 24);

    CHECK(sizeof(ar::VmRegs) == 128);
    const Field regs[] = {FIELD(ar::VmRegs, r, float[32])};
    check_sequence(regs, 1, 0, &end);
    CHECK(end == 128);

    // 0005 item 2(c) table, in its order: 24 scalars, 9 spectral slots, 4 frame vectors,
    // metal_ior, flags; the record is padded to 304 B.
    const Field scalars[] = {
        FIELD(OpenPBRParams, base_weight, Real), FIELD(OpenPBRParams, base_diffuse_roughness, Real),
        FIELD(OpenPBRParams, base_metalness, Real), FIELD(OpenPBRParams, specular_weight, Real),
        FIELD(OpenPBRParams, specular_roughness, Real), FIELD(OpenPBRParams, specular_roughness_anisotropy, Real),
        FIELD(OpenPBRParams, transmission_weight, Real), FIELD(OpenPBRParams, transmission_depth, Real),
        FIELD(OpenPBRParams, transmission_scatter_anisotropy, Real), FIELD(OpenPBRParams, subsurface_weight, Real),
        FIELD(OpenPBRParams, subsurface_radius, Real), FIELD(OpenPBRParams, subsurface_scatter_anisotropy, Real),
        FIELD(OpenPBRParams, fuzz_weight, Real), FIELD(OpenPBRParams, fuzz_roughness, Real),
        FIELD(OpenPBRParams, coat_weight, Real), FIELD(OpenPBRParams, coat_roughness, Real),
        FIELD(OpenPBRParams, coat_roughness_anisotropy, Real), FIELD(OpenPBRParams, coat_ior, Real),
        FIELD(OpenPBRParams, coat_darkening, Real), FIELD(OpenPBRParams, thin_film_weight, Real),
        FIELD(OpenPBRParams, thin_film_thickness, Real), FIELD(OpenPBRParams, thin_film_ior, Real),
        FIELD(OpenPBRParams, geometry_opacity, Real), FIELD(OpenPBRParams, closure_weight, Real)};
    check_sequence(scalars, 24, 0, &end);
    CHECK(end == 24 * sizeof(Real));
    const Field slots[] = {
        FIELD(OpenPBRParams, base_color, Spec), FIELD(OpenPBRParams, specular_color, Spec),
        FIELD(OpenPBRParams, specular_ior, Spec), FIELD(OpenPBRParams, transmission_color, Spec),
        FIELD(OpenPBRParams, transmission_scatter, Spec), FIELD(OpenPBRParams, subsurface_color, Spec),
        FIELD(OpenPBRParams, subsurface_radius_scale, Spec), FIELD(OpenPBRParams, fuzz_color, Spec),
        FIELD(OpenPBRParams, coat_color, Spec)};
    check_sequence(slots, 9, end, &end);
    CHECK(end == (24 * sizeof(Real) + 9 * sizeof(Spec)));
    const Field frame[] = {FIELD(OpenPBRParams, geometry_normal, ar::Real3), FIELD(OpenPBRParams, geometry_coat_normal, ar::Real3),
                           FIELD(OpenPBRParams, geometry_tangent, ar::Real3),
                           FIELD(OpenPBRParams, geometry_coat_tangent, ar::Real3)};
    check_sequence(frame, 4, end, &end);
    const Field tail[] = {FIELD(OpenPBRParams, metal_ior, uint32_t), FIELD(OpenPBRParams, flags, uint32_t)};
    check_sequence(tail, 2, end, &end);
    CHECK(end == 24 * sizeof(Real) + 9 * sizeof(Spec) + 12 * sizeof(Real) + 8);
    if constexpr (sizeof(Real) == 4) {
      CHECK(end == 296);
      CHECK(sizeof(OpenPBRParams) <= 304);
    }
    CHECK(sizeof(OpenPBRParams) >= end);
  }

  // 0005 item 10's table (and item 2a for interior_ior), written out here, not read from the header.
  AR_RT_CASE("material_layout/slot semantic table") {
    const SpectrumSemantic want[] = {
        SpectrumSemantic::REFLECTANCE,  // base_color
        SpectrumSemantic::REFLECTANCE,  // specular_color
        SpectrumSemantic::IOR,          // specular_ior
        SpectrumSemantic::REFLECTANCE,  // transmission_color
        SpectrumSemantic::REFLECTANCE,  // transmission_scatter
        SpectrumSemantic::REFLECTANCE,  // subsurface_color
        SpectrumSemantic::UNBOUNDED,    // subsurface_radius_scale
        SpectrumSemantic::REFLECTANCE,  // fuzz_color
        SpectrumSemantic::REFLECTANCE,  // coat_color
        SpectrumSemantic::ILLUMINANT,   // emission_color
        SpectrumSemantic::IOR};         // interior_ior
    CHECK(int(ar::Slot::COUNT_) == 11);
    CHECK(int(ar::Slot::BASE_COLOR) == 0);
    CHECK(int(ar::Slot::INTERIOR_IOR) == 10);
    for (int i = 0; i < 11; ++i) {
      INFO("slot " << i);
      CHECK(ar::kSlotSemantic[i] == want[i]);
    }
  }

  AR_RT_CASE("material_layout/header packing and enums") {
    // ext = ext_kind u8 | ext_index u24; the six flag bits are distinct single bits.
    ar::MaterialHeader h = {};
    h.ext = ar::ext_pack(3u, 0x123456u);
    CHECK(ar::ext_kind(h) == 3u);
    CHECK(ar::ext_index(h) == 0x123456u);
    // The largest legal values round-trip; overflow is refused, never truncated.
    uint32_t packed = 0;
    REQUIRE(ar::ext_pack_checked(0xffu, ar::kExtIndexMax, &packed));
    CHECK(packed == 0xffffffffu);
    h.ext = packed;
    CHECK(ar::ext_kind(h) == 0xffu);
    CHECK(ar::ext_index(h) == 0xffffffu);
    packed = 0xdeadbeefu;
    CHECK(!ar::ext_pack_checked(0x100u, 0u, &packed));
    CHECK(!ar::ext_pack_checked(0u, ar::kExtIndexMax + 1u, &packed));
    CHECK(!ar::ext_pack_checked(0u, 0xffffffffu, &packed));
    CHECK(packed == 0xdeadbeefu);
    const uint32_t bits[] = {ar::MAY_BE_DELTA,       ar::NON_OPAQUE,         ar::EXTENSION,
                             ar::READS_RAY_TYPE,     ar::CONSTANT_SURFACE,   ar::CONSTANT_EMISSION};
    uint32_t seen = 0;
    for (uint32_t b : bits) {
      CHECK((b & (b - 1u)) == 0u);
      CHECK((seen & b) == 0u);
      seen |= b;
    }
    CHECK(seen == 0x3fu);
    // 0005 items 14 and 16: the closed kind list and the 64 B constants block.
    CHECK(sizeof(ar::ExtParams) == 64);
    CHECK(uint32_t(ar::ExtKind::NONE) == 0u);
    CHECK(uint32_t(ar::ExtKind::GRATING) == 1u);
    CHECK(uint32_t(ar::ExtKind::HAIR) == 2u);
    CHECK(uint32_t(ar::ExtKind::FLUORESCENT) == 3u);
    CHECK(uint32_t(ar::ExtKind::MULTILAYER) == 4u);
    CHECK(uint32_t(ar::ExtKind::MEASURED) == 5u);
    // 0005 item 9 kind list and item 10 semantics, in the record's order.
    CHECK(int(ar::SpectrumKind::CONSTANT) == 0);
    CHECK(int(ar::SpectrumKind::RGB) == 1);
    CHECK(int(ar::SpectrumKind::TABLE) == 2);
    CHECK(int(ar::SpectrumKind::BLACKBODY) == 3);
    CHECK(int(ar::SpectrumKind::CAUCHY_ABBE) == 4);
    CHECK(int(ar::SpectrumKind::SELLMEIER) == 5);
    CHECK(int(ar::SpectrumKind::GAUSSIAN) == 6);
    CHECK(int(SpectrumSemantic::REFLECTANCE) == 0);
    CHECK(int(SpectrumSemantic::UNBOUNDED) == 1);
    CHECK(int(SpectrumSemantic::ILLUMINANT) == 2);
    CHECK(int(SpectrumSemantic::IOR) == 3);
  }

  // OpenPBR 1.1.2's 41 inputs in specification order, then item 2a's two fields. Scalars f32,
  // colour inputs and specular_ior SpectrumRef, thin_walled a flag, frame vectors 3 x f32.
  AR_RT_CASE("material_layout/constant block order") {
    using ar::ConstantBlock;
    using ar::SpectrumRef;
    using F3 = float[3];
    const Field in[] = {
        FIELD(ConstantBlock, base_weight, float), FIELD(ConstantBlock, base_color, SpectrumRef),
        FIELD(ConstantBlock, base_diffuse_roughness, float), FIELD(ConstantBlock, base_metalness, float),
        FIELD(ConstantBlock, specular_weight, float), FIELD(ConstantBlock, specular_color, SpectrumRef),
        FIELD(ConstantBlock, specular_roughness, float), FIELD(ConstantBlock, specular_roughness_anisotropy, float),
        FIELD(ConstantBlock, specular_ior, SpectrumRef), FIELD(ConstantBlock, transmission_weight, float),
        FIELD(ConstantBlock, transmission_color, SpectrumRef), FIELD(ConstantBlock, transmission_depth, float),
        FIELD(ConstantBlock, transmission_scatter, SpectrumRef),
        FIELD(ConstantBlock, transmission_scatter_anisotropy, float),
        FIELD(ConstantBlock, transmission_dispersion_scale, float),
        FIELD(ConstantBlock, transmission_dispersion_abbe_number, float), FIELD(ConstantBlock, subsurface_weight, float),
        FIELD(ConstantBlock, subsurface_color, SpectrumRef), FIELD(ConstantBlock, subsurface_radius, float),
        FIELD(ConstantBlock, subsurface_radius_scale, SpectrumRef),
        FIELD(ConstantBlock, subsurface_scatter_anisotropy, float), FIELD(ConstantBlock, fuzz_weight, float),
        FIELD(ConstantBlock, fuzz_color, SpectrumRef), FIELD(ConstantBlock, fuzz_roughness, float),
        FIELD(ConstantBlock, coat_weight, float), FIELD(ConstantBlock, coat_color, SpectrumRef),
        FIELD(ConstantBlock, coat_roughness, float), FIELD(ConstantBlock, coat_roughness_anisotropy, float),
        FIELD(ConstantBlock, coat_ior, float), FIELD(ConstantBlock, coat_darkening, float),
        FIELD(ConstantBlock, thin_film_weight, float), FIELD(ConstantBlock, thin_film_thickness, float),
        FIELD(ConstantBlock, thin_film_ior, float), FIELD(ConstantBlock, emission_luminance, float),
        FIELD(ConstantBlock, emission_color, SpectrumRef), FIELD(ConstantBlock, geometry_opacity, float),
        FIELD(ConstantBlock, geometry_thin_walled, uint32_t), FIELD(ConstantBlock, geometry_normal, F3),
        FIELD(ConstantBlock, geometry_coat_normal, F3), FIELD(ConstantBlock, geometry_tangent, F3),
        FIELD(ConstantBlock, geometry_coat_tangent, F3)};
    const size_t n_in = sizeof(in) / sizeof(in[0]);
    CHECK(n_in == 41);
    size_t end = 0;
    check_sequence(in, n_in, 0, &end);
    const Field tail[] = {FIELD(ConstantBlock, interior_ior, SpectrumRef), FIELD(ConstantBlock, medium_priority, uint8_t)};
    check_sequence(tail, 2, end, &end);
    CHECK(end + 3 == sizeof(ConstantBlock));  // 3 bytes of explicit padding after medium_priority
    CHECK(sizeof(ConstantBlock) == 424);
  }

  AR_RT_CASE("material_layout/constant block defaults") {
    Fixture fx;
    const ar::ConstantBlock b = ar::default_constant_block(fx.table);
    // Scalars: every default of OpenPBR 1.1.2 (lengths in metres: 1 cm radius = 0.01).
    CHECK(b.base_weight == 1.0f);
    CHECK(b.base_diffuse_roughness == 0.0f);
    CHECK(b.base_metalness == 0.0f);
    CHECK(b.specular_weight == 1.0f);
    CHECK(b.specular_roughness == 0.3f);
    CHECK(b.specular_roughness_anisotropy == 0.0f);
    CHECK(b.transmission_weight == 0.0f);
    CHECK(b.transmission_depth == 0.0f);
    CHECK(b.transmission_scatter_anisotropy == 0.0f);
    CHECK(b.transmission_dispersion_scale == 0.0f);
    CHECK(b.transmission_dispersion_abbe_number == 20.0f);
    CHECK(b.subsurface_weight == 0.0f);
    CHECK(b.subsurface_radius == 0.01f);
    CHECK(b.subsurface_scatter_anisotropy == 0.0f);
    CHECK(b.fuzz_weight == 0.0f);
    CHECK(b.fuzz_roughness == 0.5f);
    CHECK(b.coat_weight == 0.0f);
    CHECK(b.coat_roughness == 0.0f);
    CHECK(b.coat_roughness_anisotropy == 0.0f);
    CHECK(b.coat_ior == 1.6f);
    CHECK(b.coat_darkening == 1.0f);
    CHECK(b.thin_film_weight == 0.0f);
    CHECK(b.thin_film_thickness == 0.5f);
    CHECK(b.thin_film_ior == 1.4f);
    CHECK(b.emission_luminance == 0.0f);
    CHECK(b.geometry_opacity == 1.0f);
    CHECK(b.geometry_thin_walled == 0u);
    for (int i = 0; i < 3; ++i) {
      CHECK(b.geometry_normal[i] == 0.0f);
      CHECK(b.geometry_coat_normal[i] == 0.0f);
      CHECK(b.geometry_tangent[i] == 0.0f);
      CHECK(b.geometry_coat_tangent[i] == 0.0f);
    }
    CHECK(b.medium_priority == 0);

    // Colour and IOR slots: semantic from the table, value at four wavelengths.
    const Lambda lam = lanes(Real(450), Real(520), Real(590), Real(660));
    struct Want {
      ar::Slot slot;
      double value;
    };
    const Want want[] = {{ar::Slot::BASE_COLOR, 0.8},         {ar::Slot::SPECULAR_COLOR, 1.0},
                         {ar::Slot::SPECULAR_IOR, 1.5},       {ar::Slot::TRANSMISSION_COLOR, 1.0},
                         {ar::Slot::TRANSMISSION_SCATTER, 0.0}, {ar::Slot::SUBSURFACE_COLOR, 0.8},
                         {ar::Slot::FUZZ_COLOR, 1.0},         {ar::Slot::COAT_COLOR, 1.0},
                         {ar::Slot::INTERIOR_IOR, 1.5}};
    ar::ConstantBlock mb = b;
    for (const Want& w : want) {
      const ar::SpectrumRef& r = ar::slot_ref(mb, w.slot);
      INFO("slot " << int(w.slot));
      CHECK(r.semantic == uint8_t(ar::kSlotSemantic[int(w.slot)]));
      CHECK(lane_error(ar::resolve(r, lam, fx.view, nullptr), w.value) <= 1e-4);
    }
    CHECK(ar::slot_ref(mb, ar::Slot::SPECULAR_IOR).kind == uint8_t(ar::SpectrumKind::CONSTANT));
    CHECK(ar::slot_ref(mb, ar::Slot::BASE_COLOR).kind == uint8_t(ar::SpectrumKind::RGB));
    // 0005 item 10's table.
    CHECK(ar::kSlotSemantic[int(ar::Slot::SUBSURFACE_RADIUS_SCALE)] == SpectrumSemantic::UNBOUNDED);
    CHECK(ar::kSlotSemantic[int(ar::Slot::EMISSION_COLOR)] == SpectrumSemantic::ILLUMINANT);
    CHECK(ar::kSlotSemantic[int(ar::Slot::SPECULAR_IOR)] == SpectrumSemantic::IOR);
    CHECK(ar::kSlotSemantic[int(ar::Slot::BASE_COLOR)] == SpectrumSemantic::REFLECTANCE);
    // subsurface_radius_scale (1, 0.5, 0.25): red end above green above blue.
    const ar::SpectrumRef& rs = ar::slot_ref(mb, ar::Slot::SUBSURFACE_RADIUS_SCALE);
    const Spec s = ar::resolve(rs, lanes(Real(620), Real(540), Real(460), Real(620)), fx.view, nullptr);
    CHECK(s.v[0] > s.v[1]);
    CHECK(s.v[1] > s.v[2]);
  }

  AR_RT_CASE("material_layout/rgb white reflectance") {
    Fixture fx;
    ar::SpectrumRef r;
    uint32_t flags = 0;
    REQUIRE(ar::make_rgb_ref(fx.table, Rgb{{Real(1), Real(1), Real(1)}}, SpectrumSemantic::REFLECTANCE, &r, &flags));
    CHECK(flags == 0u);
    double worst = 0;
    for (int i = 0; i < ar::kObsSamples; i += 4) {
      Lambda lam;
      for (int l = 0; l < 4; ++l) lam.nm[l] = Real(360 + (i + l < ar::kObsSamples ? i + l : ar::kObsSamples - 1));
      const double e = lane_error(ar::resolve(r, lam, fx.view, nullptr), 1.0);
      if (!(e <= worst)) worst = e;
    }
    CHECK_MESSAGE(worst <= 1e-4, "max |s - 1| = " << worst);

    // A chromatic colour exercises the stored coefficients and scale, not just the grey path:
    // the ref must give what the upsampling functions give for the same convention.
    const Rgb rgb{{Real(0.2), Real(0.5), Real(0.8)}}, big{{Real(2.5), Real(1.0), Real(0.1)}};
    const Lambda lam = lanes(Real(430), Real(510), Real(580), Real(640));
    uint32_t f2 = 0, f3 = 0, f4 = 0, f5 = 0;
    ar::SpectrumRef a, u, il;
    REQUIRE(ar::make_rgb_ref(fx.table, rgb, SpectrumSemantic::REFLECTANCE, &a, &f2));
    REQUIRE(ar::make_rgb_ref(fx.table, big, SpectrumSemantic::UNBOUNDED, &u, &f3));
    REQUIRE(ar::make_rgb_ref(fx.table, big, SpectrumSemantic::ILLUMINANT, &il, &f4));
    const Spec want_a = ar::albedo(fx.table, rgb, lam, &f5);
    const Spec want_u = ar::unbounded(fx.table, big, lam, &f5);
    const Spec want_i = ar::illuminant(fx.table, fx.view, big, lam, &f5);
    const Spec got_a = ar::resolve(a, lam, fx.view, nullptr), got_u = ar::resolve(u, lam, fx.view, nullptr),
               got_i = ar::resolve(il, lam, fx.view, nullptr);
    for (int l = 0; l < 4; ++l) {
      CHECK(std::fabs(double(got_a.v[l]) - double(want_a.v[l])) <= 1e-4);
      CHECK(std::fabs(double(got_u.v[l]) - double(want_u.v[l])) <= 1e-4 * (1.0 + double(want_u.v[l])));
      CHECK(std::fabs(double(got_i.v[l]) - double(want_i.v[l])) <= 1e-4 * (1.0 + double(want_i.v[l])));
    }
    CHECK(got_u.v[0] != got_u.v[3]);  // not flat
    // IOR accepts no RGB source.
    ar::SpectrumRef none;
    CHECK(!ar::make_rgb_ref(fx.table, rgb, SpectrumSemantic::IOR, &none, &f2));
  }

  AR_RT_CASE("material_layout/illuminant white") {
    Fixture fx;
    ar::SpectrumRef r;
    uint32_t flags = 0;
    REQUIRE(ar::make_rgb_ref(fx.table, Rgb{{Real(1), Real(1), Real(1)}}, SpectrumSemantic::ILLUMINANT, &r, &flags));
    for (int i = 0; i < ar::kObsSamples; i += 4) {
      Lambda lam;
      for (int l = 0; l < 4; ++l) lam.nm[l] = Real(360 + (i + l < ar::kObsSamples ? i + l : ar::kObsSamples - 1));
      const Spec s = ar::resolve(r, lam, fx.view, nullptr);
      for (int l = 0; l < 4; ++l) {
        const double want = double(ar::illuminant_at(fx.view, lam.nm[l]));
        CHECK(std::fabs(double(s.v[l]) - want) <= 1e-4 * (1.0 + want));
      }
    }
    // The constant form of the same slot is grey times the illuminant.
    const Lambda lam = lanes(Real(450), Real(520), Real(590), Real(660));
    const Spec c = ar::resolve(ar::make_const_ref(SpectrumSemantic::ILLUMINANT, Real(2)), lam, fx.view, nullptr);
    for (int l = 0; l < 4; ++l)
      CHECK(std::fabs(double(c.v[l]) - 2.0 * double(ar::illuminant_at(fx.view, lam.nm[l]))) <= 1e-5);
  }

  AR_RT_CASE("material_layout/constant clamp by semantic") {
    Fixture fx;
    const Lambda lam = lanes(Real(450), Real(520), Real(590), Real(660));
    // REFLECTANCE clamps to [0, 1] (the compiler reports it, 059).
    CHECK(lane_error(ar::resolve(ar::make_const_ref(SpectrumSemantic::REFLECTANCE, Real(1.7)), lam, fx.view, nullptr), 1.0) == 0.0);
    CHECK(lane_error(ar::resolve(ar::make_const_ref(SpectrumSemantic::REFLECTANCE, Real(-0.3)), lam, fx.view, nullptr), 0.0) == 0.0);
    CHECK(lane_error(ar::resolve(ar::make_const_ref(SpectrumSemantic::REFLECTANCE, Real(0.4)), lam, fx.view, nullptr), 0.4) <= 1e-6);
    // The other semantics do not.
    CHECK(lane_error(ar::resolve(ar::make_const_ref(SpectrumSemantic::UNBOUNDED, Real(1.7)), lam, fx.view, nullptr), 1.7) <= 1e-6);
    CHECK(lane_error(ar::resolve(ar::make_const_ref(SpectrumSemantic::IOR, Real(1.7)), lam, fx.view, nullptr), 1.7) <= 1e-6);
    // An RGB reflectance above 1 is clamped and flagged for the compiler to report.
    ar::SpectrumRef r;
    uint32_t flags = 0;
    REQUIRE(ar::make_rgb_ref(fx.table, Rgb{{Real(1.7), Real(1.7), Real(1.7)}}, SpectrumSemantic::REFLECTANCE, &r, &flags));
    CHECK((flags & ar::kRgbAboveOneClamped) != 0u);
    CHECK(lane_error(ar::resolve(r, lam, fx.view, nullptr), 1.0) <= 1e-4);
  }
}  // TEST_SUITE

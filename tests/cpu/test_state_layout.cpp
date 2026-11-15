// SPDX-License-Identifier: Apache-2.0
// Layout of the path state and shadow item, the termination, Stage and ShadeKind
// enums, and the per-field allocation sizes. Single copy at Real = float: the byte
// budgets of the records are stated there. Every expected value below is typed from
// the record's tables, not computed from the code under test.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d19, d20, d22,
// Verification check 13; docs/decisions/0002-kernel-architecture.md D8 (Stage).
#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <stdexcept>

#include "host/state_alloc.h"
#include "kernel_globals.h"
#include "state/stage.h"
#include "state/state.h"
#include "state/termination.h"

static_assert(sizeof(ar::Real) == 4, "the layout budgets are stated at Real = float");

namespace {

using ar::StateFeature;
using ar::StateFeatures;
using astroray::host::FieldAlloc;
using astroray::host::StateLayout;

struct Row {
  const char* name;
  uint32_t bytes;
  StateFeature feature;
};

// 0003 d19, one row per field, table order (rows 1-31).
const Row kPath[] = {
    {"sample_id", 4, StateFeature::ALWAYS},
    {"next_stage", 1, StateFeature::ALWAYS},
    {"term", 1, StateFeature::ALWAYS},
    {"ray_type", 1, StateFeature::ALWAYS},
    {"flags", 4, StateFeature::ALWAYS},
    {"bounces", 12, StateFeature::ALWAYS},
    {"event_index", 2, StateFeature::ALWAYS},
    {"ray_o", 12, StateFeature::ALWAYS},
    {"ray_d", 12, StateFeature::ALWAYS},
    {"cone", 8, StateFeature::ALWAYS},
    {"lambda_t", 16, StateFeature::ALWAYS},
    {"beta", 16, StateFeature::ALWAYS},
    {"r_u", 16, StateFeature::ALWAYS},
    {"r_l", 16, StateFeature::ALWAYS},
    {"eta_scale", 4, StateFeature::ALWAYS},
    {"prev_ng", 4, StateFeature::ALWAYS},
    {"prev_ns", 4, StateFeature::ALWAYS},
    {"prev_dist", 4, StateFeature::ALWAYS},
    {"hit_t", 4, StateFeature::ALWAYS},
    {"hit_prim", 4, StateFeature::ALWAYS},
    {"hit_inst", 4, StateFeature::ALWAYS},
    {"hit_uv", 8, StateFeature::ALWAYS},
    {"ray_time", 4, StateFeature::MOTION},
    {"ray_tmax", 4, StateFeature::CURVED_OR_SUBSURFACE},
    {"medium_stack", 16, StateFeature::MEDIA},
    {"medium_rng", 4, StateFeature::MEDIA},
    {"min_ray_pdf", 4, StateFeature::FILTER_GLOSSY},
    {"guide_tint", 12, StateFeature::GUIDES_OR_LIGHTPASS},
    {"pass_frac", 16, StateFeature::LIGHTPASS},
    {"stats_time", 4, StateFeature::STAT_TIME},
    {"stats_rays", 4, StateFeature::STAT_RAYS},
};

// 0003 d20, one row per field, table order. r_l_film_pos is 16 B with media.
const Row kShadow[] = {
    {"next_stage", 1, StateFeature::ALWAYS},
    {"ray_o", 12, StateFeature::ALWAYS},
    {"ray_d", 12, StateFeature::ALWAYS},
    {"tmax", 4, StateFeature::ALWAYS},
    {"time", 4, StateFeature::MOTION},
    {"contribution", 16, StateFeature::ALWAYS},
    {"target", 4, StateFeature::ALWAYS},
    {"item_flags", 1, StateFeature::LIGHTPASS_OR_MEDIA},
    {"transparent_left", 1, StateFeature::NON_OPAQUE},
    {"light_group", 1, StateFeature::LIGHT_GROUPS},
    {"lambda_t", 16, StateFeature::MEDIA_OR_NON_OPAQUE},
    {"r_u", 16, StateFeature::MEDIA},
    {"r_l_film_pos", 16, StateFeature::LIGHTPASS_OR_MEDIA},
    {"medium_stack", 16, StateFeature::MEDIA},
    {"medium_rng", 4, StateFeature::MEDIA},
};

template <size_t N>
void check_table(const ar::StateField* got, uint32_t got_count, const Row (&want)[N]) {
  REQUIRE(got_count == N);
  for (size_t i = 0; i < N; ++i) {
    INFO("row " << i << " " << want[i].name);
    CHECK(std::strcmp(got[i].name, want[i].name) == 0);
    CHECK(got[i].bytes == want[i].bytes);
    CHECK(got[i].feature == want[i].feature);
  }
}

StateFeatures only(bool StateFeatures::*member) {
  StateFeatures f;
  f.*member = true;
  return f;
}

}  // namespace

TEST_SUITE("fast") {
  TEST_CASE("state/path state bytes") {
    check_table(ar::kPathStateFields, ar::kPathStateFieldCount, kPath);

    const uint32_t unconditional = ar::path_state_bytes(StateFeatures{});
    const uint32_t all = ar::path_state_bytes(ar::all_features_on());
    std::printf("path state bytes: unconditional %u, all features %u\n", unconditional, all);
    CHECK(unconditional == 157);
    CHECK(all == 225);
    CHECK(all <= ar::kPathStateBudgetBytes);
    CHECK(ar::kPathStateBudgetBytes == 256);

    // Each feature on its own adds exactly the rows of its condition.
    CHECK(ar::path_state_bytes(only(&StateFeatures::motion)) == 157 + 4);
    CHECK(ar::path_state_bytes(only(&StateFeatures::curved)) == 157 + 4);
    CHECK(ar::path_state_bytes(only(&StateFeatures::subsurface)) == 157 + 4);
    CHECK(ar::path_state_bytes(only(&StateFeatures::media)) == 157 + 20);
    CHECK(ar::path_state_bytes(only(&StateFeatures::filter_glossy)) == 157 + 4);
    CHECK(ar::path_state_bytes(only(&StateFeatures::guides)) == 157 + 12);
    CHECK(ar::path_state_bytes(only(&StateFeatures::lightpass)) == 157 + 12 + 16);
    CHECK(ar::path_state_bytes(only(&StateFeatures::stat_time)) == 157 + 4);
    CHECK(ar::path_state_bytes(only(&StateFeatures::stat_rays)) == 157 + 4);
    // Shadow-item-only facts do not touch the path.
    CHECK(ar::path_state_bytes(only(&StateFeatures::non_opaque)) == 157);
    CHECK(ar::path_state_bytes(only(&StateFeatures::light_groups)) == 157);
    // Curved and subsurface share one array.
    StateFeatures both;
    both.curved = both.subsurface = true;
    CHECK(ar::path_state_bytes(both) == 157 + 4);

    CHECK(sizeof(ar::PathStateSoA) == sizeof(void*) * 31);
  }

  TEST_CASE("state/shadow item bytes") {
    check_table(ar::kShadowItemFields, ar::kShadowItemFieldCount, kShadow);

    const uint32_t unconditional = ar::shadow_item_bytes(StateFeatures{});
    const uint32_t all = ar::shadow_item_bytes(ar::all_features_on());
    std::printf("shadow item bytes: unconditional %u, all features %u\n", unconditional, all);
    CHECK(unconditional == 49);
    CHECK(all == 124);
    CHECK(all <= ar::kShadowItemBudgetBytes);
    CHECK(ar::kShadowItemBudgetBytes == 128);

    CHECK(ar::shadow_item_bytes(only(&StateFeatures::motion)) == 49 + 4);
    CHECK(ar::shadow_item_bytes(only(&StateFeatures::non_opaque)) == 49 + 1 + 16);
    CHECK(ar::shadow_item_bytes(only(&StateFeatures::light_groups)) == 49 + 1);
    // Light pass without media: item_flags and the 8-B film_pos only.
    CHECK(ar::shadow_item_bytes(only(&StateFeatures::lightpass)) == 49 + 1 + 8);
    // Media: item_flags, lambda_t, r_u, 16-B r_l, medium_stack, medium_rng.
    CHECK(ar::shadow_item_bytes(only(&StateFeatures::media)) == 49 + 1 + 16 + 16 + 16 + 20);
    // Light pass with media shares the 16-B r_l storage; it adds nothing more.
    StateFeatures lp_media;
    lp_media.lightpass = lp_media.media = true;
    CHECK(ar::shadow_item_bytes(lp_media) == 49 + 1 + 16 + 16 + 16 + 20);
    // Non-opaque and media both need lambda_t once.
    StateFeatures no_media;
    no_media.non_opaque = no_media.media = true;
    CHECK(ar::shadow_item_bytes(no_media) == 49 + 1 + 1 + 16 + 16 + 16 + 20);

    CHECK(sizeof(ar::ShadowItemSoA) == sizeof(void*) * 15);
  }

  TEST_CASE("state/feature tags") {
    // Every tag against every single fact: the condition columns of d19 and d20.
    struct Case {
      StateFeature tag;
      bool StateFeatures::*on[3];  // facts that switch the tag on (nullptr terminated)
    };
    const Case cases[] = {
        {StateFeature::MOTION, {&StateFeatures::motion, nullptr, nullptr}},
        {StateFeature::CURVED_OR_SUBSURFACE, {&StateFeatures::curved, &StateFeatures::subsurface, nullptr}},
        {StateFeature::MEDIA, {&StateFeatures::media, nullptr, nullptr}},
        {StateFeature::FILTER_GLOSSY, {&StateFeatures::filter_glossy, nullptr, nullptr}},
        {StateFeature::GUIDES_OR_LIGHTPASS, {&StateFeatures::guides, &StateFeatures::lightpass, nullptr}},
        {StateFeature::LIGHTPASS, {&StateFeatures::lightpass, nullptr, nullptr}},
        {StateFeature::STAT_TIME, {&StateFeatures::stat_time, nullptr, nullptr}},
        {StateFeature::STAT_RAYS, {&StateFeatures::stat_rays, nullptr, nullptr}},
        {StateFeature::LIGHTPASS_OR_MEDIA, {&StateFeatures::lightpass, &StateFeatures::media, nullptr}},
        {StateFeature::NON_OPAQUE, {&StateFeatures::non_opaque, nullptr, nullptr}},
        {StateFeature::LIGHT_GROUPS, {&StateFeatures::light_groups, nullptr, nullptr}},
        {StateFeature::MEDIA_OR_NON_OPAQUE, {&StateFeatures::media, &StateFeatures::non_opaque, nullptr}},
    };
    bool StateFeatures::* const all[] = {
        &StateFeatures::motion,     &StateFeatures::curved,     &StateFeatures::subsurface,
        &StateFeatures::media,      &StateFeatures::filter_glossy, &StateFeatures::guides,
        &StateFeatures::lightpass,  &StateFeatures::stat_time,  &StateFeatures::stat_rays,
        &StateFeatures::non_opaque, &StateFeatures::light_groups};
    CHECK(ar::feature_on(StateFeatures{}, StateFeature::ALWAYS));
    for (const Case& c : cases) {
      CHECK_FALSE(ar::feature_on(StateFeatures{}, c.tag));
      for (auto fact : all) {
        bool expect = false;
        for (auto o : c.on) expect = expect || (o == fact);
        CHECK(ar::feature_on(only(fact), c.tag) == expect);
      }
    }
  }

  TEST_CASE("state/termination values") {
    using ar::Term;
    static_assert(sizeof(Term) == 1, "u8");
    CHECK(uint32_t(Term::ACTIVE) == 0);
    CHECK(uint32_t(Term::ESCAPED) == 1);
    CHECK(uint32_t(Term::RUSSIAN_ROULETTE) == 2);
    CHECK(uint32_t(Term::ZERO_THROUGHPUT) == 3);
    CHECK(uint32_t(Term::MAX_BOUNCE) == 4);
    CHECK(uint32_t(Term::TRANSPARENT_LIMIT) == 5);
    CHECK(uint32_t(Term::ABSORBED) == 6);
    CHECK(uint32_t(Term::VOLUME_LIMIT) == 7);
    CHECK(uint32_t(Term::STACK_OVERFLOW) == 8);
    CHECK(uint32_t(Term::INVALID) == 9);
    CHECK(uint32_t(Term::CAPTURED) == 10);
    CHECK(uint32_t(Term::GEODESIC_STEP_LIMIT) == 11);
    CHECK(uint32_t(Term::GEODESIC_NUMERICAL) == 12);
    CHECK(uint32_t(Term::LIGHT_PATH_IN_REGION) == 13);
    CHECK(uint32_t(Term::LIGHT_PATH_END) == 14);
    CHECK(uint32_t(Term::CANCELLED) == 15);
  }

  TEST_CASE("state/stage values") {
    using ar::Stage;
    static_assert(sizeof(Stage) == 1 && sizeof(ar::ShadeKind) == 1, "u8");
    // Launch order of 0002 D8, hair stages omitted.
    const Stage order[] = {
        Stage::INTERSECT_CLOSEST, Stage::INTERSECT_SUBSURFACE, Stage::INTERSECT_SHADOW,
        Stage::SHADE_BACKGROUND,  Stage::SHADE_LIGHT,          Stage::SHADE_SURFACE,
        Stage::SHADE_SURFACE_EXT, Stage::SHADE_LIGHT_PATH,     Stage::SHADE_LIGHT_PATH_EXT,
        Stage::SHADE_SHADOW,      Stage::SHADE_VOLUME,         Stage::SHADE_SUBSURFACE,
        Stage::ADVANCE_GEODESIC,  Stage::TERMINATED,           Stage::SHADOW_TERMINATED};
    for (uint32_t i = 0; i < 15; ++i) CHECK(uint32_t(order[i]) == i);
    for (uint32_t i = 1; i < 15; ++i) CHECK(uint32_t(order[i]) > uint32_t(order[i - 1]));
    CHECK(ar::kStageCount == 15);

    CHECK(uint32_t(ar::ShadeKind::Common) == 0);
    CHECK(uint32_t(ar::ShadeKind::Ext) == 1);
    CHECK(uint32_t(ar::ShadeKind::Light) == 2);
    CHECK(uint32_t(ar::ShadeKind::LightExt) == 3);
  }

  TEST_CASE("state/allocation layout") {
    constexpr uint64_t cap = 1000;
    // Feature off: a field gets no allocation, no bytes and no offset.
    const StateLayout off = astroray::host::path_state_layout(StateFeatures{}, cap);
    REQUIRE(off.fields.size() == 31);
    uint32_t allocated = 0;
    for (size_t i = 0; i < 31; ++i) {
      const FieldAlloc& a = off.fields[i];
      INFO(a.name);
      CHECK(std::strcmp(a.name, kPath[i].name) == 0);
      const bool want = kPath[i].feature == StateFeature::ALWAYS;
      CHECK(a.allocated == want);
      if (want) {
        ++allocated;
        CHECK(a.element_bytes == kPath[i].bytes);
        CHECK(a.bytes == uint64_t(kPath[i].bytes) * cap);
        CHECK(a.offset % 256 == 0);
      } else {
        CHECK(a.element_bytes == 0);
        CHECK(a.bytes == 0);
        CHECK(a.offset == 0);
      }
    }
    CHECK(allocated == 22);
    CHECK(off.bytes_per_slot == 157);

    // All on: 256-B aligned, ordered, non-overlapping arrays; total covers the last.
    const StateLayout on = astroray::host::path_state_layout(ar::all_features_on(), cap);
    CHECK(on.bytes_per_slot == 225);
    uint64_t end = 0;
    for (const FieldAlloc& a : on.fields) {
      INFO(a.name);
      CHECK(a.allocated);
      CHECK(a.offset % 256 == 0);
      CHECK(a.offset >= end);
      end = a.offset + a.bytes;
    }
    CHECK(on.total_bytes % 256 == 0);
    CHECK(on.total_bytes >= end);
    CHECK(on.total_bytes < end + 256);
    // Exact: sample_id 4000 B at 0, next_stage starts at the next 256 multiple (4096).
    CHECK(on.fields[0].offset == 0);
    CHECK(on.fields[1].offset == 4096);
    CHECK(on.fields[2].offset == 4096 + 1024);

    // The capacity scales every array; zero capacity is refused.
    const StateLayout one = astroray::host::path_state_layout(ar::all_features_on(), 1);
    CHECK(one.total_bytes == 31 * 256);
    CHECK_THROWS_AS(astroray::host::path_state_layout(StateFeatures{}, 0), std::invalid_argument);
    CHECK_THROWS_AS(astroray::host::shadow_item_layout(StateFeatures{}, 0), std::invalid_argument);

    // Shadow item: r_l_film_pos is 16 B with media, 8 B with the light pass alone.
    const StateLayout sh_all = astroray::host::shadow_item_layout(ar::all_features_on(), cap);
    CHECK(sh_all.bytes_per_slot == 124);
    CHECK(sh_all.fields[12].element_bytes == 16);
    const StateLayout sh_lp = astroray::host::shadow_item_layout(only(&StateFeatures::lightpass), cap);
    CHECK(sh_lp.bytes_per_slot == 58);
    CHECK(sh_lp.fields[12].allocated);
    CHECK(sh_lp.fields[12].element_bytes == 8);
    CHECK_FALSE(sh_lp.fields[11].allocated);  // r_u needs media
    const StateLayout sh_off = astroray::host::shadow_item_layout(StateFeatures{}, cap);
    CHECK(sh_off.bytes_per_slot == 49);
    CHECK_FALSE(sh_off.fields[12].allocated);
  }

  TEST_CASE("state/accessors read the field arrays") {
    // Backing arrays for a media-on session; ray_time stays null (motion off).
    ar::Spec beta[2] = {};
    ar::Spec rl[2] = {};
    ar::Real2 film[2] = {};
    ar::PathStateSoA path = {};
    ar::ShadowItemSoA shadow = {};
    path.beta = beta;
    shadow.r_l_film_pos = rl;
    ar::KernelGlobals kg = {};
    kg.path = &path;
    kg.shadow = &shadow;
    kg.session.features.media = true;
    ar::ps_beta(kg, 1).v[2] = ar::Real(3);
    CHECK(beta[1].v[2] == ar::Real(3));
    CHECK(path.ray_time == nullptr);
    ar::sh_r_l(kg, 1).v[0] = ar::Real(5);
    CHECK(rl[1].v[0] == ar::Real(5));
    // With media a connection's film_pos overlays the first 8 B of its 16-B slot.
    ar::sh_film_pos(kg, 1).y = ar::Real(7);
    CHECK(rl[1].v[1] == ar::Real(7));
    // Without media the array is dense Real2.
    shadow.r_l_film_pos = reinterpret_cast<ar::Spec*>(film);
    kg.session.features.media = false;
    ar::sh_film_pos(kg, 1).x = ar::Real(9);
    CHECK(film[1].x == ar::Real(9));

    CHECK(sizeof(ar::StepTable) <= 256);
  }
}

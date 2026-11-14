# 025 Path state, shadow item, enums and KernelGlobals
Goal: no stage can be written because the shared state does not exist -> the complete path-state and shadow-item tables of 0003 are X-macros with feature tags, the termination, Stage and ShadeKind enums exist, `KernelGlobals` is defined, and the byte budgets are static_asserts.
Read first: 0003 d19 (path state table), d20 (shadow item and side buffers), d22 (termination enum); 0002 D5 (pools), D8 (Stage enum, ShadeKind), D13 (X-macro storage mechanism); 0003 Verification check 13.
Scope: src/kernel/state/path_state.def, src/kernel/state/shadow_item.def, src/kernel/state/state.h, src/kernel/state/termination.h, src/kernel/state/stage.h, src/kernel/kernel_globals.h, src/host/state_alloc.{h,cpp}, tests/cpu/test_state_layout.cpp.
Out of scope: allocation through the ledger (026 provides it; this task computes sizes only); side buffers' contents; any stage.
Reference: Cycles `kernel/integrator/state_template.h` (Apache-2.0) as the X-macro pattern.
Do:
1. path_state.def: one `AR_PATH_FIELD(name, type, feature)` per row of 0003 d19, in table order, with the types and widths given there (`Real3` = 3 x Real, `Spec`, `Real2`, `uint16_t bounces[6]`, `unorm16 pass_frac[8]` as uint16_t[8]). Features: `ALWAYS`, `MOTION`, `CURVED_OR_SUBSURFACE`, `MEDIA`, `FILTER_GLOSSY`, `GUIDES_OR_LIGHTPASS`, `LIGHTPASS`, `STAT_TIME`, `STAT_RAYS`.
2. shadow_item.def likewise from 0003 d20, one feature per distinct condition in its condition column (`ALWAYS`, `MOTION`, `LIGHTPASS_OR_MEDIA`, `NON_OPAQUE`, `LIGHT_GROUPS`, `MEDIA_OR_NON_OPAQUE`, `MEDIA`); `r_l`/`film_pos` is one shared field of 16 B when media are on and 8 B (film_pos only) with the light pass and no media.
3. state.h: from the X-macros, `PathStateSoA` and `ShadowItemSoA` (one pointer per field; a disabled feature's pointer is null) and AR_FN accessors `ps_<field>(kg, slot)`; `constexpr` byte counts per feature set.
4. termination.h: `enum class Term : uint8_t` exactly 0003 d22 in order. stage.h: `enum class Stage : uint8_t` exactly 0002 D8's list in launch order (omit `SHADE_HAIR` and `SHADE_LIGHT_PATH_HAIR`, which exist only if split), and `enum class ShadeKind : uint8_t { Common, Ext, Light, LightExt }`.
5. kernel_globals.h: `struct KernelGlobals` holding pointers to both SoA structs, the step table, queue index arrays, a `SessionUniforms` struct (resolution, spp state, seed, sampler mode, max bounces, feature flags), scene data pointers (filled by later tasks; declare each as an opaque struct pointer added by the task that needs it), film pointers, wavelength table and rgb2spec table views. Passed by const reference only.
6. state_alloc: given a feature set and capacities, returns per-field byte sizes, 256-B aligned offsets and totals (bytes per path and per item, used by the work counters).
7. test_state_layout.cpp (fast): static_asserts and runtime checks at Real = float: unconditional path fields 157 B, all fields 225 B, shadow item all fields 124 B (check 13); Stage values strictly increasing in launch order; a feature-off field gets no allocation.
Acceptance: `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=fast -tc="state/*"` prints the three byte totals 157, 225 and 124.
Look at: none.
Canary: tasks/025.canary.patch must turn red: astroray_tests_cpu "state/path state bytes", "state/shadow item bytes".
Budget: none.
Depends on: 019, 021.
Size: M (about 350 lines); stop and report at 2x.
Progress:

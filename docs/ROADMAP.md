# Astroray 2.0 roadmap

Milestones are decided only here (G1). This page says **when**; what and how belong to the records. Notation follows `ARCHITECTURE.md`: `O7` owner decision, `R45` ruling, `0002 D8` / `0003 d19` record decisions, `B-14` a 0009 budget row, `FI 123` line 123 of `docs/v1-lessons/feature-inventory.md` as of 2026-11-14. Precedence (R50): owner decisions > rulings > spine > records > this page.

## Rules that apply to every milestone

1. **Visible result.** A milestone ends with something the owner sees in Blender 5.3 (viewport and F12), a showcase gallery and a benchmark report with graphs (0009 d22-23, O15), and a green `tools/check.ps1 full`.
2. **Standing gates at every close.** B-14: 30 updates/s at full 1080p on B-1M, and the 24/s floor (at full resolution until the controller lands in M2, with it afterwards) on every bench scene then in the set: B-1M, B-5M and B-TX from M1a, B-CR from M3, the window interior from M5, the VDB scene from M6, B-NB in M10; the full-resolution rate and the controller's settled resolution are reported for each. V7a first frame at most 1.5x Cycles (0007 d35, O23) on S1, S2, S3 and every bench scene in the set. B-16 counters and tripwire; B-17; kernel resources (0002 D10). No bench regression beyond 0009 d18's A/B rule against the previous milestone tag, unless the change that causes it re-blesses the baseline with an A/B report the reviewer accepts (a new feature may legitimately cost time). A milestone that breaks a gate does not close.
3. **Order.** A vertical slice through the real architecture first, then one stage family per milestone, each built on landed, gated code. Every unproven number is measured in the milestone that first depends on it, with a pass line and a fallback (see Measurements). Each PROVISIONAL budget is reset from measurement in the milestone that first measures it, not at release.
4. **Scenes.** Exit criteria use only scene-builder scenes (`scenes/`, 0009) and manifest assets. Scenes the owner models (research 16 "Gaps") are gallery additions when they exist; agents build a simplified scene-builder version of each gap scene a milestone shows, so no milestone waits on the owner.
5. **Features land on.** Every user-facing feature lands either on by default (Blender's default where a Cycles setting matches) or with a written default, its reason and the condition or owner decision for turning it on (example: DLSS stays off until spike F part B passes, 0008 V12). Either way a test shows that toggling it changes the image or the counters (0006 V5 style). A feature that cannot pass that test has not landed. v1 ended with landed features switched off.
6. **Size** counts task files (0009 d28): S under 10, M 10-25, L 25-50. No calendar dates. The figures in the overview and in each milestone's "Size and risk" are the pre-build estimates; M1a needed 135 task files against 25 (5.4x: 4.4x in decomposition, then 22% added by measurements and gaps). Until a milestone has its own plan, read its size as the estimate times 4.4 to 5.4, built in slices of 15-30 tasks with a review between slices, a fifth of each slice held for follow-ups, and at most two remedy tasks per budget row before the owner decides (`docs/retro/M1a-retrospective.md` R8, R13). Each milestone gets a planning session that writes its plan and only its first batch of task files (M1b: `docs/PLAN-M1b.md`).

## Overview

| M | Name | The owner sees | Size | Main risk |
|---|---|---|---|---|
| — | Before M1a | spike F part A and spike G reports; owner answers | S (~4) | DLSS unavailable; grid route fails |
| M1a | First pixels | textured, lamp-lit meshes in the viewport on CUDA and CPU; heat maps | 135 task files landed (25 estimated, 111 at sign-off); built, close waits on the owner (B-19 idle, B-20, B-31) | wavefront overhead; first frame with materials |
| M1b | First light | HDRI world, metal, alpha foliage, a grating card, a test medium, final OIDN | 110-135 planned in five slices (~22 estimated; `docs/PLAN-M1b.md`) | register growth as the must-exist set fills in |
| M2 | Lobes, shadows and media | every OpenPBR lobe, transparent shadows, homogeneous media, the viewport controller and denoiser | M (~25) | shade-kernel spills at full lobe scope |
| M3 | Nodes and textures | procedural and colour nodes, filtered textures, Classroom's materials with coverage | M (~24) | node lowering and corpus translation time |
| M4 | Lights, sky, spectra and cameras | all lamp types, sky and sun, spectral lamps and metals, light tree, every camera model | L (~32) | light-tree quality and build time |
| M5 | Light tracing and caustics | caustics, dispersion and grating spectra from point lamps and spotlights; Classroom sunlit | M (~22) | light-pass share versus viewport rate |
| M6 | Volumes | VDB smoke, fire and clouds, fog with beams; VRAM spill | L (~33) | grid ingestion against the first-frame gate |
| M7 | Advanced materials and astro numerics | fluorescence (surface and volumetric), subsurface, hair; geodesic precision settled | L (~34) | extension-kernel registers; S1 outcome |
| M8 | Outputs, science, DLSS and motion | all passes, motion blur, bands, photon counts, cubes, DLSS | L (~35) | DLSS image quality |
| M9 | First release 2.0.0 | the full breadth installed from the release channel | S (~10) | hardening finds a late defect |
| M10 | Black hole in an emission nebula | O8's target in the viewport and as science output | L (~30) | geodesic stage cost and precision |

"First release" (M9) holds the breadth of O7, O16, O17, O24, O25; M1a-M8 build it and M9 only hardens it. M10 follows it (O8, spine).

**The first milestone.** M1a and M1b are the two halves of M1 under this page's split rule. M1a is the small working slice of O27 and the first thing the owner sees. The records' "must exist from the first milestone" items are cheap layouts, interfaces and gates (path state, ABI shapes, record layout, sampler, film write path), which land in M1a, or implementations (VM compiler, tracking, extension kinds, guides, OIDN), which land in M1b. M1 closes before any breadth milestone starts. That is what the records' reason ("retrofitting is expensive") needs: no breadth code is written before them.

## Before M1a

Spike F part A (0008 V12; CHECKPOINT-1 requires it before sign-off) and spike G (0006 Risk 5, Python only, inside Blender 5.3) run before planning sign-off, or as the first two tasks if sign-off does not wait. G runs here because a negative result reopens 0001 D9 (native OpenVDB, TBB collision), and that must be settled before M1b certifies the DLL set (0001 V5). The owner questions answered before M1a are listed under Open items.

## M1a — First pixels (O27)

**Goal.** Textured, lamp-lit meshes render progressively in the Blender 5.3 viewport and F12 on CUDA and CPU through the final C ABI, sync, wavefront kernels, film and presentation, with every gate live from the first task, and the unproven architecture numbers are measured.

**Owner sees.** Bitterli Cornell Box (CC0) under an area lamp and a point lamp; the textured S3 scene (B-TX) under a constant world; S2 with 300 materials; sample-count and bounce-count heat maps; CPU and GPU images side by side; the viewport going from single samples to growing batches (O22). The report shows B-19, B-14, V7a, the M-1 probe and OptiX cold compile against their lines.

**Scope.**
- Process (0009 must-exist list): repository layout, `dev`/`cpu` presets, `build`/`check fast|gate|full|ci`/`land`/`status`/`fetch`, locks, lint, build identity with the `ninja -n` check, `budgets.toml` with family sizes, `deps.toml` with our Embree release asset, hosted CI, the `astroray/gate` status and branch protection, CLAUDE.md, three agent files, the `land` skill with the canary, STATUS, bench history format and report skeleton, the `spikes-final` tag; the CPU and GPU doctest executables with every lobe, light and sampler function pure and taking explicit uniforms (0009 d14); scene builders, ABI recording and replay, exporter equivalence on deterministic AOV dumps (R59); the API-surface check against `bpy` 5.3.x.
- Product shape (0001): `astroray.h` in the D2.10 grammar with its generator (`_abi.py`, layout table, hash, recorder, replayer), handles and `ar_status`, the report (D2.11), fault handling and log (D2.5), `/MT` and static cudart, embedded kernels with `ar_build_info`, SPDX, `THIRD_PARTY.md`, `MANIFEST.toml`, `CpuDevice` and `CudaDevice`, CLI `info|render|bench|dump`, version pins (R63).
- Kernels (0002): dialect and lint (D1-D4), `KernelGlobals`, the state X-macro carrying the complete 0003 d19-d20 tables (medium state, λt, region flags and hero bits included), `queue_build` with step table, shadow-slot reservation and sort, both pools, `path_kind`, light-path routing with the stub `generate_light` test, `generate_camera`, `intersect_closest`, opaque `intersect_shadow`, `route_hit`, `k_route_hit` on OptiX and Embree, asynchronous device init with the D15 injection tests, the `budget.toml` gate and the agreement build.
- Capped `shade_surface` with the real VM: the 304 B record and 32-register file (0005 items 2-3), `ops.def` generating enum, interpreter and the interval-rule column from the first opcode (0006 D7), a minimal compiler (constants, image texture, Principled base colour, roughness and emission), Jakob-Hanika upsampling (Rec.709), 8-bit textures (Closest and Linear, Repeat and Extend), EON diffuse and GGX dielectric specular, NEE over an alias table, emission MIS with the previous vertex's context, magenta plus report for every unlowered node.
- Spectral core (0003): `kLanes = 4`, stored λt, packet operations incl. `apply_frequency_shift`, `film_weights`, random hero lane, the sampler with its full dimension map, event index and streams, unit anchor, K and radiance scale, shutter-relative time, `Real` float and double builds, scene-origin rule, `WavelengthTable` (visible component, `lines` hook), `specgen` observer tables.
- Lights and camera (0004): light interface with `sample_le` and the segment context; point lamps (radius 0 and above), rectangle and disk area lamps as primitives with `shade_light`; emissive triangles; constant world through `shade_background`; perspective camera with `We`, `PDF_We`, `sample_wi`; light-group ids, epochs, statistics counters.
- Film (0008): `FilmLayout`, the `film_*` write path with response conversion, per-sample buffer, `sample_add` and resolve into fp64 moments with S1A, the `SPLAT` plane fold (fed by the stub), filter importance sampling with Blender's width map, invalid counting, sample-count, bounce and invalid channels, the denoiser interface (no backend yet), `FilmView`/`DisplayImage`, FITS dumps, ledger reporting.
- Blender (0007): identity check at register, `RenderEngine`, the ABI shape of d4-d9, keys, the full classification table, the topology cache, native triangulation with its golden test, mesh, lamp, image and material sync, pass-boundary generations, the VRAM ledger and reserve rule (fail only), threading, striped RGBA16F present with the view hash, progressive batching (d50, O22), `motion_steps`, visibility bits and `light_group` in descriptors, `ar_edit_set_astro` (validated and stored, no effect until M10), the V7a/V7b harness, display modes Beauty, sample count and bounce count.

**Out of scope.** Everything in M1b; other lobes; half and float textures; HDRI; transparent shadows; media; the resolution controller; OIDN.

**Exit criteria.** `check gate` and `check ci` green for every landed task; `land prepare` refuses an inert canary (0009 verification rows 24-26); 0009 lint cases, the `ninja -n` check, the detection-power task. 0001 V1-V4, V6, V11, V14, V15. 0002 checks for D1-D8, D11 (opaque traversal, mask), D12, D13, D15. 0003 checks 1-3, 5, 6, 8, 10, 11, 13, 14, 15 (without its blackbody-light half, which is M1b), 24, 25. 0004 checks D1 (point and area), D2, D4/D5 lifetime, D6 (point, sphere, area), D7, D8, D25, D28. 0005 V6 and V10 for the implemented ops. 0007 V1-V5, V8 (1080p, Vulkan), V9, V10, V12, V15, V16, V18-V20, V22, V23. 0008 V1-V4, V8. Analytic A1 (Lambertian), A3, A4 (point, sphere), A5; D1, D2; agreement 17(i)-(iv). B-14 on B-1M; V7a at most 1.5x Cycles on S1, S2 (300 materials), S3, first and second switch; B-19 and B-17 within limits. **Look:** the lead inspects viewport screenshots of the showcase scenes and both heat maps, CPU and GPU.

**Measurements inside.** B-19 and B-17 on S1 and S2 (the alpha-card run is M1b) with an Nsight timeline (0002 Risk 2); state bandwidth (Risk 3); capped `shade_surface` spills and stack (Risk 1); the M-1 probe; upsampling cost (0003 Risk 2); B-20 against Cycles; B-18 warm and cold with OptiX cold compile, including the motion pipeline variant compiled once from these kernels (R47, Risk 4); B-13, B-21, B-22, B-28, B-29, B-31; V7a/V7b with S2's 300 materials (first M-8 data); probes P2, P3 (object mode), P4, P5; static oneTBB, `/MT` link and the DLL inside an installed extension (0001 Risks 1-3); hosted `ci` duration and toolset; Blender start-up per process; nvcc pool RAM. Spike C is decided at close (see Measurements).

**Depends on.** Before M1a. **Size and risk.** 111 task files once written out and checked for implementability, against the 25 estimated here; the other milestones' sizes are estimates of the same kind and should be read as low by a similar factor. First CLI image at task 069, first viewport image at task 085. Main risk: B-19 (estimated 20-40% against 10%) and first frame with materials; either can change the design, which is why both are settled before M1b builds on the wavefront.

## M1b — First light

**Goal.** The rest of every record's must-exist list lands on M1a's slice, so no breadth milestone retrofits a layout, a kernel flow or a gate.

**Owner sees.** The textured S3 scene and a shader-ball row under an HDRI world; the Cornell box with a metal sphere; alpha-card foliage; a flat reflective grating card under an area lamp showing first-order spectra; a homogeneous test slab; OIDN on and off in F12; the Diagnostics panel listing magenta nodes.

**Scope.**
- Materials (0005 must-exist list): `MaterialHeader`, constant block, `SpectrumRef` kinds CONST, RGB, TABLE, BLACKBODY, CAUCHY_ABBE with per-slot semantics; the full compiler (folding, register allocation, three-segment surface layout, entry table), Python schema and coverage matrix; `MAY_BE_DELTA`, `NON_OPAQUE`, emission estimate; `TransportMode` in every lobe; the F82 metal lobe (spike A's GGX metal as the starting code); the extension mechanism with GRATING (reflective, sinusoidal scalar table) in `shade_surface_ext` and `shade_light_path_ext`. Nodes: Principled (adds metallic and alpha), Emission, Blackbody, Image Texture, Environment Texture, Background, Mix Shader, outputs.
- Surface front-loading completed (0002 D10): half and float textures (tinyexr reading); light-tree traversal in the kernel with a minimal host builder (alias table as oracle); environment importance sampling (`bake_environment`, 0004 D11) with HDRI world; first-hit guides, guide tint and the denoising-surface bit, (the DLSS specular input moved to M8 as specular motion vectors, owner decision 45); alpha pass-through with the per-launch requeue.
- Media core (0006 must-exist list): hero-lane delta and ratio tracking with `r_u`/`r_l` and the per-launch cap with requeue (D8), with a minimal `shade_volume` and `shade_shadow` over a constant test medium; the coefficient-level `MediumSample` with j (D2); false-interface and nested-dielectric stack updates with `interior_ior` and `medium_priority` (D16-D17), tested by a CPU stack fixture until M2 adds the dielectric lobe; medium code in the double CPU build (D39).
- Denoising and diagnostics: our OIDN release asset loaded under V5 with asynchronous creation, final render only (0008 d16); the Diagnostics panel (FI 215); heat maps in the bench report.

**Out of scope.** Other lobes, procedurals, bump, mips, transparent shadows, the full light-tree builder, spot, sun, IES, sky, Blender volume mapping, the real light pass, viewport OIDN, subsurface, hair, passes beyond Combined, Depth, Normal, Alpha and denoising, DLSS, the resolution controller, science outputs, Rec.2020 and AP1.

**Exit criteria.** 0001 V5. 0003 check 26 and the blackbody-light half of check 15. Analytic A9. B-19 on the alpha-card scene. 0004 D3 (alpha cards). 0005 V7, V6 and V10 for every implemented op, V8's order angles under an area lamp. 0006 V1, V6 (cap 1 vs 4,096 bit-identical), and the stack fixture. 0008 V9, V10, V11 (final). M-1 ratified with traversal, guides and GRATING in the capped build. B-14 and V7a re-gated with every M1b flag on. **Look:** grating orders, OIDN before and after, metal edge colour.

**Measurements inside.** OIDN on sm_120 (0008 Risk 3); guide register cost (Risk 2); B-14 and V7a with environment sampling and tree traversal on; the hero-lane variance over the test slab.

**Depends on.** M1a, spike C settled if it ran (it did not run: B-14 passes on B-1M). **Size and risk.** Estimated M, about 22; planned at 110-135 task files in five slices: tooling and debts, materials, world with lights, guides and denoise, media core, close (`docs/PLAN-M1b.md`). Main risk: tracking, traversal and guides together push `shade_surface` over its allowances; the 0002 D10 ladder applies before M2 starts. The plan orders every edit of `shade_surface` in one queue and gates each on the kernel's budget row, so the risk shows at the edit that causes it, not at the close. Added to M1b from M1a's retrospective: the canary and landing tools, a performance regression check (`check perf`), an F12 gate on both devices, and gate-time headroom under B-09.

## M2 — Lobes, shadows and media

**Goal.** Every OpenPBR lobe renders correctly through the front-loaded shade kernel with transparent shadows and homogeneous media, the viewport keeps its rate with the controller and the denoiser, and the record layout is ratified at full lobe scope.

**Owner sees.** ASWF Standard Shader Ball OpenPBR sweep; a thin-film set (scene builder: bubble, oil film, anodised titanium); Bitterli Glass of Water and Material Test Ball; foliage with coloured alpha shadows; coloured glass, a fog box and world fog; the viewport holding 30/s on B-1M with OIDN on, dropping resolution only below 24/s.

**Scope.** 0005 items 4-8 except the random walk (EON with roughness, F82 with compensation, dielectric with Turquin compensation, coat, fuzz, Airy thin film per O28, thin wall, translucent, opacity, anisotropy, dispersion via `CAUCHY_ABBE`), E tables from the host tool, item 38 Add Shader. Transparent shadows (0002 D11 `ah_record`, `shade_shadow`, K set on foliage). Per-type bounce limits (0004 D3). Homogeneous media: D10 distance-only volume NEE, D14, D16-D19 with shadow rays, D21 world volume and homogeneous Absorption, Scatter (HG) and Principled Volume constants, 0005 item 4 `transmission_depth` lowering. Resolution controller (0007 d34, d49). Viewport OIDN with quality stepping (0008 d18). A subsurface material renders as diffuse with `NODE_APPROXIMATED` until M7.

**Out of scope.** Procedurals and the texture system (M3), random walk, `SpectrumRef` kinds beyond M1b's five, grids, equiangular sampling, light pass, hair, passes.

**Exit criteria.** 0005 V1-V5, V9, V11. 0002 D11 transparent-shadow test (n = 1, K, 3K+1). 0003 check 9. 0004 D3 transparent limits. 0006 V2-V5, V7 (HG), V8 (NEE on/off), V9 (homogeneous). 0007 V11, V13 soak, V14 fail path. 0008 V11 (viewport); B-26, B-33. 0009 17(v) null calibration; O1-O3 with cross-oracle agreement (single witness recorded where an oracle is missing). A1 (all lossless configurations), A2, A6, A7, A10 (path-traced prism); D4 edit round trip. **Look:** shader-ball sweep, thin-film colours against the Airy oracle plots.

**Measurements inside.** M-1 ratified on the real lobes with every film flag on; B-26 OIDN share at 1080p and 4K; controller response (B-33).

**Depends on.** M1b. **Size and risk.** M, about 25. Main risk: shade-kernel spills and stack at full scope (0002 Risk 1); the D10 ladder can add tasks.

## M3 — Nodes and textures

**Goal.** Blender's surface node set and the full texture system lower into the VM with at least 95% coverage over the corpus, and translation time holds the first-frame gate on large material sets.

**Owner sees.** A procedural sweep (Noise, Voronoi, Wave, Brick and colour chains); mip-filtered and UDIM textures with Cubic and Smart interpolation; bump and normal maps; Bitterli CC-BY interiors; Blender Classroom with its real materials and a coverage report, lit by its world (if its windows are glazed, its sun reaches the room only through the light pass in M5, and the report says so).

**Scope.** 0005 item 34 (Cubic and Smart, Clip and Mirror, projections, UDIM, mips with ray-cone LOD, alpha modes), item 36 procedurals and colour nodes, item 37 bump, normal maps, displacement as bump, item 41 coverage (AO and Bevel no-ops per O26, Raycast magenta), item 42 mapping with its "verify" rows; input nodes (Layer Weight, Fresnel, Backfacing, Light Path, Attribute, Object Info); Pointiness as a sync-computed attribute.

**Out of scope.** Spectral nodes (M4), custom nodes unless P1 passes, true displacement.

**Exit criteria.** 0005 V6 (every op), V10 (at least 95% of node uses over the corpus, zero silent). D3 node coverage. 0007 V6. V7a and the B-14 floor on B-CR. **Look:** procedural sweep against Cycles as a qualitative check (R61); mip filtering free of moiré.

**Measurements inside.** M-3 (sets B-30); M-4; M-5; M-7 / P1; M-8 over 1,000 corpus materials (B-24); P3 in edit mode.

**Depends on.** M1b (independent of M2 except for the shared corpus run). **Size and risk.** M, about 24. Main risk: M-8 translation time on the corpus against V7a.

## M4 — Lights, sky, spectral sources and cameras

**Goal.** Every Blender light and world type, spectral light authoring, spectral material inputs and every camera model render in physical units with efficient light selection and colour-aware adaptive sampling.

**Owner sees.** A gas-discharge lamp gallery (scene builder: Na, Hg, Ne tubes on coloured cards, metamerism against a continuous source); a measured-metals tile; a spectral Cornell box with measured spectra; sun and sky over a day with a sharp sun disc; a many-light night interior (1,000+ emitters); the adaptive-sampling heat map; thin-lens bokeh with blades, orthographic and fisheye views.

**Scope.** 0004 D6 every type (spot blend, sun angle, ellipse, spread, collimated area, IES through 0005 item 41's parser), D9 at scale, D10 full Cycles-style tree, radiometric floor and the excitation term (used from M7), D12 sky (Single and Multiple Scattering ports, sun-disc split), D16 adaptive sampling and tail rule, D17 biased controls (clamps, Filter Glossy per 0005 item 43). Camera rays for thin lens with blades, orthographic and the panoramic types (0004 D23; R63); their importance follows in M5. 0003 d3 `lines` component and bin cap, Rec.2020 and AP1 tables. 0005 item 9 `SELLMEIER`, `GAUSSIAN` and `TABLE` authoring; item 12 complex IOR and the Physical Conductor conversion. Spectral light UI (blackbody default with colour filter, temperature box, RGB toggle, presets, `ar_spectrum_ref`); spectral nodes and the Astroray add menu with badges. Host-side spectrum composition (maths, Gaussian, bandpass, SPD and CSV import, gels). Licence-cleared measured reflectance and n,k data (0001 D8) and the data-pack loader (D8.5). The asset library `.blend` (0007 d42).

**Out of scope.** Caustics from delta lights, fluorescence, science bands, research-grade atmosphere; Preetham and Hosek-Wilkie (visible error, 0004 D12).

**Exit criteria.** 0004 D1 per light type, D6 for every type, D7/D11, D9 zero-pmf emitter, D10 tree against alias table (no tile rejected, at least 2x variance reduction), D12, D16 and its tail, D17. 0003 checks 4, 7 (display and lines), 12, 21. 0005 V3, V4 (complex-IOR residual at most 2%). 0001 V9. A4 (spot). 0007 V18 for orthographic and panoramic framing. B-23 on 10k lights and on 1M emissive triangles. **Look:** lamp-gallery metamerism; metals against their n,k spectra; bokeh shape.

**Measurements inside.** Spike E; M-6; B-23; selection rates with sun plus mesh emitters (0004 Risks); lamp crossings on the 64-lamp scene; whether the Multiple Scattering sky source is spectral; AP1 corners and the ACES white (0003 Risks 4-5).

**Depends on.** M2, M3. **Size and risk.** L, about 32. Main risk: tree quality and build time at a million emitters inside the first-frame gate.

## M5 — Light tracing, gratings and caustics (O17, O25)

**Goal.** Point lamps and spotlights cast caustics, dispersion and grating spectra onto surfaces through the light-tracing pass for every camera model, and the missing path families are reported.

**Owner sees.** A dispersion cabinet (scene builder: slit lamp through BK7 and SF11 prisms); a CD and a 600 lines/mm grating under a spotlight; a laser through a transmission grating onto a screen; caustic classics (ring caustic in a mirrored cup, glass egg, pool floor) with the `DELTA_LIGHT_PATH_GAP` warning where it applies; a brilliant-cut diamond; a wine-glass caustic; Classroom and a window-lit interior lit by the sun through glass.

**Scope.** 0004 D18-D24 (real `generate_light` and `shade_light_path`, light extension variant, partition rules, emission alias table toward casters, P controller under B-36, connection and footprint splat, coverage warnings). `We`/`PDF_We`/`sample_wi` for thin lens, orthographic and panoramic cameras (0004 D23). 0005 items 21-25 complete (transmissive mode, blazed generator, CSV loader, presets, `GRATING_TABLE_LIMIT`). Caster opt-out per object; caustics toggles. Medium stack on light paths for nested dielectrics; texture footprint on light paths (R63).

**Out of scope.** Media on light paths (M6); fluorescent receivers (camera-owned, R44; tested in M7); fixes for the D24 gaps (after M10); light-path wavelengths from the emitter spectrum (R7).

**Exit criteria.** 0004 D19 partition suite with its broken-rule discriminators; D20-D23 (ball lens within 1%, slab, prism deviation within 5%, sodium through BK7, grating centroids within 0.1°); prism hue (slope −0.5 ± 0.1); D23 for every camera type and filter; D24 window sweep, which sets B-36; D21 budget rows. 0005 V8. A10, A11. V7a and the B-14 floor on B-CR and the window interior with the light pass on. **Look:** cabinet colours, grating order positions, early-viewport sparkle under OIDN at 1-16 passes.

**Measurements inside.** B-36; light paths connected per path emitted; narrow-line noise against an emitter-spectrum proposal (R7 stands unless the owner reopens it); texture filtering on light paths.

**Depends on.** M4. **Size and risk.** M, about 22. Main risk: the light-pass share needed by window-lit interiors against B-14.

## M6 — Volumes

**Goal.** Grid, procedural and emissive volumes from Blender render with spectral null-scattering tracking and the emission-absorption marcher, and large scenes spill instead of failing.

**Owner sees.** OpenVDB smoke and fire (Principled Volume with temperature and blackbody); cloud VDBs; a participating-media suite (scene builder: milk in glass, smoky room with a light shaft, copper-sulphate absorption, fog around a lamp); a grating's spectrum as a beam in fog; Bitterli Volumetric Caustic; a scene over the VRAM budget rendering with a spill report.

**Scope.** 0006 D1-D7 (grid leaves across the ABI, NanoVDB, majorant grid from interval evaluation), D9 marcher and estimator rule, D10 equiangular sampling and segment light selection, D11 media on light paths, D12 every phase kind (HG, Rayleigh, Draine, MIE_JD, Fournier-Forand), D13, D18, D20 camera inside and volume hulls, D21 full mapping. 0007 d47 grid sync on spike G's route. VRAM trim and spill (0007 d14).

**Out of scope.** Volumetric fluorescence (M7), spectral cloud material, velocity-driven effects, dust scattering.

**Exit criteria.** 0006 V1-V9 complete, V6 with the cap, V7 for every kind, V19 marcher convergence, V20 recorded (sets volume budgets in 0009). 0007 V21, V14 spill path. O4. A7, A13. V7a and the B-14 floor on a VDB bench scene. **Look:** smoke, fire, clouds, beam in fog.

**Measurements inside.** V20 marcher cost; majorant tightness on a chromatic VDB (0006 Risk 3); dense-cloud cost (Risk 9); ingestion time and memory on the bench VDB.

**Depends on.** M2, M4 (segment light selection), M5 (light paths). **Size and risk.** L, about 33. Main risk: ingestion time, which grows with bounding-box volume, against the first-frame gate.

## M7 — Fluorescence, subsurface, hair and the astro numerics

**Goal.** The remaining material kinds render on CPU and GPU within the kernel gates, every O19 fluorescence case is verified and shown, and the geodesic numerics and the flat-cost branch M10 needs are proven before M10 is planned in detail.

**Owner sees.** Brightener paper, fluorite and a fluorescent pigment chart under UV and white lamps, with the no-UV control dark; a UV-lit gas cloud glowing in visible lines (scene builder, flat space); marble, wax and skin with random-walk subsurface; Bitterli Hair Curls and Fur Ball; point clouds as spheres; the S1 error table.

**Scope.** 0005 item 26 FLUORESCENT with 0004 D10 excitation weighting; 0006 D15 volumetric fluorescence (O19). 0005 item 4a random walk with `intersect_subsurface` and `shade_subsurface`. 0005 item 27 HAIR: OptiX and Embree curves, curve sync, Principled Hair mapping, spectral melanin, Hair Info. Points as spheres (0007 d10). Astro numerics: the `KerrSchild` policy and DP5(4) integrator as kernel-library functions (0003 d28, 0006 D23-D27) with CPU double and GPU float, double and mixed tests (spike S1, with S2, S4 and S5). The curved-region branch in `route_hit` with the `FlatSpacetime` test (0003 d28, check 16, B-35); the large-world warning (0003 d26).

**Out of scope.** Huang near-field hair, MULTILAYER and MEASURED kinds, the geodesic stage itself.

**Exit criteria.** 0005 V12, V2 and V5 per kind. 0003 checks 16, 20, 22. A volumetric slab check for D15: UV in, visible out, re-emitted photons equal quantum yield times absorbed within the 0009 gate, no-UV control dark, NEE on/off agree (the task adds this row to 0006's Verification). 0004 D10 UV test. A12. 0006 V11-V14 at function level in double. 0002 Risk 5 verified. M-2 within the gate. S1 table published and the 0006 D40 variant chosen. **Look:** fluorescence, gas cloud and hair renders.

**Measurements inside.** M-2 (HAIR splits only on measurement); S1, S2, S4, S5; B-35 flat cost; fluorescence collapse against per-lane reradiation (0003 Risk 7: keep the collapse unless a derivation lands).

**Depends on.** M2, M4; M5 (light paths end at fluorescent surfaces); M6 (volumetric fluorescence). **Size and risk.** L, about 34. Main risk: three extension kinds inside two gated kernels (0005 item 18).

## M8 — Film outputs, science outputs, DLSS and motion

**Goal.** Every Blender pass, the final-render features, the scientific outputs and the DLSS viewport mode work, and denoised data cannot reach a science file.

**Owner sees.** Compositor passes (light-path, light groups, Cryptomatte incl. CryptoAsset, AOV, mist, vector, shadow catcher, holdout) that recombine exactly; camera, object and deformation motion blur; a blackbody-lamp scene as Johnson-Cousins band images, photon counts and a 64-bin FITS cube opened in astropy; a FITS image used as a texture; false-colour band views; every heat-map mode; Draft, Preview and Final presets; a DLSS-RR orbit beside OIDN.

**Scope.** 0008 d7 band and cube channels with 0003's `response` component, d9 photon counts, d10-d13a passes, Cryptomatte, statistics passes, science FITS and readback, d19-d20 DLSS-RR if part A passed. Shadow catcher, holdout and the alpha path (R63). Transparent film. 0007 d48 view layers and `register_pass`, d15 motion and deformation blur with the motion pipeline variant, d40 every display mode, d43 science export UI; persistent data, preview-scene reuse, `view_pause`/`view_resume`; render presets. 2D FITS image textures through an own reader (no new dependency; written into 0007's image sources). The AO pass is dropped (R58).

**Out of scope.** PSF and detector noise (0008 d14), celestial WCS, cube tiling, FITS cubes as volumes; the OptiX denoiser unless the owner answers yes (Open items).

**Exit criteria.** 0008 V5-V7, V9, V12 part B on the diffuse, glossy and depth-of-field scenes. A8. 0003 checks 7 (band component) and 23. 0004 D17 science refusal under a clamp. 0007 V17 for pass and output settings. **Look:** pass recombination in the compositor; DLSS frames for ghosting and highlight clamping.

**Measurements inside.** Spike F part B; DLSS recreate time per preset step; B-27; guide noise under Mix Shader (0008 Risk 10); denoising-surface definition (Risk 6); Cryptomatte acceptance (Risk 5); motion-variant first frame.

**Depends on.** M1b film, M4 light groups, M6 volume passes. **Size and risk.** L, about 35. Main risk: spike F part B; if DLSS fails it, DLSS stays an option, off by default, and the owner is told (O11 "if it can be").

## M9 — First release 2.0.0

**Goal.** The breadth built in M1a-M8 ships as 2.0.0 after hardening, with every gate met on the full canonical set; no feature work.

**Owner sees.** The release gallery (every earlier showcase plus Bistro at night) rendered by the release build through Blender, the benchmark report, and a clean install from the self-hosted extension repository.

**Scope.** `release.ps1`, the release preset with every architecture and 0001 V13, V7 (needs owner question 4), V10, V12; `ASSETS_NOTICE.md`; the UI completeness check (0007 V17 complete: every property in its UI table exposed and either changing the image or reported); defect fixes found by the full gate.

**Exit criteria.** 0001 D10.4 release gate. A1-A13, D1-D4, O1-O4 green. Every bench scene within budget (B-1M, B-5M, B-TX, B-CR, window interior, VDB). 0007 V13 soak with every feature, zero crashes; V14; V17 complete. No PROVISIONAL limit left in `budgets.toml` (script check). **Look:** owner and lead review the gallery.

**Measurements inside.** Full `check full`; bench A/B against the M1a tag; a real 4K display and the OpenGL backend; CUDA re-initialisation cost.

**Depends on.** M1a-M8. **Size and risk.** S, about 10. Main risk: a late defect in a feature combination no milestone gated; rule 2 makes per-feature performance surprises unlikely here.

## M10 — Black hole in an emission nebula (O8)

**Goal.** A Kerr black hole inside a line-emitting nebula with embedded stars renders physically calibrated in the viewport and as science output.

**Owner sees.** B-NB (scene builder from research 16 gaps 10 and 12, without a disk) in the viewport and F12; the lensed star field; Hα and [O III] band images and a line-centroid map; the Strömgren-sphere oracle.

**Scope.** `advance_geodesic` and `advance_geodesic_f64` (0002 D8; R18, R46) with the geodesic record, outcomes and frequency shifts (0006 D22-D31); the NEE gate (D28); light paths ending in the region; the NEBULAR op, dust extinction and the `nebular_lines` builder with cited constants (D32-D34); line-aware sampling with Doppler widths (D36); the black-hole object, panel and presets (0007 d41); science provenance keys (D37); DLSS part B on the nebula and lensing scenes. **Stretch:** a thin Novikov-Thorne disk (FI 230; research 17 second tier), started only after every exit criterion below passes and never gating M10.

**Out of scope.** Slim disks, ADAF, jets, curved NEE, moving cameras, multiple holes, polarisation.

**Exit criteria.** 0006 V10-V19, A14, 0003 checks 17-19, the S1 variant in use, B-NB in the bench with V7a and the B-14 floor. **Look:** B-NB, ε_NEE chosen by inspection (0006 Risk 8).

**Measurements inside.** S3 on the target scene; capture rule in Kerr-Schild (0006 Risk 2); line sampling under redshift (0003 Risk 6).

**Depends on.** M9 and the core listed below. **Size and risk.** L, about 30. Main risk: `advance_geodesic` in double with the marcher inlined exceeding its spill and stack allowance (0006 Risk 7).

**Core the target needs, and where it lands.** M1a: λt, `apply_frequency_shift`, the `IN_CURVED_REGION`/`LAST_SEGMENT_CURVED` flags, `ray_tmax`, the geodesic termination reasons and `LIGHT_PATH_IN_REGION`, side-buffer allocation, the double CPU build, the scene-origin rule, `ar_edit_set_astro`, the interval-rule column in `ops.def`, the `lines` hook in `WavelengthTable`. M1b: the medium state and tracking in the double build. M4: the `lines` component and spike E. M6: the marcher shared with `advance_geodesic`, NanoVDB grids, majorant grids, Volume objects with density, temperature and ionisation grids. M7: `KerrSchild`, DP5(4), S1, the region branch in `route_hit` with its flat cost measured, volumetric fluorescence. M8: band, photon and cube channels, FITS provenance, emission-mechanism light groups. M10 then adds one stage, one op, one table and the object UI (0006 Consequences).

## Measurements and spikes

| Id | Question | Where | Pass | On a negative result |
|---|---|---|---|---|
| Wavefront overhead | GPU idle and partition share of a 1080p 1 spp pass (0002 D7) | M1a | B-19, B-17, B-14 on B-1M | 0002 D7 remedy (a), then (b), (c), before M1a closes; if B-14 still fails on B-1M, run spike C (megakernel viewport prototype) and settle a superseding 0002 record before M1b starts. Spike C does not run otherwise. Result (rulings 79, 80, 84, 85): remedies taken in the order of measured cost, (c), the sort scan, (a) as stream-ordered transfers, (b) measured and not built; B-14 and B-17 met, partition 9.4 % met on B-1M, idle 11.0 % against 10 % open for the owner, B-5M unmeasured until task 136; spike C not run. Re-measured at M1b's close and after every landing that touches the scheduler (`check perf`) |
| First frame | V7a/V7b with materials, textures and lights (0007 d35) | M1a, every close | ≤ 1.5x Cycles | 0007 d35 remedies in order (`as_pointer()` topology reads, GAS disk cache, earlier parallel inlining) before more sync cost is added |
| OptiX cold compile | B-18 cold, incl. the motion pipeline variant | M1a | B-18 | split modules or cache compiled pipelines before M1b; motion variant compiled lazily on first use |
| M-1 | Record, register file, VM, all lobes in the capped fused kernel (0005) | probe M1a; ratified M1b, M2 | 0002 D10 allowances | M1a probe: a gate-only TU with the real record, register file and `ops.def` dispatch, spike A's lobes standing in. Stack fails: fp16 record scalars (−48 B). Spills fail: 0002 D10 ladder |
| M-2 | Extension kernels with three kinds | M7 | 0002 D10 | shrink live state; HAIR split (0005 item 18) |
| M-3, M-4, M-5 | VM overhead, corpus register use, software filtering | M3 | B-30 set; 99% within 24 registers | M-4: 16 more registers paid by fp16 scalars; M-5: hardware filtering only if agreement holds |
| M-6 | F82 fit residual for complex IOR | M4 | ≤ 2% | exact Fresnel stays in the lobe; albedo error reported |
| M-7 / P1 | Custom nodes through `inline_shader_nodes()` | M3 | identity kept | no custom nodes; Material panel only (0007 d39) |
| M-8 | Translation time, 300 and 1,000 materials | M1a (S2), M3 (corpus) | B-24, V7a | per-node lowering moves into the native compiler, then a value-only patch path |
| E | Wavelength sampling: default pdf, response and line weights, floor, random-hero variance (0003 Risks 1, 11) | M4; rechecked M10 | 0003 check 7, lower variance than the alternatives | keep sech² plus the 5% floor and tune weights per scene class; no design change |
| F part A | DLSS-RR from a third-party process, no DLL, own project id (0008 V12) | before M1a | all of (a)-(d) | DLSS-only items are not built; OIDN only; owner told |
| F part B | DLSS image quality | M8 (three scenes), M10 (two astro scenes) | 0008 V12 B | DLSS stays an option, off by default |
| G / P6 | Grid ingestion via Blender's `openvdb` module (0006 Risk 5) | before M1a | first frame within the gate | volumes are non-optional (O7): the lead proposes native `.vdb` reading (OpenVDB, a 0001 D9 change with TBB-collision risk) to the owner before M1b certifies the DLL set |
| Camera importance | `We` for orthographic and panoramic cameras without a reference implementation (0004 Risks) | M5 | D23 for every type | derive from the camera's ray map and verify by D23's adjoint test; if it still fails, the owner decides whether panoramic views may lack the light image (a loosening of O25's reach) |
| S1 (+S2, S4, S5) | Float, double, mixed geodesics near the photon ring (0006 D40) | M7 | 0006 D40 thresholds | mixed if it passes; if only double passes, the astro viewport runs below full resolution, an owner question asked at M7 close |
| S3 | Wavefront round trips on the target | M10 | pass time within B-14's floor with the controller | restrict media in the region to programs without procedural ops, or split marching into its own stage (0006 Risk 7) |
| Null calibration, detection power | Welch false-positive rate; gate power (0009) | M2; M1a | ≤ 2 failing repeats; each injected bug caught | 17(v) gates nothing; raise sample counts or m |
| P2-P5 | `view_pause`, edit-mode reads, heat-map route, VRAM reserve R (0007) | M1a (P3 edit mode M3) | as 0007 states | P2: addon pauses sessions itself; P3: `as_pointer()` for large edit-mode meshes; P4: POST_PIXEL handler; P5: R reset |

## Designed when built (R63)

| Topic (owner record) | Decided in |
|---|---|
| Ray-origin offset, tmin, self-intersection, shadow terminator (0002) | M1a for triangles and lamps; M7 for curves, spheres and the random walk's local rays |
| Shadow catcher, holdout, alpha path (0008) | M8 |
| Camera and lens models incl. panoramic (0004) | M1a perspective; M4 rays for thin lens, orthographic, panoramic; M5 their importance |
| Ownership and compaction of the active-pixel list (0008) | M1a ownership; M4 compaction with the adaptive tail |
| Which kernel shades the subsurface exit vertex (0002, 0005) | M7 |
| Medium-stack handling on light paths (0006) | M5 (nested dielectrics); extended in M6 (media) |
| Texture footprint on light paths (0005) | M5 |
| Array counts and light descriptor fields in the ABI appendix (0007) | M1a for meshes, instances, point and area lamps; M4 for every other light; M6 grids; M7 curves and points |
| Missing report codes (0001) | in the task that raises the condition, every milestone |
| Version pins incl. Embree (0009) | M1a |

## Open items

**Not designed, kept visible.**
- **Light and shadow linking** (R45): no fields, hooks or design. If the owner wants it, it needs a superseding record touching the path state, shadow item, light tree and ABI; earliest after M10.
- D24 gaps for delta lights (caustics seen through glass or a mirror, and their second bounce): warned in 2.0 (O25); probabilistic PPM or MNEE chosen by measurement after M10.
- AO and Bevel nodes are no-ops and Raycast is magenta (O26, 0005 item 41) until in-shader ray queries are allowed by an owner decision and a 0002 amendment.
- True displacement (0005 item 37, 0007 d36): reported as bump in 2.0; tessellation has no design.

**Owner questions, with the milestone that needs the answer.** Before M1a: CLA (0001 q1) and OptiX licence pass-through (0001 q2), since the repository goes public with M1a; CIE data (0001 q5); the unit anchor (0003 q1, first half), because it fixes every reference image from M1a on; the DLSS licence reading (0008 q1), because a passing spike F puts the hit-distance guide into `shade_surface` in M1b; whether O24's "the other denoisers" includes the OptiX denoiser (FI 180; if yes it is built in M8, otherwise it needs a measured win per 0008). M1a: Nsight counter enable (0002 q1); open-weight worker and deleting superseded ADRs (0009 q3-q4). M8: science refusal for Blender-unit emitters (0003 q1, second half); DLSS at rest (0008 q2). M9: Windows Sandbox for V7 (0001 q4); NIST-compiled constants from primary papers (0006 Risk 6), before M10 starts.

**Record risks, by milestone.** M1a: 0001 Risks 1-3, 9, 11; 0002 Risks 1-4, 9, 13; 0003 Risks 2-3, 9; 0007 TTFF, batching schedule values from `render_scheduler.cpp`, P2-P5, unmeasured material, texture and light sync; 0008 Risks 1, 9; 0009 hosted runner, Blender start-up, PROVISIONAL budgets. M1b: 0008 Risks 2-3. M2: 0002 Risks 7-8; 0005 thin-film base attenuation cross-check; 0009 Mitsuba and pbrt-v4 availability, Welch calibration. M3: 0005's four "verify" rows; 0007 P1, P3. M4: 0003 Risks 1, 4, 5, 11; 0004 light-tree build, importance weighting, lamp crossings, sky sources. M5: 0004 camera importance for orthographic and panoramic, narrow lines, window interiors, light-path efficiency, early sparkle, texture filtering on light paths, grating formulas re-read; 0005 grating books. M6: 0006 Risks 3, 9, 10 (Risk 5 before M1a). M7: 0002 Risk 5; 0003 Risks 7-8; 0005 random-walk re-read; 0006 Risk 1. M8: 0007 DLSS re-creation; 0008 Risks 4-7, 10. M9: 0001 Risks 4, 10; 0002 Risk 12; 0007 4K and OpenGL, CUDA re-initialisation; 0009 untested architectures. M10: 0003 Risk 6; 0006 Risks 2, 4, 6-8. After 2.0: Blender 5.4's OpenPBR node (0005), celestial WCS and cube tiling (0008 Risks 7-8), images without an interop id stay magenta (0007).

**Blender 5.4.** The headless sync suite and API-surface check gain a 5.4 leg in the first milestone that closes after `bpy` 5.4 appears on PyPI (0007 d46), covering `.corner_vert` and the other fragile points; it is informational until 5.4 ships (0009).

## Coverage: v1 feature inventory

Every line of `feature-inventory.md` with a feature, by line number. "After" = after M10. "Not planned" gives the reason.

| Section | Lines | Where |
|---|---|---|
| Materials | 12, 22 (constant and image base colour, roughness, emission), 48 | M1a |
| Materials | 15, 28 | M1b (F82 metal; mirror at roughness 0) |
| Materials | 13, 16-18, 20, 22 (rest), 23-26, 44, 46, 47, 49 (contact sheet = the shader-ball sweep) | M2 |
| Materials | 38, 40, 41 | M3 |
| Materials | 27, 29, 30, 42, 43 (band output in M8), 45 | M4 |
| Materials | 31-36 | M7 (Burley and Kajiya-Kay reported as approximated) |
| Materials | 14 Phong; 19 rough-glass random walk; 21 Disney BSDF; 37 two-sided wrapper; 39 closure graph | not planned: non-physical (Glossy preset); CPU test oracle only (0005 item 5); no Blender node, Principled covers it; thin-wall and shading frame cover it; rejected (0005 Alternatives) |
| Textures and nodes | 53, 54 (Repeat, Extend), 55 (Linear), 78, 90 | M1a |
| Textures and nodes | 68 (roughness, metallic) | M1b; transmission and IOR M2 |
| Textures and nodes | 54-56 (rest), 57 (mips, UDIM), 58-60, 62-67, 69-73, 75 (Pointiness as a sync-computed attribute), 76, 77 | M3 |
| Textures and nodes | 80-83, 85 (add menu and badges), 86, 87 (Material panel; nodes only if P1 passes), 88 (Spectrum Mix) | M4 |
| Textures and nodes | 61 GPU procedural bake | superseded: procedurals evaluated per point in the VM (M3) |
| Textures and nodes | 74 true displacement; 88 Element Emission; 89 Cloud Volume | after: no tessellation design; element line data needs a data pack (0001 D8); spectral cloud material (0006 D38) |
| Textures and nodes | 57 texture cache; 79 Astroray Output node; 84 NRC hint | not planned: no demand loading, spill instead (0007 d13-14); Material panel kinds (0005 item 16); no neural cache |
| Lights | 94, 97 (rectangle, disk), 98, 100-103, 112 | M1a |
| Lights | 104 (tree traversal over every emitter), 105 | M1b |
| Lights | 95, 96, 97 (ellipse, spread), 99, 106-111, 113, 114 (full builder; alias table as oracle), 117 | M4 (111 through gratings in M5) |
| Lights | 118 | M2 |
| Lights | 116 | M8 |
| Lights | 115 goniometric, projection, portal, lens-coupled spot | after: not designed (0007 d36) |
| Sky and world | 122, 133 | M1a |
| Sky and world | 123, 124 | M1b |
| Sky and world | 125 spectral environment atlas | superseded: world program evaluated per wavelength, table only for sampling (M1b) |
| Sky and world | 134 | M2 |
| Sky and world | 127-129 | M4 (129 as far as Blender's sky types expose it) |
| Sky and world | 126 Preetham bake; 130 Hosek-Wilkie | not planned: visible error (0004 D12) |
| Sky and world | 131, 132 Rayleigh and per-wavelength UV/IR sky | after: research-grade atmosphere (research 17 §5) |
| Volumes | 141 (tracking core) | M1b |
| Volumes | 138, 139, 141 (NEE), 143 (homogeneous), 144 (stack), 146 (bounce limit) | M2 |
| Volumes | 140, 141 (equiangular), 142, 143 (phase options), 144 (camera inside), 145 | M6 |
| Volumes | 146 (passes) | M8 |
| Volumes | 147 velocity motion and dense-media gain; 148 cloud material | after: media static in 2.0 (0006 D2); 0006 D38 |
| Cameras and sensors | 152, 155, 161, 165 | M1a |
| Cameras and sensors | 153, 154, 156, 157 | M4 rays; M5 importance |
| Cameras and sensors | 158, 159, 162-164, 166 (camera curves as response channels), 167, 169, 170 | M8 |
| Cameras and sensors | 160 shutter curves, rolling shutter; 168 PSF, noise, ADU | after: not designed; 0008 d14 |
| Cameras and sensors | 166 selectable observer | not planned: one observer (0003 d14) |
| Passes and outputs | 178 (heat maps; convergence plots in the bench report), 187 | M1a |
| Passes and outputs | 174, 179 (final render) | M1b; viewport OIDN M2 |
| Passes and outputs | 175-177, 181-185 (182 decided with the alpha path; 183-184 written by Blender; 185 as FITS) | M8 |
| Passes and outputs | 180 OptiX denoiser | owner question before M1a (O24 "the other denoisers"): M8 if yes; otherwise only with a measured win (0008) |
| Passes and outputs | 185 EXR cube; 186 light path expressions | not planned: engine writes no EXR (R54); not designed |
| Render settings | 191, 193, 194 (total), 195, 196, 204, 205 (triangles) | M1a (191's distribution settled by spike E in M4) |
| Render settings | 194 (per type) | M2 |
| Render settings | 192 | M4 |
| Render settings | 194 (caustic toggles), 197 (light tracing replaces the photon map; R7 wavelengths), 199 | M5 |
| Render settings | 205 (curves, spheres) | M7 |
| Render settings | 198 manifold and polynomial caustics; 200 path guiding; 201 ReSTIR | after: candidates for the D24 gaps; only with a measured gain (0004 D26) |
| Render settings | 202 neural radiance cache; 203 AO integrator | not planned: slower in v1, not designed; AO pass dropped (R58) |
| Viewport and tools | 209, 211-213, 214 (display modes), 220, 221, 223 | M1a |
| Viewport and tools | 215 | M1b |
| Viewport and tools | 210 (only below the floor, O10), 214 (viewport OIDN) | M2 |
| Viewport and tools | 217, 219 (library picker) | M4 |
| Viewport and tools | 214 (full pass selector), 216, 219 (pass panel) | M8 |
| Viewport and tools | 224 | M7 |
| Viewport and tools | 218 | M10 |
| Viewport and tools | 222 offline .blend importer | not planned: ABI recordings replace it (0001 D1.4) |
| Astrophysics | 228, 229, 231, 235 (hole only), 236, 237, 238 (extinction), 241 (strong lensing), 242 (emission-mechanism light groups) | M10 |
| Astrophysics | 230 thin disk | M10 stretch: O8 names the nebula, not a disk, and a disk adds an emission model B-NB does not need; built in M10 only after its exit criteria pass, otherwise first after |
| Astrophysics | 232 slim disk, 233 ADAF, 234 jet, 235 disk sub-panels, 238 reflection nebula, 239 element library, 240 HMXB, 241 weak-lensing pass, 242 photon-ring subimages | after: 0006 "can wait", none needed by O8; 239 needs a data pack |
| Data I/O | 246-248, 255 | M4 |
| Data I/O | 249 (2D FITS textures, own reader) | M8 |
| Data I/O | 253 | M6 |
| Data I/O | 254 (Blender writes EXR; engine reads EXR textures from M1b) | M8 |
| Data I/O | 256 (spill) | M6 |
| Data I/O | 250-252 FITS cubes, HDF5, yt, npy, SPH | after: no volume ingestion beyond Blender grids designed; the leaf ABI accepts them later |
| Data I/O | 256 demand loading | not planned: 0007 d13 |
| Ideas | 260 gratings | M1b mechanism, M5 complete |
| Ideas | 261/287 line absorption | M4 (`TABLE` sources), M6 (media); molecular spectra after (licence, 0001 D8.3) |
| Ideas | 262 fluorescence | M7 |
| Ideas | 263, 282 gemstones and diamond | M4 presets, M5 showcase |
| Ideas | 264 measured BSDF | after: enum only (R36); cloth beyond sheen not planned (fuzz) |
| Ideas | 265 owner's early list | covered by research 17's catalogue (below) and O20 |
| Ideas | 266 in-air rainbow; 275 neutron-star scene | after: needs tabulated Mie phase; astro scene after M10 |
| Ideas | 267 showcase tiles | M5 wine glass, M6 god rays, M9 viewport side by side |
| Ideas | 276 CryptoAsset | M8 |
| Ideas | 276 indirect-only visibility; light and shadow linking | after (not designed); linking is the R45 open item |
| Ideas | 268, 269, 270, 271, 272, 273, 274, 277 | not planned: not engine work; OIDN pinned, only with a measured win; out of scope (0001 D11; Blender writes outputs); rejected in 0004 Alternatives; Wilkie hero MIS only with a 10% gain (0003 d7); no polarisation (0003 d31); owner non-goals in v1; JH illuminant convention fixed (0003 d16) |
| Added after review | 281 analytic sphere | M7 (sphere primitives) |
| Added after review | 283 | M3 (Hair Info M7) |
| Added after review | 284 | M5 |
| Added after review | 285 | per-strand random M7; near-field after |
| Added after review | 286 | M1a |
| Added after review | 288 tooling | chi-squared, furnace and coverage matrix M1a-M3; perceptual reference gates and Cycles A/B not planned (O1, spine §9) |

## Coverage: research 17 first and second tier

| § | First tier | Second tier |
|---|---|---|
| 1 Surfaces | M1a: emission. M1b: Mix Shader, opacity, F82 metal. M2: diffuse roughness, dielectric with compensation, coat, fuzz, thin film, thin wall, interior absorption and scatter, dispersion (caustics M5), anisotropy, transparent shadows, legacy closures. M7: random-walk subsurface | wet surface: M4 asset-library preset |
| 2 Spectral inputs | M1a: RGB upsampling (Rec.709; Rec.2020 and AP1 in M4). M4: measured reflectance (licence-cleared subset), drawn spectrum, complex-IOR conductor, Sellmeier and tabulated n(λ), spectral σa, IR/UV material behaviour (band output M8) | spectrum maths, Gaussian, SPD import: M4. Spectral melanin: M7 |
| 3 Extension lobes | grating: M1b mechanism, M5 complete; roughness-broadened orders not planned (no licence-compatible model, 0005 item 25) | hair and fur: M7 (O24). Multilayer stack, measured BSDF: after, enum values only (R36) |
| 4 Wavelength coupling | none | fluorescence, surface and volumetric: M7 (O19, O24). Doppler and gravitational shift: M10 (packet operation in M1a) |
| 5 Emitters | M1a: point and area lamps, emissive meshes, absolute units. M1b: HDRI sampling. M4: spot and sun, blackbody default light, lamp presets (sources cleared under 0001 D8, otherwise a data pack), laser and narrow lines (through gratings M5), line-aware sampling (after spike E), unified sampling at scale, IES, sky with sun disc | research-grade atmosphere: after. Gas-discharge and element lines: M4 lamp presets; element library after (data pack) |
| 6 Media | M1b: tracking core. M2: homogeneous media and world fog, chromatic σ with spectral MIS, nested media, volume NEE, Principled Volume constants. M6: grids, camera inside, equiangular sampling, Principled Volume grids and blackbody | Rayleigh: M6. Mie: M6 as MIE_JD; tabulated θ×λ after (cloud material). Dust: M10 extinction; scattering after. Emission nebula, line emission and absorption in volumes: M10 |
| 7 Sensors | M1a: CIE 1931 observer; sample and bounce heat maps (every mode M8) | bands and false colour, FITS cube, filter × QE and camera curves, photon counts: M8 (O24); EXR cube and selectable observer not planned (R54, 0003 d14) |
| 8 Astrophysical | none | Schwarzschild and Kerr: M10 (numerics M7). Nebula around the hole: M10. Thin disk: M10 stretch, otherwise after. FITS import: images M8; cubes after (no volume ingestion designed) |

## How the roadmap changes

Only the lead edits this file, in a commit of its own whose message names the cause: a measurement in this file's table, an owner decision, or a ruling. A negative result applies its listed fallback and edits the roadmap in the same commit that records the result. Exit criteria may be tightened freely; loosening one, or moving any first-release item (O7, O16, O17, O24, O25) past M9, needs the owner's approval first. Closed milestones are not edited; git keeps the history, and `docs/STATUS.md` names the current milestone. Ids of started milestones are never reused or renumbered; a milestone that splits keeps its id with a suffix (M6a, M6b).

## Review and resolution

The first draft was reviewed independently before any milestone started; this version answers it. One line per objection.

1. M1 too big, breadth before the B-19/B-14 decision — ACCEPTED: split into M1a (~25) and M1b (~22); spike C is decided at M1a close. M1a keeps a minimal VM and image textures, which the reviewer's M1a lacked, because O27 names textures and the first-frame measurement must include materials.
2. 0006 D8 moved to M2 without authority — ACCEPTED: D8 tracking with the requeue is in M1b over a test medium; the placement note is gone.
3. Unit anchor and DLSS licence asked too late — ACCEPTED: both before M1a; the science-refusal half of 0003 q1 stays at M8, where it is first needed.
4. Performance on other bench scenes found late — ACCEPTED: rule 2 gates the 24/s floor on every bench scene from the milestone that adds it and reports full-resolution rate and settled resolution.
5. Nothing stops features landing switched off — ACCEPTED: rule 5.
6. Volumetric fluorescence neither verified nor shown — ACCEPTED: M7 has the slab check (written back into 0006) and a UV-lit gas cloud showcase.
7. M2 far larger than claimed — ACCEPTED: split into M2 (lobes, shadows, media) and M3 (nodes, textures, corpus).
8. Classroom needs later features — ACCEPTED in part: Classroom's materials and coverage are shown in M3 and its sunlit look is gated in M5, since sun through glass is light-pass only (0004 D19). The sun lamp is not moved to M2: alone it would not light Classroom. Thin-lens and other camera rays move one milestone earlier, to M4.
9. Spike G too late — ACCEPTED: before M1a, with spike F part A.
10. Milestones depend on the owner's availability — ACCEPTED: rule 4; exits use scene-builder scenes, and owner scenes are gallery items.
11. Nested-dielectric code in M1 cannot be tested — ACCEPTED: CPU stack fixture in M1b, 0006 V4 in M2.
12. Principled Metallic without a metal lobe — ACCEPTED: F82 metal in M1b. Correction: spike A has a GGX metal with Gulbrandsen complex Fresnel, not F82; it is the starting code.
13. Missing must-exist items — ACCEPTED: the denoising-surface bit and DLSS hit distance (M1b), doctest executables with pure functions and shutter-relative time (M1a).
14. Viewport OIDN breaks B-14/B-26 — ACCEPTED: M1b has final-render OIDN only; viewport OIDN lands with quality stepping in M2.
15. The 5.4 CI leg waits for M8 — ACCEPTED: it lands in the first milestone after `bpy` 5.4 is on PyPI.
16. Rule 3 conflicts with resetting budgets at release — ACCEPTED: each is reset where first measured; M9 only checks that none remain.
17. "No bench regression" blocks legitimately slower features — ACCEPTED: re-blessing with an accepted A/B report.
18. Release milestone overloaded, UI completeness unchecked — ACCEPTED: spill to M6; persistent data and presets to M8; region branch and large-world warning to M7; M9 is hardening, with V17 as the UI check.
19. Motion pipeline variant measured late — ACCEPTED: its cold compile is measured in M1a.
20. Camera importance has no reference and blocks the light pass — ACCEPTED: rays in M4, `We` in M5, with a negative branch in Measurements.
21. FI 180 OptiX denoiser — ACCEPTED: owner question before M1a.
22. FI 230 thin disk — ACCEPTED: M10 stretch with the reason stated.
23. FI 249 FITS textures — ACCEPTED: 2D FITS textures in M8 through an own reader; cubes stay after.

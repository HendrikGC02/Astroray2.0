# Checkpoint 1 — research summary (2026-11-14)

Stage 1 is complete: 14 research files in `docs/research/` (each fact-checked by a second agent), 4 general lesson files and 11 topic files inherited from v1 in `docs/v1-lessons/`. This page is the lead's reading of them. Nothing here is decided until a decision record exists.

## Where the research converges

| Area | Direction | Source |
|---|---|---|
| Blender attachment | Python `RenderEngine` in a Blender extension; native engine in-process behind a flat, versioned C API of scene deltas. No Hydra delegate for 2.0. A separate render process stays a fallback. | 01 |
| Shader graphs | Read only through `inline_shader_nodes()`; never walk raw node trees. | 01, 07 |
| Viewport display | One CPU-to-GPU upload per presented frame is the floor for an addon on 5.3; zero-copy exists only inside Cycles. Engine outputs scene-linear and Blender applies the display transform. | 02 |
| Kernel language | Restricted C++ compiled by MSVC for CPU and by the CUDA compiler for GPU. Slang is a time-boxed comparison, not the default. | 03 |
| GPU scope | NVIDIA-first (CUDA + OptiX for traversal, Embree on CPU), with a clean traversal interface so another vendor is a later port. Nothing else is built or tested in 2.0. | 03 |
| Integrator | Wavefront stages written once, structure-of-arrays path state, counter-based Owen-scrambled Sobol sampler, NEE with a separate shadow stage. | 04 |
| Spectral | 4 stratified wavelengths, wavelength packet in the path state, sampling distribution replaceable so other bands are a change of table. | 05 |
| Materials | One pinned OpenPBR record is all the integrator sees; Principled 5.3 and a future OpenPBR node both lower into it. Lobes written from the spec, cross-checked against Adobe's reference. | 05 |
| Astrophysics hooks | Spacetime as a compile-time policy with flat space as the trivial case; one segment loop for curved rays and the BVH; double-precision CPU reference first. | 06 |
| Build | CMake + Ninja + MSVC only; engine as a C-ABI DLL; runtime kernel compilation for the edit loop, ahead-of-time for the gate. | 08 |
| Tests | Deterministic lobe tests on the CPU target in seconds; analytic and explicit-spectrum oracle scenes; all v1 reference images treated as untrusted. | 09 |
| Denoising | One denoiser interface; OIDN default; science buffers never denoised. DLSS only as an optional NVIDIA viewport mode, never bundled. | 10 |
| Licensing | Thin GPL-3 addon, permissive engine core, distributed outside extensions.blender.org, no NVIDIA binaries in the repo. | 11 |
| Process | Rules enforced by scripts rather than hooks; about three agent roles; small task files; one page of status. | 13 |

## Contradictions and how the lead proposes to settle them

1. **Material evaluation.** 04/05 want a fixed OpenPBR record; 07 wants a Cycles-style node VM. Proposal: both, in two steps. The VM evaluates node graphs into the *parameters* of the fixed record; it never emits closures. Mix Shader blends or selects stochastically; Add Shader and other closure sums raise a visible error at first.
2. **Register budget.** 03/04 rely on launch bounds and no-inline as levers; v1 measured both as ineffective on this GPU generation. This is the central bet of the rewrite and is settled only by measurement (spike A).
3. **Binding.** 01 says nanobind wheel; 08 says C ABI with ctypes. Proposal: C ABI is the contract; start with ctypes; add a compiled binding only if profiling shows the Python boundary costs time.
4. **Hero wavelength distribution.** 05 adopts the PBRT distribution; v1 measured it worst in blue and found no single winner. Proposal: keep the distribution replaceable and decide by spike E.
5. **Precision.** A `Real` type switchable to double on the CPU target from the start. GR uses double on CPU as the reference; float on GPU only if its measured error near the photon ring is acceptable.
6. **OptiX module format** (PTX vs OptiX-IR) and **wavefront vs megakernel**: one forum post and one preprint respectively. Measured in spikes A and C.
7. **First milestone contents.** 04 includes homogeneous media and defers the light tree; 07 defers volumes. Depends on the owner's answer on volumes below.
8. **Viewport buffers.** Denoiser research wants motion vectors and hit distances from day one; integrator research omits them. Proposal: produce albedo, normal, depth and motion vectors from the first milestone; add the rest when a temporal denoiser is actually adopted.
9. **OpenPBR version.** Files cite 1.1, 1.1.1 and 1.1.2. Pin 1.1.2 after confirming the release tag.
10. **Cycles sky source licence.** v1 notes say two files are GPL; two v2 research files could not confirm either way. Per-file check before borrowing any sky code.

## Provisional scrap / keep

**Scrap:** separate CPU and GPU implementations; the fused shade kernel and its template variants; MinGW and the multiple build trees; pybind11 monolith binding; Python in the per-frame pixel path; full scene re-export on edit; constant-folding node translator; baked sky with fitted exposure; Cycles pixel-parity gates and their reference images; the package/round/gate-letter process, session journal, orchestrator and multi-harness agent copies.

**Keep as knowledge (already in `docs/v1-lessons/`):** physics and statistics rules; observer-consistency rule for spectral colour; redshift and Kerr chart fixes; Blender depsgraph and camera-change behaviour; viewport threading design; statistical tile test instead of SSIM; furnace-test traps; light-type energy conventions.

**Keep as data (regenerate or re-verify, do not copy blindly):** upsampling tables, CIE data, measured metal data, emission-line tables.

**Port nothing as code** until a decision record says a specific piece is worth it.

## Measurements required before the architecture is written (Stage 2)

- **A. Kernel budget.** A split OpenPBR-lobes + NEE + 4-wavelength shade stage on the 5070 Ti: registers, stack, spills, whether launch bounds are honoured, PTX vs OptiX-IR, cold and warm compile seconds, effect of a shared-header edit, compiler-cache hit rate. Same stage in Slang for comparison. *This decides whether the rewrite's central premise holds.*
- **B. Blender end-to-end.** In Blender 5.3 on Vulkan: sync of a 1M-triangle edit, 1080p and 4K present cost, time to a usable low-sample image, against the Cycles viewport on the same scene.
- **C. Wavefront vs megakernel** on identical stage functions: one-sample latency and 64-sample throughput.
- **D. CPU target.** Same source built with double precision: build and link time, test throughput against the 10 s and 3 min budgets, CPU/GPU agreement with Embree vs OptiX, float geodesic error.
- **E. Spectral sampling.** Equal-time colour and brightness noise for 4 vs 8 wavelengths vs an RGB path on dispersive, coloured-light and line-emitter scenes, with two sampling distributions.

### Added after the owner's answers (2026-11-14)

- **B is extended.** It also measures time to first frame on a large scene (at least 5M triangles, 10k objects) against Cycles, split into Python sync, data transfer, acceleration-structure build and first sample. If the extension route is more than 2x Cycles on first frame, or cannot hold 24 updates per second at 1080p, the deeper routes are costed: a native module reading Blender's data directly, or a patched Blender with a Cycles-style display driver.
- **F. DLSS feasibility.** Drive DLSS Ray Reconstruction from the engine's CUDA context inside Blender 5.3 (the route Cycles uses), with the driver-supplied model and no bundled NVIDIA binaries. Pass: it initialises, upscales a 1-sample-per-frame feed with motion vectors, and its licence terms permit an Apache-2.0 project to call it.
- Because DLSS is wanted, the integrator produces the full set of denoiser buffers (motion vectors, depth, albedo, normal, roughness, hit distance) from the first milestone. This replaces proposal 8 above.
- Dynamic resolution: a controller holds the update rate above the floor by scaling render resolution, with DLSS or a filter upscaling to the viewport.
- First astrophysics target is fixed: black hole inside an emission nebula. This is backward ray transfer through an emitting, absorbing volume along curved rays, so it needs no scattering integrator and fits the planned core.

## Decisions that are the owner's (answered: see `decisions/0000-owner-decisions.md`)

1. **Licence and commercial intent.** Open source only, or keep a commercial option open? (Recommendation: Apache-2.0 core, GPL-3 addon, which keeps both.)
2. **Distribution.** Self-hosted extension repository and GitHub releases, rather than extensions.blender.org? (Recommended; the official site rules out a bundled compiled engine.)
3. **Repository.** Public or private, and where hosted. This decides whether this PC can act as the GPU test runner.
4. **Volumes in the first usable release**, or surfaces first and volumes second?
5. **First astrophysics deliverable:** lensed black hole with thin disk, emission-nebula volume, or X-ray binary with scattering corona. The third needs a different integrator.
6. **Viewport targets in numbers:** time to first updated frame, frame rate while orbiting, resolution.
7. **Unsupported nodes:** magenta material plus a per-render report, or abort the render?
8. **Platforms:** Windows only for 2.0?
9. **Merge authority in the build phase:** lead merges after checks and review, or owner approves each merge, or owner approves at milestones.
10. **CPU role:** a shipping render device, or a correctness and test target only?

Defaults the lead will assume unless told otherwise: 2 = self-hosted, 7 = magenta plus report, 8 = Windows only, 10 = correctness and test target that can also render.

# Architecture spine

The lead's binding positions after Stage 1 research and spikes A and B. Every decision record (0001-0009) must be consistent with this page and with `0000-owner-decisions.md`. A record may challenge a position here only by naming the evidence; the lead then changes this page.

## 1. Shape

- Engine core: a C++ library behind a small, versioned C ABI (`astroray_core.dll`), Apache-2.0. It knows nothing about Blender.
- Front ends: a thin GPL-3 Python Blender extension (loads the DLL with ctypes) and a command-line host used by tests and benchmarks.
- Windows only, NVIDIA first: CUDA for kernels, OptiX for ray traversal. The CPU target compiles the same kernel source with MSVC and uses Embree for traversal. No other vendor is built or tested; the traversal interface is the only vendor seam.
- Distributed from a self-hosted extension repository and GitHub releases. No NVIDIA binaries or SDK headers in the repository.

## 2. Kernels (spike A: `spikes/A-kernel-budget/RESULTS.md`)

- One kernel source in restricted C++ (no virtuals, exceptions or STL in kernel code; a thin host/device macro layer; `Real` switchable to double on CPU).
- 4 hero wavelengths. 8 was measured over budget (197 registers).
- Surface shading starts as ONE fused kernel (BSDF sample/eval + next-event estimation) with `__launch_bounds__`: measured 148 registers uncapped, 128 capped, and the capped fused kernel beat split kernels by about 22%. Split only when a measurement says so.
- Work that is structurally different gets its own kernel and queue: ray generation, traversal, shadow rays, volume/medium, random-walk subsurface, the extension-lobe surface stage, the light-tracing pass, geodesic stepping.
- Budgets enforced by the merge gate, per kernel: spills and stack in the shipping capped build stay within recorded allowances, and at most 3 s compile per translation unit. The uncapped register count (warning at 150, limit 168) is reported by `check full` (amended by ruling 17). Records are passed by pointer or reference, never by value across a non-inlined call. No template variant matrices; runtime flags instead.
- One GPU compiler: nvcc ahead of time (1.3-2.5 s per file measured). Runtime compilation is dropped unless the measured edit loop exceeds 5 s (ruling 16).
- CPU/GPU agreement is tested on the throughput weight f·cos/pdf, with a test build that disables FMA contraction (1.1e-6 agreement measured).

## 3. Spectral model and path state

- Radiometric quantities in physical units internally; conversion to display RGB, sensor bands or photon counts happens only at the film.
- Each path carries 4 transport wavelengths, stored and rewritable (fluorescence, redshift, gratings change them), separate from the film wavelengths the sample was generated for. Dispersion terminates secondary wavelengths.
- The wavelength sampling distribution is data, not code: visible by default, replaceable per sensor or band.
- One observer everywhere: colour-matching functions, XYZ matrix, white point and upsampling tables are generated together.
- Path state is structure-of-arrays, with a counter-based sampler keyed on (pixel, sample, dimension, seed) and no stored sampler state. Diagnostic counters (bounces, termination reason, stage time) live in the state from the first milestone.
- Rays carry position including time and a propagation vector, under a compile-time spacetime policy whose flat case costs nothing. Curved rays advance in segments tested against the ordinary acceleration structure.

## 4. Integrator

- Core: unidirectional path tracing with next-event estimation and multiple importance sampling, light tree, environment importance sampling, null-scattering volumes with spectral majorants (homogeneous and grid), random-walk subsurface, colour-aware adaptive sampling.
- A light-tracing pass is part of the first release, because gratings, prisms and caustics are unreachable otherwise (owner decision 17). Record 0004 chooses the method from published work.
- Film: per-pixel first and second moments, the full denoiser buffer set, and statistics buffers (sample count, bounce count, variance, termination reason, time).

## 5. Materials

- The common shade kernel sees one fixed OpenPBR Surface 1.1.2 parameter record.
- Any colour or refractive-index slot can be fed by a spectrum source (upsampled RGB as reflectance, as unbounded, or as illuminant; a table; an analytic form) instead of an RGB value.
- A material may carry at most one extension behaviour from a closed list (owner decision 18), evaluated in a separate stage.
- Blender node graphs are evaluated per shading point by a bytecode VM that fills the record's parameters; it never emits closures. Mix Shader selects stochastically. Unsupported nodes render magenta and are listed in a per-render report.
- The objections in `research/17-materials-and-custom-types.md` ("Review findings") are open and record 0005 must answer each.

## 6. Blender integration (spike B: `spikes/B-blender-end-to-end/RESULTS.md`)

- Python `RenderEngine`; shader graphs read only through `inline_shader_nodes()`.
- Sync: read meshes through float attributes with exact dtypes, ship raw topology to native code and triangulate there (measured 3x faster than loop triangles; first frame for 5M triangles 1.42 s against Cycles' 1.28 s). Never `loop_triangles`. Key meshes by the original datablock's `session_uid`. Treat `depsgraph.updates` as noisy and deduplicate.
- Present: one persistent texture, a persistent storage buffer updated with half-float data, a compute-shader unpack, drawn under Blender's display transform (0.74 ms at 1080p; 60 updates per second held). Each buffer update stays under 32 MiB (a measured 10x cliff).
- Resolution: full by default; a controller lowers render resolution only when the measured update interval exceeds the floor.
- Engine output is scene-linear in the file's working space.

## 7. Astrophysics

- First target: a black hole inside an emission nebula: emission and absorption along curved rays through a volume, with line-emission tables. No scattering integrator is needed for it.
- Double-precision CPU reference first; float on the GPU for the viewport only if its measured error near the photon ring is acceptable.

## 8. Display aids

- OIDN is the default denoiser. DLSS Ray Reconstruction is an optional NVIDIA viewport mode loaded from the driver, never bundled, and never applied to final or science output.

## 9. Build, tests, process

- CMake + Ninja + MSVC, one build tree. Measured: 11 s for a 34-file kernel build.
- Test tiers with time budgets; register count and compile time are gated; reference images come only from analytic results or independent oracles.
- Benchmarks write an HTML report with graphs; each milestone ends with showcase renders.
- Rules are enforced by scripts, not hooks; a few agent roles; small task files; one page of status.

## Release structure (lead's assumption until the roadmap)

Milestones each end in something visible in Blender. "First release" contains the full breadth the owner listed (surfaces, volumes, sky and world, textures, lights, gratings with the light-tracing pass). The black hole in a nebula is the milestone immediately after, with its core hooks present from the first milestone.

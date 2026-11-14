# Integrator and path-state architecture for a spectral GPU path tracer

Date: 2026-11-14. Workstream: integrator-architecture. Tags: VERIFIED = I read the primary source (source file, official doc, paper page);
REPORTED = secondary source or forum post; UNVERIFIED = background knowledge or inference not checked this session. "ESTIMATE" marks my own
arithmetic.

## Question

What integrator and path-state architecture should Astroray 2.0 adopt: megakernel vs wavefront vs hybrid (RTX 50, OptiX 9, SER), path-state
layout with 4 hero wavelengths, material evaluation (bounded closures vs per-material kernels), how to keep kernels small, sampler, light
sampling, NEE and shadow stages, volumes, adaptive sampling, path guiding, ReSTIR for the viewport, and progressive accumulation/reset.
Which parts belong in milestone 1 (M1) and which are designed-for.

Constraints from the owner: one kernel source for CPU and GPU, physics + OpenPBR as the correctness target, fast interactive viewport, short
builds, fast tests, simple design. From v1 (recon file): shade kernels saturated at 254 registers, one translation unit took ~1800 s of a
~1870 s build, features landed switched off, adaptive sampling on a luminance metric left a blue variance floor.

## Findings

### 1. How the reference renderers are structured

**pbrt-v4 (wavefront, CUDA + OptiX).**
- Stages per depth: generate camera rays, generate ray samples, intersect closest, sample medium, handle escaped rays, handle emissive
  hits, evaluate materials/BSDFs, trace shadow rays, subsurface, update film. Queues: double-buffered ray queue, escaped, area-light hit,
  medium sample/scatter, material-eval (one queue per material type), shadow ray, subsurface. VERIFIED:
  https://pbr-book.org/4ed/Wavefront_Rendering_on_GPUs/Path_Tracer_Implementation
- Rationale: a megakernel keeps data on chip but diverges on materials/textures, must allocate registers for the worst case, and compiles
  slowly; wavefront gives each kernel its own register budget at the cost of memory bandwidth; pbrt fuses some stages to cut traffic.
  VERIFIED: https://pbr-book.org/4ed/Wavefront_Rendering_on_GPUs/Mapping_Path_Tracing_to_the_GPU
- Each material type has its own kernel; materials are split into "basic" and "universal" queues by texture-evaluator complexity
  (VERIFIED, same Path_Tracer_Implementation page).
- Per-path data: PixelSampleState (pixel, L, lambda, filterWeight, cameraRayWeight, RaySamples) plus RayWorkItem (ray, depth, lambda,
  pixelIndex, beta, r_u, r_l, prevIntrCtx, etaScale, flags). SampledSpectrum is 4 floats. VERIFIED:
  https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/wavefront/workitems.h and
  https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/util/spectrum.h (`NSpectrumSamples = 4`, `lambda, pdf` arrays,
  `TerminateSecondary()`, 360-830 nm).
- Active samples capped at 1024x1024 = 1,048,576; book text: each active sample needs "roughly 1,000 bytes of additional storage for its
  state variables", and queues are sized for one work item per active sample, so queue storage is on top of the ~1,000 B (VERIFIED,
  corrected in the verification pass). The cap is a hard-coded constant with a TODO to base it on GPU memory (VERIFIED, wavefront/integrator.cpp).
- Same stage code on CPU and GPU: `ParallelFor`/`Do` dispatch to `GPUParallelFor` or the CPU `ParallelFor` on `Options->useGPU`; the GPU
  build is behind `PBRT_BUILD_GPU_RENDERER`. VERIFIED: https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/wavefront/integrator.h
- Only "path"/"volpath" exist in the wavefront integrator; any other integrator name falls back to volpath with a warning (VERIFIED,
  integrator.cpp).
- Sample dimensions are assigned explicitly: 6 for camera + wavelengths, then 7 per depth (3 BSDF, 1 light choice, 2 light position, 1
  Russian roulette), +2 with media (VERIFIED, book).

**Cycles (wavefront since 3.0, CUDA/OptiX/HIP/HIPRT/Metal/oneAPI).**
- Release notes 3.0: "GPU kernels and scheduling were rewritten ... between 2x and 8x faster in real-world scenes." VERIFIED:
  https://developer.blender.org/docs/release_notes/3.0/cycles/ (the page does not itself use the word wavefront).
- GPU kernel list has no megakernel: init_from_camera, intersect_{closest,shadow,subsurface, volume_stack,dedicated_light,mnee},
  shade_{background,light_nee,light_forward,shadow,surface, surface_raytrace,volume,volume_ray_marching,dedicated_light}, plus
  queue/sort/compact kernels. VERIFIED: https://raw.githubusercontent.com/blender/blender/main/intern/cycles/kernel/device/gpu/kernel.h
- Scheduling: the host runs the kernel with the most queued paths; refills camera paths when active paths fall below max_num_paths/4;
  compacts main paths (only when >=32 slots) and shadow paths (when used slots >= 2x active); only SHADE_SURFACE and SHADE_SURFACE_RAYTRACE
  are sorted, by shader, using a partitioned counting sort. VERIFIED:
  https://raw.githubusercontent.com/blender/blender/main/intern/cycles/integrator/path_trace_work_gpu.cpp
- State layout: `state_template.h` is the single declaration; GPU expands it to SoA (one global array per member, `IntegratorState` is an
  int index), CPU to an AoS struct; every member carries a `KERNEL_FEATURE_*` tag so unused members are not allocated; `path_trace_work_gpu`
  sizes the path budget from the sum of enabled member sizes. VERIFIED: state.h, state_template.h, path_trace_work_gpu.cpp (same
  directory/URL family as above).
- Path struct (29 fields): pixel index, sample, rng_pixel, rng_offset, eight uint16 bounce counters, queued_kernel, visibility, flag, MIS
  pdfs, throughput (a 3-float spectrum), continuation probability, shader_sort_key. Ray (8), isect (6), volume stack, guiding (8, only with
  path guiding), shadow state separate (`shadow_state_template.h`). VERIFIED: state_template.h.
- A shadow ray is a separate lightweight "shadow path" state created by shade_surface; it copies throughput x BSDF value (the unoccluded
  contribution), pixel/rng/bounce counters, volume stack, and the MIS weight is already folded in (`light_sample_mis_weight_nee` inside the
  shade kernel). A non-constant light shader goes through an extra shade_light_nee kernel. VERIFIED: shade_surface.h.
- Register policy: kernels declared with `ccl_gpu_kernel(GPU_KERNEL_BLOCK_NUM_THREADS, GPU_KERNEL_MAX_REGISTERS)`; for every CUDA
  arch from 700 to 1299 (7.x through 12.x, including sm_120) the cap is 168 registers, block 384 (older archs 48-64; above 1299 is an
  #error). `ccl_device` is force-inline with the comment "Function calls are
  bad for OptiX performance, so inline everything." VERIFIED: kernel/device/cuda/config.h, optix/compat.h.
- OptiX backend: only the five intersection kernels are OptiX raygen programs; no miss/anyhit/ closesthit programs; no `optixReorder` /
  hit objects. All shading is ordinary CUDA kernels. VERIFIED:
  https://raw.githubusercontent.com/blender/blender/main/intern/cycles/kernel/device/optix/kernel.cu A NVIDIA forum poster says Blender "has
  a mega kernel" (REPORTED: https://forums.developer.nvidia.com/t/practical-way-to-use-ser-on-principled-shader/379995); the kernel list
  above contradicts that for the GPU integrator.
- Shading: one `integrator_shade_surface` evaluates the shader graph (`surface_shader_eval`, templated on a node feature mask), builds
  closures (`MAX_CLOSURE 64`), does emission, direct light, AO and the BSDF bounce. Rare feature (ray-trace nodes) lives in a second kernel
  variant. VERIFIED: shade_surface.h, kernel/types.h.
- Counter-based sampling: `path_rng_1D..4D(sample, rng_pixel, dimension)` over Sobol-Burley; blue-noise dithered modes added in 4.2
  ("FIRST" mode uses a separate 1-spp sequence "so that the viewport has blue-noise behaviour during 1-SPP navigation"). VERIFIED:
  https://raw.githubusercontent.com/blender/blender/main/intern/cycles/kernel/sample/pattern.h and
  https://developer.blender.org/docs/release_notes/4.2/cycles/
- Cycles is not spectral. 5.3 Principled dispersion draws one random wavelength value per path from (pixel, sample) on demand
  (`shader_setup_wavelength`, flag `SD_REQUIRES_WAVELENGTH`), not a hero set. VERIFIED: kernel/geom/shader_data.h. The press calls it
  "RGB-based" (REPORTED: https://80.lv/articles/principled-bsdf-in-blender-s-cycles-now-supports-dispersion).

**Mitsuba 3 / Dr.Jit.**
- Dr.Jit loops default to symbolic mode, which traces the loop once into "one large fused kernel, which the page calls a megakernel";
  evaluated mode runs every iteration explicitly and costs memory bandwidth. VERIFIED: https://drjit.readthedocs.io/en/latest/cflow.html
- The Mitsuba docs nevertheless describe the `cuda` backend as "a wavefront path tracer that delegates ray tracing to NVIDIA OptiX"
  (VERIFIED, https://mitsuba.readthedocs.io/en/stable/src/key_topics/variants.html); the two pages disagree (see Unverified).
- `dr.reorder_threads()` (SER) arrived in Dr.Jit 1.1.0, 7 Aug 2025; OptiX backend needs the OptiX 8 ABI. VERIFIED:
  https://drjit.readthedocs.io/en/latest/changelog.html
- The `path` integrator is one `dr::while_loop` over a small LoopState (ray, throughput, result, eta, active, sampler state); 2D/1D
  sampler draws in the order emitter, BSDF, RR; the first bounce is traced `coherent=true`, later ones `coherent=false` with reordering.
  VERIFIED: https://raw.githubusercontent.com/mitsuba-renderer/mitsuba3/master/src/integrators/path.cpp
- Polarized variants cost "roughly 1.5 to 2 times the rendering time" (VERIFIED, variants page).

**NVIDIA Falcor / RTXPT (real-time, megakernel + SER).**
- RTXPT: "a single path tracing shader evaluates all light transport"; HLSL (not Slang as far as the README says); SER on hits, switchable
  with `--noSER`; one DispatchRays thread per pixel loops over bounces with inline RayQuery for scatter rays; Owen-scrambled low-discrepancy
  sampler ("Practical Hash-based Owen Scrambling"); NEE-AT feedback light sampler (up to 2M lights); SHaRC radiance cache and DLSS-RR;
  ReSTIR DI/GI only as a fallback when DLSS-RR is off. VERIFIED: https://github.com/NVIDIA-RTX/RTXPT and .../Docs/Design.md
- Falcor PathTracer: raygen loop over path segments, shading in hit shaders; emissive-mesh light BVH with MIS for environment and emissive
  meshes. VERIFIED: https://raw.githubusercontent.com/NVIDIAGameWorks/Falcor/master/docs/usage/path-tracer.md Falcor 7.0 release notes list
  SER support (REPORTED via search; not opened).
- Both are RGB renderers tuned for 1 sample/pixel and a denoiser; neither carries 4 spectral samples. (RTXPT's README is silent on colour
  model: UNVERIFIED.)

### 2. SER, megakernel vs wavefront evidence

- SER = `optixTraverse` + `optixReorder(hint)` + `optixInvoke`; works only from OptiX raygen programs. A forum reply by "dhart" (signed David; employer not stated on the page, probably NVIDIA): reorder cost is
  proportional to live state (variables used both before and after the reorder); RNG decisions must be made before the reorder;
  re-materialise cheap values. REPORTED (NVIDIA staff on the forum thread above). The OptiX 9 SER chapter itself I could not read (guide PDF
  is compressed; HTML needs JavaScript).
- Blackwell: "SER throughput in the SM has also doubled" (REPORTED via HotHardware/Tom's, NVIDIA marketing claim; a Czech review says the
  measurement is unclear). Real gains reported: 47.78% on Khronos's vk_gltf_renderer with a single closest-hit ubershader and no hints (the
  article calls it "not the ideal scenario"), 3.7x for ReSTIR GI in Black Myth Wukong, 11% then 24% in Indiana Jones. VERIFIED (read):
  https://www.khronos.org/blog/boosting-ray-tracing-performance-with-shader-execution-reordering-introducing-vk-ext-ray-tracing-invocation-reorder
- SER is no longer NVIDIA-only: VK_EXT_ray_tracing_invocation_reorder is hardware-accelerated on RTX 40/50 and Intel Arc B-series; AMD
  "planned"; `reorderThreadEXT` only in ray generation shaders, inline ray query reorder unspecified. VERIFIED: same Khronos blog.
- OptiX 9.1 + SER + OptiX-IR + HAGS crash (CUDA 719), PTX stable: single user report. REPORTED:
  https://forums.developer.nvidia.com/t/optix-9-1-ser-optix-ir-crashes-with-hags-enabled-cuda-719-ptx-appears-stable/387608
- Only direct wavefront vs megakernel measurement I found: arXiv 2605.27323, one scene (A Beautiful Game), RTX 3060 Ti, Vulkan + Slang +
  inline ray query + OpenPBR: wavefront 13.58 ms vs megakernel 15.47 ms (about 14%, the paper says 16%), VRAM throughput 41% vs 19%; SER not
  evaluated; neither method saturated any unit. VERIFIED (read): https://arxiv.org/html/2605.27323 (preprint; weak evidence).
- No published comparison of SER-megakernel vs wavefront on a spectral, many-material path tracer.

### 3. Spectral path state and MIS

- Hero wavelength sampling: one random wavelength per path, others spaced equally; Wilkie et al. EGSR 2014. VERIFIED (abstract):
  https://cgg.mff.cuni.cz/~wilkie/Website/EGSR_14.html
- pbrt volpath: sampling uses wavelength 0 but PDFs are evaluated at all wavelengths; state is `beta`, `r_u`, `r_l` (rescaled path
  probability ratios, each a 4-spectrum); estimator divides by the wavelength-average; spectral MIS gives 149x lower MSE on the chromatic
  test scene; null events do not count toward depth. VERIFIED:
  https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/Volume_Scattering_Integrators
- `TerminateSecondary()` zeroes secondary pdfs and divides pdf[0] by 4 (dispersion, VERIFIED spectrum.h).
- Cycles and RTXPT do not need r_u/r_l because they carry RGB; v1's recon notes say JH upsampling is nonlinear in magnitude (carry
  reflectance, not pre-scaled BSDF).
- ESTIMATE of a lean per-path SoA state with 4 hero wavelengths (bytes): pixel 4, sample 4, packed depth/flags/queue id/rng dimension 8,
  ray origin+direction 24, hero seed 4 (wavelengths and pdfs recomputed on demand, as Cycles recomputes its wavelength from RNG), beta 16,
  r_u 16, r_l 16, MIS context (prev pdf, packed normal, etaScale) 12, hit record (t, u, v, prim, instance) 20 = about 124 bytes; +16 if L is
  kept per path instead of accumulated into the film, +~32 for a 4-deep volume stack. Roughly 130-170 B/path vs pbrt's ~1,000 B of state variables (book; queues extra). 1080p x 1 spp = 2.07M paths = ~270-350 MB. I did not measure any of this.

### 4. Light sampling, MIS, environment

- pbrt BVHLightSampler: infinite lights kept apart, chosen with probability nInf/(nInf+1) when a BVH exists; BVH importance uses cone
  bounds, distance and orientation; traversal is a stack-free while loop; PMF recomputed from a 32-bit bit trail; Uniform and Power (alias
  table) samplers ignore the shading context. Interface is `Sample(ctx,u)` / `PMF(ctx,light)`. VERIFIED:
  https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/lightsamplers.h
- Cycles light tree follows Conty Estevez and Kulla, uses max and min importance, bit trail for the MIS pdf, stack-free descent; written
  in shared kernel macros. Default on since 3.5; the 3.5 notes say it was disabled on AMD GPUs then. VERIFIED: kernel/light/tree.h and
  https://developer.blender.org/docs/release_notes/3.5/cycles/
- Light-tree energy and pbrt's `phi` are scalars. For physically spectral emitters outside the visible range a scalar power is band-blind.
  Inference, UNVERIFIED.
- pbrt ImageInfiniteLight: equal-area (square) map, `PiecewiseConstant2D`, and a second "compensated" distribution (average subtracted,
  floored at 0) used when `allowIncompletePDF`. VERIFIED: https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/lights.cpp
  Technique: MIS compensation, Karlik et al. SIGGRAPH Asia 2019, 2.75x lower NMSE on an environment-map test, "subtracting a constant from a
  tabulated sampling density". VERIFIED (abstract): https://cgg.mff.cuni.cz/~jaroslav/papers/2019-mis-compensation pbrt's image light
  requires RGB channels (spectral upsample), not a spectral map.
- Shadow rays: in both pbrt and Cycles the shade stage computes the unoccluded contribution (light sample + BSDF value + MIS weight) and
  enqueues a shadow item; a later stage only traces visibility and adds the result. pbrt `ShadowRayWorkItem` = ray, tMax, lambda, Ld, r_u,
  r_l, pixel. VERIFIED: workitems.h, shade_surface.h.

### 5. Samplers

- pbrt default sampler: "zsobol" with "fastowen" randomization; samplers are selected by name (zsobol, paddedsobol, sobol, halton,
  pmj02bn, independent, stratified). VERIFIED: scene.cpp and samplers.cpp (raw URLs under mmp/pbrt-v4/master/src/pbrt/). ZSobol needs the
  sample count up front (power of two) - from my knowledge of its Morton indexing, UNVERIFIED here.
- Cycles Sobol-Burley: stateless, pure function of (sample, pixel hash, dimension); needs 12 bytes of state per path (sample, rng_pixel,
  dimension). The per-pixel `sample_is_class_A` parity split exists so variance can still be estimated from correlated Owen-scrambled
  samples. VERIFIED: pattern.h.
- Blue-noise error distribution across pixels: Heitz and Belcour 2019 (seed permutation) and Cycles' dithered variant (Owen scrambling of
  Morton index per Psychopath's post). Cycles: "mainly at low maximum sample counts and in interactive viewport renders". VERIFIED: 4.2
  notes, pattern.h; Heitz-Belcour REPORTED (search result abstract): https://diglib.eg.org/handle/10.1111/cgf13778
- Practical hash-based Owen scrambling (Burley, JCGT 2020) is used by RTXPT (VERIFIED README) and is the origin of Cycles' `sobol_burley`
  (name only; UNVERIFIED that code is Burley's).

### 6. Volumes

- Cycles 5.0: null-scattering volume algorithm is the default ("unbiased, no step size / max steps / homogeneous settings"); the old
  biased ray marching remains as an option; NanoVDB for smoke/fire. VERIFIED: https://developer.blender.org/docs/release_notes/5.0/cycles/
  and kernel list (shade_volume vs shade_volume_ray_marching).
- Theory: Miller, Georgiev, Jarosz SIGGRAPH 2019 null-scattering path integral exposes the pdfs so delta/ratio tracking techniques combine
  with MIS in spatially and spectrally varying media; spectral tracking (Kutz et al. 2017) renders all colour components together but
  samples distance from the densest component. VERIFIED (abstract): https://cs.dartmouth.edu/~wjarosz/publications/miller19null.html
- pbrt volpath details (above): event probabilities from wavelength 0 (absorb, scatter, null), `sigma_n = clamp(sigma_maj - sigma_a -
  sigma_s)`, medium emission added at tentative collisions with an `r_e` weight, ratio tracking for shadow transmittance, Russian roulette
  on `beta/avg(r_u)`. VERIFIED (Volume_Scattering_Integrators). Majorant grid + DDA for heterogeneous media: from my knowledge of pbrt's
  GridMedium, UNVERIFIED this session.

### 7. Adaptive sampling

- Cycles stop test: `error = sum_c |I_c - A_c| / (0.0001 + normalize)`, `normalize = sqrt(I_sum)` for sum<1 else `I_sum`, scaled by
  exposure/sample count; per-pixel `converged` flag in the aux buffer; the flag is dilated by one pixel in x and y; `film_need_sample_pixel`
  skips converged pixels when camera paths are scheduled. VERIFIED: kernel/film/adaptive_sampling.h.
- That formula divides a sum of absolute per-channel errors by the total intensity. My reading: a dim channel (blue under a warm scene)
  contributes little to the numerator and nothing to the normaliser, so it can stay noisy while the pixel is declared converged. This
  matches the v1 note (recon: luminance-scalar metric left a blue variance floor). INFERENCE, not tested.
- No 2025-26 source found on colour-aware stop metrics; Dammertz et al. WSCG 2010 uses colour noise relative to log luminance (REPORTED,
  search abstract).

### 8. Path guiding

- OpenPGL v0.8.0 (Apache 2.0): CPU only (SSE/AVX/AVX-512/NEON), "support for different GPU backends is planned", "still in a pre v1.0
  stage". VERIFIED: https://github.com/OpenPathGuidingLibrary/openpgl A Dec 2025 ASWF proposal lists GPU (CUDA, SYCL, HIP) as in progress
  (REPORTED via search). OpenPGL joined ASWF in April 2026 (recon, REPORTED).
- Cycles guiding exists as hooks in shade_surface (`guiding_record_surface_segment/emission/bounce`,
  `surface_shader_bsdf_guided_sample_closure`) and 8 state fields compiled only with path guiding; CPU only (Blender 3.5 manual per recon;
  not re-read). VERIFIED for the hooks (shade_surface.h, state_template.h).
- GPU research exists: "Path guiding for wavefront path tracing: a memory efficient approach for GPU path tracers" (CUDA/OptiX, 2024).
  REPORTED (search result only; not read).

### 9. ReSTIR-class methods

- NVIDIA RTXDI SDK 3.2.0 contains ReSTIR DI (original), GI (v2.0) and PT (v3.0); D3D12 or Vulkan via NVRHI; shaders are HLSL via DXC.
  VERIFIED: https://github.com/NVIDIA-RTX/RTXDI It is not a CUDA/OptiX library, so a CUDA-based engine reimplements it. License text not
  read.
- ReSTIR PT = Generalized RIS with shift mappings (Lin et al., SIGGRAPH 2022); reuse correlates samples and loses RIS convergence
  guarantees unless modified (VERIFIED abstract: https://research.nvidia.com/labs/rtr/publication/lin2022generalized). ReSTIR BDPT (2025),
  PG (SIGGRAPH Asia 2025): recon, REPORTED.
- Spectral ReSTIR: I found no peer-reviewed treatment of hero wavelengths with reservoir reuse (searched three times). Weaker traces
  exist: a Blender devtalk post and a Waseda thesis (both only seen as search snippets, not read) say resampling must be extended over the
  wavelength dimension and call it open; the closed alpha "Spectra Render" claims it (recon, REPORTED). So compatibility is UNVERIFIED
  research territory; my reasoning is in Options.
- RTXPT uses ReSTIR only as a fallback and prefers NEE-AT + SHaRC + DLSS-RR (VERIFIED README/Design).

### 10. Viewport and progressive use

- Cycles viewport: 3.0 notes say it became "significantly more responsive due to new scheduling and display algorithms"; 5.3 pauses
  viewport renders during final renders. VERIFIED (notes above and https://developer.blender.org/docs/release_notes/5.3/cycles/). The 5.3
  DLSS denoiser is Cycles-internal (recon).
- Cycles refill policy (init new camera paths when active < 1/4 of max) keeps occupancy up in the tail without a global barrier (VERIFIED,
  path_trace_work_gpu.cpp).
- Windows watchdog (TDR, ~2 s default) limits single kernel runtime under WDDM: background knowledge, UNVERIFIED here.

## Options and tradeoffs

The real forks, not a fixed count.

**A. Execution model: where does the loop live?**
1. Megakernel (+SER). One function loops over bounces, state in registers. Pros: least code, no queues, no per-bounce state traffic, one
   launch per sample, fastest 1-spp latency if registers fit, SER can regain coherence (RTXPT, Falcor, Mitsuba default). Cons: v1's own
   history is the case against it (one TU dominated the build; REG 254); registers sized by the worst material/feature; the 4-wide spectral
   live state (beta, r_u, r_l = 48 B plus ray) is exactly what SER must migrate; SER needs OptiX raygen (so the whole integrator lives in an
   OptiX module, the SER-IR-HAGS crash report applies) or Vulkan RT pipelines; a CPU path needs the same loop (simple); long launches vs
   TDR; reset latency = one full launch.
2. Wavefront with SoA state and queues (pbrt, Cycles). Pros: per-stage register budgets (Cycles caps 168), per-stage translation units
   (build parallelism, and a one-stage edit rebuilds one TU), natural place for shader sorting, short launches (cancel/reset at kernel
   boundaries), sorting coherent shading without SER, easy to unit-test a stage on the CPU against fixtures. Cons: state traffic (ESTIMATE
   of tens of GB/s of effective traffic at 1080p, unmeasured), more host scheduling code, launch overhead and tail occupancy at low spp.
3. Wavefront across bounces, fused inside a bounce (hybrid). Stages are plain functions over a state accessor; a kernel is a composition
   of stages. pbrt and Cycles already fuse some stages. The experiment is how much to fuse (e.g. intersect+shade for primary hits from a
   V-buffer, or light sample + BSDF eval + shadow-ray emit), constrained by registers and traffic. This is not a separate architecture so
   much as the knob that the stage-function design leaves free. The axis the evidence does not settle: megakernel+SER vs sorted wavefront on
   a spectral, many-material workload. Published numbers are one preprint and games.

**B. Material evaluation**
- Per-material-type kernels (pbrt). Needed there because pbrt has many material classes. With a single OpenPBR uber-material the type axis
  collapses; the variation is in the parameter graph, not the lobes.
- Interpreter for the parameter graph, then a fixed BSDF (Cycles SVM + closures; v1's op-VM). Sorted by shader ID so interpretation stays
  coherent. Cost: interpreter registers and a parameter record in memory.
- Generated per-material code (JIT/NVRTC). Best per-material speed, but runtime compile, cache and one-source concerns; belongs to the
  materials workstream.
- Closure arrays (Cycles `MAX_CLOSURE 64`) vs a fixed lobe struct. A fixed OpenPBR lobe set (fuzz, coat, specular, metal, diffuse,
  translucent/transmission, subsurface) is bounded, so no closure array and no dynamic lobe count: this is the "bounded per-material state"
  lever that v1 lacked.

**C. NEE shape and kernel size**
- Fused shade (pbrt, Cycles): material eval + light sample + BSDF eval + BSDF sample + shadow item in one kernel. Simple, but that is v1's
  REG:254 pattern (NEE block and spectral BSDF each ~160 registers on a ~95 base, recon).
- Split: surface setup writes a compact shading record; light-sample stage; BSDF stage. More traffic, smaller kernels. A middle form moves
  only the light-tree traversal out.

**D. Sampler**: independent hash RNG (no stratification), Owen-scrambled Sobol (pbrt zsobol, Cycles, RTXPT), plus blue-noise dithering
(Cycles 4.2). Real constraint: progressive/adaptive/viewport use needs an unbounded, per-pixel-indexed sequence, which rules out samplers
that fix spp up front.

**E. Light sampling**: uniform/power alias (O(1), no spatial awareness) vs light tree (spatial, needs bit-trail PMF, GPU traversal) vs
feedback samplers (NEE-AT) vs RIS/ReSTIR. Interface must expose `PMF(ctx, light)` for MIS from BSDF-hit emitters.

**F. Interactive denoising/reuse**: plain progressive + denoiser (OIDN/OptiX) vs ReSTIR DI vs ReSTIR PT + cache. Spectral reuse needs a
target function decision (below).

**G. Hardware abstraction**: the stage design needs from a backend: hardware traversal from a compute-like stage (OptiX raygen, or Vulkan
ray query), atomics, prefix sums, indirect launches. Cycles shows the stage code can span CUDA, OptiX, HIP, HIPRT, Metal and oneAPI (macro
layer, VERIFIED from kernel.h/compat headers). SER hardware exists on NVIDIA and Intel, planned AMD (Khronos blog). The GPU-backend
workstream decides NVIDIA-only vs neutral; this design works with either if traversal is an interface (`intersect_closest/shadow` queues).

## Recommendation for Astroray 2.0

R1. **Stage-function wavefront, written once.** Every stage (generate, intersect, shade pieces, shadow, medium, film) is a plain function
over a state accessor, as in Cycles (`INTEGRATOR_STATE` macros over one template) and pbrt (same stage lambdas on CPU via `ParallelFor`).
GPU launches each stage over a compacted queue; the CPU oracle runs the same functions. M1 ships wavefront; fusion and a megakernel
composition stay available because stages are functions. Evidence: pbrt and Cycles are the two production-scale spectral/PBR wavefront
designs with source (VERIFIED); v1's costs (register saturation, a 30-minute TU) are the documented failure of the fused shade kernel
(recon); Cycles' 168-register cap and per-stage launch bounds are VERIFIED. Honest caveat: the megakernel+SER alternative is unmeasured on
this workload, so the spike (S2) can overturn the default; it must be run before M1 code is frozen.

R2. **Path state.** SoA, ~130-170 B per path (ESTIMATE): pixel, sample, packed flags/depth, ray, hero seed (wavelengths recomputed), `beta`,
`r_u`, `r_l` as 4-wide spectra (pbrt formulation, so dispersion, chromatic media and spectral MIS need no special cases later), MIS context,
hit record. Counter-based RNG, so no sampler state. Film accumulation by atomic add from the shadow/emission stages instead of a per-path L.
Shadow items are separate small records (about 100 B). Use feature-tagged state members (Cycles) so volumes, subsurface and guiding cost
nothing when absent. Make the width a compile-time constant `N = 4` (pbrt `NSpectrumSamples`), not baked into code. Evidence: pbrt
workitems.h, Cycles state_template.h (VERIFIED). Size figures are my arithmetic.

R3. **Materials: one bounded OpenPBR lobe set, two stages.** Stage 1 evaluates the parameter graph (textures, procedural nodes) to a fixed
`MaterialParams` record, sorted by shader ID (Cycles' sort: partitioned counting sort, only on the shade kernels). Stage 2 is one BSDF
kernel with fixed lobes, used for eval, pdf and sample, with no per-material variants. No template matrix over (lobe, texture, phase, ...) -
runtime flags only (the 128-variant lesson). Whether stage 1 is an interpreter or generated code belongs to the materials workstream; the
integrator only needs the record boundary. A record this size is traffic, so S3 measures whether stage 1 and 2 should fuse.

R4. **Keep kernels small by construction.** Rules: (a) one stage per translation unit; (b) per-kernel `__launch_bounds__` with a
max-register cap (start at Cycles' 168 on sm_120) and a CI check on `-Xptxas -v` that fails on spills; (c) rare features (subsurface,
volumes, hair, geodesic stepping, ray-trace nodes) get their own stage or kernel variant, as Cycles does with `shade_surface_raytrace`,
`shade_volume`; (d) feature masks decide which state members exist; (e) force-inline device functions in OptiX modules (Cycles comment,
VERIFIED); (f) no pointers or fat structs passed by value on the hot path (v1 lesson, recon). Fusing two stages is allowed only with a
measured register and bandwidth win (S1, S3).

R5. **Sampler.** Owen-scrambled Sobol keyed by (pixel hash, sample index, dimension), stateless and unbounded in sample count, with a
Cycles-style blue-noise dithered mode for 1-4 spp and the viewport. Dimension map explicit and short per bounce (pbrt: 7 per depth),
wavelength dims first. Do not use a sampler that needs spp fixed up front (pbrt zsobol). Keep the class A/B parity split for variance
estimates. Evidence: pattern.h, 4.2 notes, scene.cpp (VERIFIED).

R6. **Light sampling.** M1: `LightSampler` interface with `Sample(ctx,u)` and `PMF(ctx,light)` from day one; implementations: power alias
table (all lights) and equal-area octahedral environment map with a compensated PDF. M2: light tree (pbrt/Cycles stack-free traversal +
32-bit trail); it must be on by default when it lands. Emitter "power" for tree importance is defined per sensor band, not luminance, so
astro emitters outside the visible range still get sampled. Whether the tree must be in M1 is an owner call (Blender users expect it since
3.5): see Open questions. MIS: pbrt's `r_u`, `r_l` balance heuristic with wavelength averaging.

R7. **NEE and shadow stage.** Shade computes light sample, BSDF value and MIS weight, writes a shadow item with the unoccluded contribution;
the shadow stage uses a terminate-on-first-hit traversal and adds to the film. The only M1 transparency in shadows is
alpha-tested/stochastic opacity (no coloured transparent shadows); anything else raises a visible warning (rule 7). Evidence: pbrt, Cycles
shade_surface (VERIFIED).

R8. **Volumes.** The state carries `r_u`, `r_l` and a medium stack slot from M1, and the queue set includes a medium stage. M1: homogeneous
media with spectral delta tracking, null events not counting toward depth, ratio-tracked shadows, per pbrt's volpath formulation. M2:
heterogeneous grids with a majorant grid and DDA (NanoVDB as in Cycles 5.0). M3/astro: emission-dominated, optically thin media with
analytic per-segment integration and spectral emission tables; the design-for hook is a separate "medium advance" stage that can replace
delta tracking, and a ray type whose advance is a geodesic step (a distinct intersect stage, like Cycles' intersect_* variants). Evidence:
pbrt volpath, Cycles 5.0 default (VERIFIED).

R9. **Adaptive sampling.** The path-init stage skips pixels whose `converged` flag is set (Cycles `film_need_sample_pixel`). Colour-aware
metric: per-output-channel error from the A/B split, `e_c = |A_c - B_c| / (sqrt-floor(mean_c) + eps_c)`, converged only when `max_c e_c <
threshold` (channel = output colour component or sensor band), plus Cycles' one-pixel dilation. Not Cycles' sum-over-total-intensity form
(see Findings 7). Landed in M1 only with a test that renders a blue-dominant chromatic-light scene and shows the per-channel metric fixes
the floor.

R10. **Guiding: designed-for, deferred.** OpenPGL is CPU-only and pre-1.0; v1's gain was capped by scene physics. Reserve (a) a "guided pdf"
slot in the BSDF-sample stage and (b) an optional record-segment hook; no state members until the feature exists (feature-tagged members).

R11. **ReSTIR: designed-for, deferred.** A viewport-only second integrator mode, in this order: ReSTIR DI on the primary hit (one extra
stage, screen-space reservoirs), then GI, then PT. Reservoirs hold light/path identity, not radiance, so temporal reuse across frames with
different hero wavelengths is possible. To stay unbiased with spectral output the target function must be positive wherever the contribution
is non-zero in any recorded band: use a band-summed scalar target (not luminance only) and re-evaluate `f` at the current frame's
wavelengths for the final estimate. This is my reasoning, UNVERIFIED; the spike S10 tests it against a reference with IR and UV emitters.
RTXDI is HLSL/D3D12/Vulkan, so we reimplement; we do not depend on it.

R12. **Progressive accumulation and reset.** Film = float32 running sum + per-pixel sample count (XYZ or band channels). One epoch counter:
any scene/camera edit bumps the epoch and clears; stages check the epoch at launch boundaries so cancellation latency is one kernel
(milliseconds), and no launch ever runs long enough for TDR. Viewport: time-budgeted passes of N spp per present, a navigation resolution
divisor, blue-noise FIRST-style sequence for the first sample, denoise only a display copy, accumulate the raw buffer. Persistent
GPU-resident film; host copy only for presentation. Refill camera paths when active < 1/4 of capacity (Cycles) instead of per-bounce
barriers.

### Milestone split

| Feature | M1 | Designed-for, deferred |
|---|---|---|
| Wavefront stage functions, SoA state, CPU+GPU same stages | yes | megakernel composition (decided by S2) |
| 4 hero wavelengths, r_u/r_l spectral MIS, dispersion termination hook | yes | arbitrary band sets, sensor-weighted wavelength sampling, polarization (state templated on spectrum type; Mitsuba pays 1.5-2x) |
| OpenPBR bounded lobes, 2-stage material eval, shader sort | yes | JIT per-material code |
| Owen-Sobol sampler + blue-noise dither, counter-based RNG | yes | - |
| Power alias lights + compensated environment map | yes | light tree (M2), feedback samplers |
| NEE + shadow stage, opaque/alpha shadows | yes | coloured transparent shadows, light linking |
| Homogeneous media | yes | heterogeneous (M2), emission-absorption astro volumes, geodesic ray stage |
| Per-channel adaptive sampling | yes (gated by test) | denoiser-aware sampling |
| Progressive viewport, epoch reset, refill scheduler | yes | ReSTIR DI/GI/PT mode, radiance cache |
| Path guiding | no | guided-pdf slot and record hook |
| SER | no (measure only) | intersect-stage or fused-stage SER if S2 wins |

## What a measurement spike must settle

All on the RTX 5070 Ti, minimal prototype with 4-wide spectral state, OpenPBR-lite lobes, one light sampler, three scenes (Cornell-like
diffuse; textured many-material with 1/8/64 shaders; chromatic glass + a homogeneous medium). Record the command and GPU clock state for
every number.

- S1 Register/spill/compile map. `-Xptxas -v` and `--fdevice-time-trace` per stage for: fused shade vs split shade, with launch-bound caps
  128/168/255. Pass bar: no spills at 168, incremental single-stage rebuild under 30 s, clean build under the 5 min budget.
- S2 Megakernel vs wavefront on identical stage functions, SER on/off (OptiX raygen with `optixReorder`, PTX and OptiX-IR, HAGS on/off to
  reproduce or kill the crash), at 1 spp 1080p latency and 64 spp throughput. Decides R1.
- S3 State traffic: DRAM throughput per frame in Nsight Compute vs the ESTIMATE; test hero seed vs stored lambdas, fp16 beta/r_u/r_l, film
  atomics vs per-path L, record fusion.
- S4 Sorting: shade stage with and without shader sort at 1, 8, 64 shaders; sort cost; compare to SER in the intersect stage.
- S5 Launch overhead and tail: kernels per frame, CUDA Graph vs direct launch, idle gaps, refill threshold (1/4 vs others), cancel latency
  from edit to first new frame.
- S6 Sampler and adaptive: MSE vs reference at 1/4/16/64 spp for Owen-Sobol vs blue-noise dither; per-channel vs Cycles-form stop metric
  on a blue-dominant scene, report residual per-channel variance at stop.
- S7 CPU same-source cost: CPU SoA wavefront vs per-path execution of the same stages; fast-test budget (pre-commit under 10 s, merge gate
  under 3 min) on a 64x64 furnace and one chi-squared test.
- S8 Light sampling: power alias vs light tree at 10 / 1k / 100k emissive triangles, noise at equal time, tree registers, and whether the
  per-band power definition changes selection for IR sources.
- S9 Spectral MIS cost: 4-wide `r_u/r_l` vs scalar on surface-only scenes (is the generality free?).
- S10 Spectral ReSTIR DI: band-summed target vs luminance target against a long reference with UV/IR and chromatic lights; check bias
  numerically, not by eye alone.
- S11 Interactive latency end to end through Blender (depends on the engine-integration workstream).

## Unverified or conflicting

- Mitsuba 3: Dr.Jit docs (symbolic loops = megakernel, default) vs Mitsuba docs ("cuda backend is a wavefront path tracer"). Probably
  stale wording in one; not resolved.
- A NVIDIA forum poster says Cycles uses a mega kernel; the Cycles GPU kernel list shows a wavefront integrator. I side with the source.
  Cycles' "shade_surface" is, however, one large per-shader kernel with a node interpreter, which may be what the poster meant.
- SER on Blackwell "2x": marketing claim, measurement unclear. Real-game gains are workload dependent. I did not read the OptiX 9 SER
  chapter or whitepaper text.
- The OptiX-IR + SER + HAGS crash is a single forum report; unconfirmed fix status.
- The only wavefront-vs-megakernel measurement is a single-scene preprint on a 3060 Ti without SER.
- Cycles sort conditions/thresholds and the CPU execution loop were not read (I read the GPU scheduler only). Cycles volume stack size
  value not seen. Cycles per-path byte size not stated anywhere I read. pbrt's ~1,000 B/path is book text, not computed in code.
- Light tree status on GPU backends in Cycles main: only the undated meeting note (recon) and the 3.5 notes (AMD disabled then). Spectral
  light-tree importance is my inference.
- Whether CUDA (non-OptiX) kernels can trace against an OptiX acceleration structure directly: not verified; Cycles' split (OptiX raygen
  for intersect, CUDA for shading) is the evidence.
- No published spectral ReSTIR; R11's target-function argument is untested. RTXDI license not read.
- OpenPGL GPU timeline unknown; the 2024 wavefront-guiding paper not read.
- Cycles 5.3 dispersion described as RGB-based by the press; source shows a per-path random wavelength value. How that value becomes a
  wavelength was not visible.
- Windows TDR limit, RTX 5070 Ti memory bandwidth and Burley's authorship of `sobol_burley` are from background knowledge, not checked.
- ESTIMATE bytes/path and traffic figures are my arithmetic, not measurements.

## Verification pass

Independent check, 2026-11-14. Recommendation R1-R12 unchanged: it never depended on the corrected points, and it already labels
megakernel+SER vs wavefront as unmeasured (spike S2).

- CONFIRMED: pbrt-v4 `ParallelFor`/`Do` dispatch on `Options->useGPU` to `GPUParallelFor` or CPU `ParallelFor`; GPU branch behind
  `PBRT_BUILD_GPU_RENDERER`. https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/wavefront/integrator.h
- CONFIRMED: `NSpectrumSamples = 4`; `TerminateSecondary()` zeroes pdf[1..3] and divides pdf[0] by 4.
  https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/util/spectrum.h
- CORRECTED: the book's ~1,000 bytes is "additional storage for its state variables" per active sample; queues (one item per active
  sample) are extra, not included. https://pbr-book.org/4ed/Wavefront_Rendering_on_GPUs/Path_Tracer_Implementation
- CONFIRMED: Cycles GPU kernel list has no megakernel (31 integrator kernels, separate intersect/shade/sort/compact).
  https://raw.githubusercontent.com/blender/blender/main/intern/cycles/kernel/device/gpu/kernel.h
- CONFIRMED: OptiX backend defines exactly five `__raygen__` intersect programs; no miss/anyhit/closesthit; no optixReorder/Traverse
  in that file (headers not shown). https://raw.githubusercontent.com/blender/blender/main/intern/cycles/kernel/device/optix/kernel.cu
- CORRECTED: the 168-register / 384-thread config covers `__CUDA_ARCH__` 700-1299 as one range, not only 7.x/8.x/12.x (it also
  covers 9.x/10.x); sm_120 is included. https://raw.githubusercontent.com/blender/blender/main/intern/cycles/kernel/device/cuda/config.h
- COULD NOT VERIFY (not re-read): state_template.h SoA/AoS expansion, only shade_surface sorted, path_trace_work_gpu.cpp scheduling.
  Kernel list is consistent with sorting being limited to the `sorted_paths`/`sort_*` kernels.
- CONFIRMED: arXiv 2605.27323 (Padilla, Webster, Kim): RTX 3060 Ti, "A Beautiful Game", megakernel 15.47 ms vs wavefront 13.58 ms
  (about 14% faster by throughput; paper says approximately 16%); SER not mentioned. https://arxiv.org/html/2605.27323
- CONFIRMED (with caveat on author): forum reply on live state migration cost comes from "dhart"/David, employer unstated on the page.
  Tag stays REPORTED. The thread's Blender "mega kernel" remark is the original poster's. https://forums.developer.nvidia.com/t/practical-way-to-use-ser-on-principled-shader/379995
- CONFIRMED: pbrt volpath samples distance at wavelength 0, evaluates PDFs at all wavelengths via r_u/r_l, and Figure 14.7 caption
  reports MSE reduced by a factor of 149 (read only the first 100k of 250k characters; the figure was in that span).
  https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/Volume_Scattering_Integrators
- CONFIRMED: OpenPGL v0.8.0 CPU-only, GPU backends "planned", "still in a pre v1.0 stage". https://github.com/OpenPathGuidingLibrary/openpgl
- CONFIRMED: RTXDI 3.2.0, ReSTIR DI/GI(2.0)/PT(3.0), D3D12 + Vulkan via NVRHI, HLSL via DXC; license not stated on the page.
  https://github.com/NVIDIA-RTX/RTXDI
- CORRECTED (softened): "no published spectral ReSTIR" holds for peer-reviewed work, but a Blender devtalk post and a Waseda thesis
  discuss it as open work (search snippets only). The band-summed-target argument remains UNVERIFIED.
  https://devtalk.blender.org/t/thoughts-on-making-cycles-into-a-spectral-renderer/2192
- COULD NOT VERIFY: Blackwell SER "2x", Khronos 47.78% figure, OptiX 9 SER chapter, HAGS crash fix status (not rechecked).

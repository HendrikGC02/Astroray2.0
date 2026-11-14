# Denoising and upscaling for a third-party Blender engine (late 2026)

Researched 2026-11-14. Tags: VERIFIED = I read the primary source (repo file, PR, release notes, API response). REPORTED = secondary source or vendor claim I did not test. UNVERIFIED = inference or not found.

## Question

Which denoisers and upscalers can a third-party Blender render engine really use in late 2026? Can DLSS Ray Reconstruction (DLSS-RR) run from a CUDA/OptiX or Vulkan compute renderer with no game swapchain? What does it need as input, and what do the licences allow? How did Blender 5.3 integrate it under the GPL? How do OptiX, NRD, OIDN, FSR and XeSS compare? How do they interact with spectral rendering and with scientific output? Which buffers must the integrator write from day one, and what are sensible viewport and final-render defaults?

## Findings

### 1. DLSS-RR can be driven from CUDA, with no swapchain and no Streamline

- NGX (the layer under DLSS) exposes CUDA, Vulkan, D3D11 and D3D12 entry points. Conversion between APIs is the app's job. VERIFIED: https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf (section 1 and 5.2, read via pdftotext).
- The DLSS SDK ships a CUDA path for Ray Reconstruction. `include/nvsdk_ngx_helpers_dlssd_cuda.h` has `NGX_CUDA_CREATE_DLSSD_EXT` and `NGX_CUDA_EVALUATE_DLSSD_EXT`. Inputs and outputs are CUDA texture/surface objects (`CUtexObject`), and the create-params struct carries the CUDA context and stream as `void*` fields (`InCUContext`, `InCUStream`). VERIFIED: https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_dlssd_cuda.h
- Vulkan and D3D12 helpers exist too (`nvsdk_ngx_helpers_dlssd_vk.h`, `..._d3d.h`). VERIFIED: file listing at https://github.com/NVIDIA/DLSS/tree/main/include
- NVIDIA staff on the OptiX forum: the direct CUDA API was NOT enabled in public drivers in Dec 2025, then "released this week" on 2026-01-29 (SDK 310.5.3, tagged 2026-01-26). It needs Ampere or newer and a 590+ driver. Streamline is "not likely" to be the route for CUDA/OptiX apps. VERIFIED: https://forums.developer.nvidia.com/t/optix-sample-for-real-time-animated-subdivision-surfaces-dlss-rr/331986 (thread JSON read directly). NVIDIA's `optix-subd` sample uses it: https://github.com/nvidia/optix-subd
- DLSS-RR works on RGB only. An NVIDIA reply to a question about arbitrary wavelength channels was "RGB only" (2026-04-23). VERIFIED: same thread.
- Streamline (repo `NVIDIA-RTX/Streamline`, v2.14.16, 2026-10-16) targets DirectX 11/12 and Vulkan 1.2+, and wraps the DLSS DLLs. Its own code is MIT-style (`license.txt`). I found no CUDA path. VERIFIED: https://github.com/NVIDIA-RTX/Streamline (README prerequisites, license.txt).
- Current SDK is v310.9.16 (2026-10-16); v310.9.1 (2026-09-08) is what Blender developers called DLSS-RR 4.5. VERIFIED: `gh api repos/NVIDIA/DLSS/releases`; Blender PR comment, see 2.

### 2. How Blender 5.3 integrated it (PR #153077) and why the GPL is not a problem for them

- PR #153077, "Cycles: Add NVIDIA DLSS support for viewport denoising", merged 2026-09-10. VERIFIED: https://projects.blender.org/blender/blender/pulls/153077 (read via Gitea API).
- It does not link against DLSS. The NVIDIA DLSS SDK is pulled in only for type definitions. The PR body also says the NGX driver "will internally look for a DLSS implementation library" in the application path and prefers it; without one the API reported DLSS unsupported (at 590-era drivers). Driver-resident DLSS only arrives with 620 (see below), so a third-party exe on 590-619 likely needs a local DLL. The implementation is loaded at runtime from the NVIDIA driver's NGX component (`_nvngx.dll` / `libnvidia-ngx.so.1`). Later the SDK's own header-only loader replaced custom loader code (`NVSDK_NGX_HEADER_ONLY`). VERIFIED: PR body and comment 42; https://raw.githubusercontent.com/blender/blender/main/intern/cycles/integrator/denoiser_dlss.cpp
- `denoiser_dlss.cpp` is Apache-2.0 and calls the NGX CUDA API (`NVSDK_NGX_CUDA_Init1`, `CreateFeature1`, `EvaluateFeature`). Feature flags: `IsHDR | MVLowRes | AlphaUpscaling`. Depth is linear. Roughness is packed into the normal's alpha. It passes color, depth, diffuse albedo, specular albedo, normal+roughness, motion vectors and specular motion vectors. VERIFIED: same file.
- Build side: `build_files/cmake/Modules/FindDLSS.cmake` only needs `nvsdk_ngx.h`. `blender_release.cmake` sets `WITH_DLSS ON`. VERIFIED: raw files in github.com/blender/blender main.
- Early licensing position. In Jan 2026 a Blender dev (sherholz) wrote that Blender "can not ship the full package" because it would not be GPL-compliant, and brecht wrote on 2026-02-02 that licence compatibility was not sufficiently verified and there would be no signed package build "for now". At that time users had to copy `nvngx_dlssd.dll` next to blender.exe. VERIFIED: PR comments 2 and 12.
- Resolution. PR #165500 (merged 2026-10-21): "DLSS is bundled with the driver starting with the 620 driver releases. This is the intended way to use DLSS in Cycles", so the minimum driver was raised. PR #164002 (merged 2026-09-18) enabled DLSS in release builds via buildbot-installed SDK headers and moved Cycles to CUDA 13. VERIFIED: https://projects.blender.org/blender/blender/pulls/165500 and /164002.
- Driver 620 (Windows 621.14, Linux 620.70.06) was released 2026-11-04. REPORTED: https://www.phoronix.com/news/NVIDIA-620.70.06-Linux-Driver
- Docs conflict on the driver number: the 5.3 release notes say 620 (VERIFIED, https://developer.blender.org/docs/release_notes/5.3/cycles/); the manual says 590 (stale, VERIFIED, https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/render/cycles/gpu_rendering.rst); PR #153077's text says 590. The manual also says compute capability 7.5+ and "DLSS is not available for final render denoising."
- Behaviour Blender needed for DLSS (all VERIFIED in PR #153077 body/comments): render one independent jittered sample per frame and reset after every sample; Halton sub-pixel jitter passed to DLSS; viewport camera motion vectors (object motion disabled at first; later PR #156227 added viewport motion vectors); specular albedo pass added (later PR #153545); adaptive sampling disabled; minimum input size 128x96; alpha is upscaled by DLSS (flag) rather than copied.
- Quality issues Blender reviewers reported: ghosting from missing motion vectors; no convergence when the camera rests, with reflections and highlights clamped or patchy compared to OptiX/OIDN at 512 spp (comment 16). NVIDIA's developer said DLSS was being changed to converge better with more input (comment 24, 2026-04-22), and that depth-of-field needs a DOF guide that was not yet hooked up (comment 49). A Blender maintainer (Sergey): "viewport-only option"; for offline renders OIDN is a better choice (comment 28). Sergey (comment 23) also objected to a hybrid DLSS-then-OIDN switchover because artists would not know which result they were looking at; the NVIDIA developer later called that experiment obsolete (comment 24).
- Blender's other upscaling-denoiser plumbing: PR #151133 added generic support (OptiX upscale model as demo). It states OIDN does not upscale. VERIFIED: https://projects.blender.org/blender/blender/pulls/151133. A MetalFX viewport denoiser PR (#160045) is still a WIP: VERIFIED listing via API.

### 3. DLSS-RR inputs and constraints (integration guide, Aug 2026)

Source for this whole block: https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS-RR%20Integration%20Guide.pdf (VERIFIED, read in full).

- Required, at render resolution: noisy color (HDR, before tone mapping), diffuse albedo (3-ch, not sRGB format), specular albedo (average specular reflectivity for the view direction; the guide gives `EnvBRDFApprox2`), normals (normalized; world space if the app does not supply specular motion), roughness (linear; may be packed into normal alpha), motion vectors (RG16F/RG32F, pixels), depth (linear view-space or HW depth).
- Then EITHER specular motion vectors OR specular hit distance (world-space distance from the primary surface to the first specular hit) plus world-to-view and view-to-clip matrices. Sky: hit distance FP16_MAX, specular albedo 0, diffuse albedo 0.5.
- Optional: transparency layer, color-before-transparency (presets E/F), SSS guide, DOF guide (E/F), responsivity mask (F), alpha upscaling (E/F).
- Not supported: dynamic resolution, exposure/auto-exposure/sharpness. Jitter: at least 32 phases recommended. Modes: DLAA, Quality 0.66, Balanced 0.58, Performance 0.5, Ultra-Performance 0.33. Only the default preset is recommended.
- Sampling: RR "assumes independent samples" with minimal spatial and temporal correlation. Avoid checkerboarding, screen-space dithering, shared sampling patterns. Noisy guide buffers cause grain and ghosting.
- Cost: about 154 MB (1080p) to 579 MB (4K) of GPU memory on RTX 30/40/50 for the default preset. Per-GPU ms figures are in the guide's tables, but the text extraction mislabels the rows, so I quote none. Needs driver >= 580 for DLSS 4.5+ RR in general.
- Distribution note inside the guide: chapter 4.2 says the dev and release DLLs "included in the NDA SDK package must not be included or distributed in any public release" and to contact NVIDIA for a distributable DLL. The older DLSS Programming Guide (4.2) says the `rel` DLL is the one to ship. These conflict; see Unverified.

### 4. DLSS / NGX / NRD licence text (NVIDIA RTX SDKs LICENSE)

Read at https://raw.githubusercontent.com/NVIDIA/DLSS/main/LICENSE.txt (v. March 14, 2024), VERIFIED. Key clauses:

- 1(c) allows distributing SDK software in object-code form inside an application; 2(a) the application needs "material additional functionality"; 2(c) distribute under terms at least as protective; 4(b) no stand-alone redistribution.
- 4(e): you may not use the SDK in a way that would subject it to an open source licence (disclosure, derivative-work licensing, or no-charge redistribution).
- Supplement 1: DLSS and NGX SDKs are licensed only for systems with NVIDIA GPUs. Supplement 4: notify NVIDIA before commercial release of an application "(including a plug-in to a commercial application)" using DLSS or NGX (https://developer.nvidia.com/sw-notification). Supplement 6.1: NVIDIA may update the SDK over the air. 6.2: NVIDIA may disable a problematic integration as a last resort. 7.1(b): attribute the SDK and show NVIDIA marks on splash/about screens. 4(f) excludes life-critical uses (navigation, medical); astrophysical visualization is not in that category.
- Third-party code notices from section 9.5 must be reproduced if you ship the DLL (Programming Guide 4.2.3).
- The same repo hosts the DLLs under `lib/*/rel` (e.g. `nvngx_dlssd.dll`, 48 MB). A repo clone is heavy; for build time only `include/` is needed.
- NRD's LICENSE.txt is an older version of the same agreement (v. April 12, 2021) with the same 4(e) clause. The NVIDIA-GPU-only clause names only DLSS and NGX. VERIFIED: https://raw.githubusercontent.com/NVIDIA-RTX/NRD/master/LICENSE.txt
- OptiX headers (`NVIDIA/optix-dev`, v9.1.0) are under a different "SDK ... License Agreement (with distribution rights)": binary-only incorporation, supported NVIDIA hardware only, and 2.1(xii) forbids use that makes the software subject to an open-source licence. VERIFIED: https://github.com/NVIDIA/optix-dev/blob/main/LICENSE.txt
- Blender Extensions Platform accepts add-ons only under GPL-3.0-or-later. VERIFIED: https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/advanced/extensions/licenses.rst. Together with 4(e), this means an add-on on that platform must not bundle NVIDIA binaries. Loading the driver-resident implementation (the Blender approach) avoids that.

### 5. OptiX AI denoiser (OptiX 9.x)

- Latest OptiX found: 9.1.0 (Dec 2025), needs R590+; the only denoiser line in the notes is better output quality; the full OptiX SDK was also published on GitHub in Jan 2026. VERIFIED: https://forums.developer.nvidia.com/t/optix-9-1-release/354119. I searched for OptiX 9.2 / 10 and found none (absence UNVERIFIED).
- Built-in model kinds: `AOV`, `TEMPORAL_AOV`, `UPSCALE2X`, `TEMPORAL_UPSCALE2X` (the old LDR/HDR/TEMPORAL are deprecated). Guide layers: albedo (RGB), normal (world-space XYZ for the new models), flow (pixel motion previous to current), flow trustworthiness, plus internal guide layers carried frame to frame. Alpha mode copy/denoise. VERIFIED: https://github.com/NVIDIA/optix-dev/blob/main/include/optix_types.h (lines ~1820-1990).
- It takes CUDA device pointers (`CUdeviceptr`), so it is zero-copy from a CUDA renderer. NVIDIA-only. Quality note from Blender developers: DLSS-RR "is a significant improvement" on 1 spp viewport input compared with "Optix, OIDN 2.0" (sherholz, 2026-09-10; REPORTED opinion in PR comment 48). The OptiX upscale mode also upscales alpha (comment 49).

### 6. Intel OIDN (2.5.x now; 3 not released)

- Latest release v2.5.1 (2026-08-18); `devel` is at v2.5.2 (2026-10-05). Licence Apache-2.0. VERIFIED: `gh api repos/RenderKit/oidn` and CHANGELOG at https://github.com/RenderKit/oidn
- Supports NVIDIA Turing through Blackwell via CUDA, AMD RDNA2-4, Intel, Apple and CPUs. Volta dropped and CUDA 13 build support added in 2.4.0. NVIDIA driver 528.33+. The README lists Blackwell; PR #258 (compute capability 12.0) is closed, merge not confirmed. VERIFIED: README, release notes, PR list.
- Interop: pass `cudaMalloc` pointers directly; create a device on your own stream (`oidnNewCUDADevice`) (VERIFIED, README). External semaphore import was added in 2.5.0 for CUDA (Win and Linux) and HIP (Win) (VERIFIED, release notes). Whether the CUDA device can import Vulkan/D3D12 buffers via `oidnNewSharedBufferFromFD/Win32Handle` is NOT confirmed by the 2.5.0 notes (they only add a dedicated-memory flag); the README says support depends on device and driver, queried through the `externalMemoryTypes` device parameter. Treat CUDA external-memory import as UNVERIFIED until the spike queries it.
- Prebuilt Windows binary zip is 56.6 MB; no need to compile it. VERIFIED: release assets.
- `RT` filter: `color` is 1-3 channels (so a single-band image is allowed), plus optional `albedo` and `normal`, `hdr`, `inputScale` (maps to physical units, 1.0 = 100 cd/m2), `cleanAux`, `quality` (high/balanced/fast, "balanced or fast for interactive"). It is "not temporally stable". Auxiliary images should hold the first non-delta hit (follow perfect mirrors and glass). VERIFIED: README.
- OIDN 3: CG Channel (2026-01-30) reports temporal denoising (realtime variant uses previous denoised frame, backward motion vectors, depth; final-quality variant also uses future frames and forward motion), kernel-prediction network, supersampling, due "second half of 2026". REPORTED: https://www.cgchannel.com/2026/01/open-image-denoise-3-will-support-temporal-denoising/. As of 2026-11-14 there is no 3.x release and the `devel` branch is 2.5.x. VERIFIED. Roadmap issues opened 2026-09-29 by an Intel developer, with only a hardware line in each body: #299 "Viewport temporal + upscaler filter" (x86 CPUs with AMX-FP16 only; Intel/NVIDIA/AMD/Apple GPUs), #298 same for Intel GPUs only, #300 "Final frame temporal filter" (x86/ARM64 CPUs). VERIFIED: https://github.com/RenderKit/oidn/issues/299 etc. These say nothing about dates or inputs. A third-party AI-written summary of a BCON26 talk claims OIDN 3 temporal needs split diffuse/specular albedo, roughness and depth (UNVERIFIED): https://youtubesummary.com/summary/50x2bgBdz-E
- An anecdotal user comment (issue #293) puts OIDN's small albedo+normal model at about 8 ms versus DLSS-RR about 5 ms on an RTX 4070. REPORTED, one user, no scene or resolution given.

### 7. NVIDIA NRD

- v4.18.0 (2026-10-30). API-agnostic spatio-temporal denoiser for D3D12, Vulkan or D3D11. It makes no graphics calls; the app dispatches NRD's HLSL compute shaders (integration layer built on NRI). No CUDA path. Needs per-pixel normal, roughness, view-Z and motion vectors; noisy diffuse and specular radiance with hit distance packed by NRD helper functions; opaque surfaces only ("not natively designed for volumetrics or transparency"). README claims its "SH" mode is "comparable" to DLSS-RR; vendor performance claim for REBLUR diffuse+specular is 2.55 ms at 1440p on an RTX 4080. VERIFIED that these claims are in https://github.com/NVIDIA-RTX/NRD/blob/master/README.md; quality claim REPORTED.
- It is not NVIDIA-only at the GPU level (HLSL compute), but it demands a diffuse/specular split with hit distances, a demodulation convention, and a D3D12/Vulkan host.

### 8. AMD FSR and Intel XeSS

- AMD FSR SDK v2.4.0 (2026-10-28) includes "FSR Ray Regeneration 1.2.1", an ML denoiser. It needs a Radeon RX 9000 series GPU or later, and its docs list DX12 only: "Vulkan is currently not supported for Ray Regeneration". It ships as signed DLLs. VERIFIED: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (release notes, readme known-issues table, denoising.md line ~1186).
- FSR licence (`docs/license.md`): redistribution of copies "in binary form only", no reverse engineering, notices must be reproduced. VERIFIED. I did not check whether older MIT-licensed FSR 2/3 source remains in the tree.
- XeSS SDK v3.0.2 (2026-07-24): Intel Simplified Software License (Oct 2022), binary-only redistribution, no modification, no reverse engineering. VERIFIED: https://github.com/intel/xess/blob/main/LICENSE.txt. I did not verify its feature list; XeSS and FSR upscaling take renderer-supplied motion vectors, depth and jitter and are upscalers, not path-tracing denoisers (general knowledge, not re-checked).
- Result: for a vendor-neutral temporal denoiser there is nothing shipping. OIDN 3's viewport temporal filter (issue #299) is the only cross-vendor candidate, and it is unreleased. FSR Ray Regeneration is DX12 and RDNA 4 only.

### 9. Spectral rendering and scientific output

All of this section is inference from the findings above, not a measured result (UNVERIFIED unless cited).

- Every ML denoiser here consumes RGB (DLSS-RR "RGB only", VERIFIED; OptiX and OIDN take 1-3 channel images). A spectral integrator must convert accumulated spectra to linear scene-referred RGB (via XYZ) before denoising and denoise before the view transform (DLSS-RR guide: before tone mapping; OIDN: HDR with `inputScale`).
- Guides must be RGB projections of spectral quantities. Diffuse and specular albedo become the reflectance spectrum projected under a fixed reference illuminant, clamped to [0,1]. This is a design choice that needs checking: an albedo guide that does not match the colour image's response may produce colour shifts.
- Hero-wavelength sampling produces chromatic noise on top of luminance noise. Whether denoisers trained on RGB path-traced noise remove it cleanly is not documented anywhere I read. The spike must test it.
- Blender reviewers saw DLSS-RR clamp or lose highlights and reflections relative to a 512 spp reference (VERIFIED, PR comment 16). Any denoiser may blur, clamp, or shift energy. None preserves photon statistics or per-pixel error bars.
- Reprojection-based methods (DLSS-RR, NRD, OptiX temporal, OIDN 3 temporal) assume rigid reprojection of surfaces. Gravitationally lensed images do not move like surfaces when the camera moves, so these will probably ghost on relativistic-lensing scenes. Spatial-only OIDN is the safe option there. Hypothesis to test.
- Non-visible bands, narrow-band emission-line maps and spectral cubes: DLSS-RR cannot take them (RGB only). OIDN can denoise one channel at a time but it is trained on RGB renderings, so the result is a visual aid only.
- Therefore: science buffers (spectral radiance, photon counts, per-pixel sample count and variance) are written raw. Denoised output exists only as a separate display product, labelled as such in file metadata. The denoiser API takes the science buffer read-only and writes only a display buffer.

## Options and tradeoffs

The real forks, not a fixed number of options:

1. Backend coupling. DLSS-RR (CUDA path), the OptiX denoiser and OIDN all accept CUDA device pointers on NVIDIA, so a CUDA/OptiX backend gets all three with no copies. A Vulkan-compute backend can use DLSS-RR through the NGX Vulkan helpers, OIDN through external-memory import (CUDA-device support for this is unconfirmed; query `externalMemoryTypes`, else copy), and NRD natively, but the OptiX denoiser would need CUDA interop. A non-NVIDIA GPU gets only OIDN (and FSR RR on RDNA 4 under DX12). So the denoiser landscape does not force the backend choice, but it does mean a vendor-neutral backend gives non-NVIDIA users a slower, spatial-only viewport until OIDN 3 ships.
2. Temporal reconstruction in the viewport versus progressive accumulation plus a spatial denoiser. DLSS-RR gives the fastest clean viewport (1 spp per frame, upscaling) but needs motion vectors, specular guides, decorrelated samples and a reset every frame. It does not converge to the reference in Blender's report and the integrator must change how it samples. Accumulation plus OIDN balanced/fast converges to the correct image, at lower interactive speed.
3. Licence exposure. OIDN (Apache-2.0) carries no restrictions. DLSS, NGX, NRD and OptiX share the "no open-source contamination" clause. The Blender route (build-time headers only, driver-resident runtime, no bundled DLL) is the only route shown to work with a GPL product. If Astroray 2.0 is distributed outside the Extensions Platform under a proprietary licence, bundling the DLL becomes possible in principle but hits the unresolved NDA wording and needs NVIDIA contact.
4. Mixing modes in one viewport (DLSS while moving, OIDN when still). Blender's maintainer objected to it for trust reasons; it is cheap for us to implement and a product decision for the owner.
5. NRD. High integration cost (diffuse/specular split, hit-distance packing, HLSL dispatch host, opaque surfaces only) for a quality claim that is vendor-reported. It buys nothing over DLSS-RR on NVIDIA and nothing on CUDA without an interop layer. I see no case for it on day one.
6. Own temporal accumulation (reprojection + variance-guided accumulation). No licence, vendor-neutral, but we write and tune it ourselves and it will not match a trained model on 1 spp input. Only worth considering as the non-NVIDIA viewport fallback if OIDN 3 slips.

## Recommendation for Astroray 2.0

Rests on: findings 1-3 (CUDA path, driver-bundled runtime, Blender precedent), 4 (licence clauses), 6 (OIDN licence, interop, unreleased OIDN 3), 8 (no cross-vendor temporal denoiser).

1. Define one `Denoiser` interface inside the engine. Input: an AOV bundle (below), a mode (viewport/final), and a device context. Output: a display buffer only. Backends behind it: OIDN, OptiX, DLSS-RR. A missing or unsupported backend must report loudly and fall back to the next one, never silently (project rule 7).
2. Final render default: OIDN `RT`, quality high, `hdr=true`, albedo and normal prefiltered then `cleanAux=true`, run on the CUDA device (own stream, no copy) or on CPU. This is the only option that also runs on the CPU oracle path and on non-NVIDIA GPUs, and Blender maintainers prefer it for offline output. Always write the raw (noisy) combined and science passes next to the denoised display pass.
3. Viewport default (provisional, pending the spike): progressive accumulation with OIDN balanced (fast if the frame budget is missed) on the accumulated buffer. This is the always-available baseline, converges to the reference, and works on every backend.
4. Viewport fast mode on NVIDIA, driver >= 620 with `NVSDK_NGX_CUDA_GetFeatureRequirements` reporting RR supported: DLSS-RR through the NGX CUDA API, loaded the way Blender does it (header-only loader at build time, nothing bundled, nothing linked). Quality or Balanced upscale. Make it opt-in until the spike shows acceptable bias on astrophysical test scenes and a clean convergence story; it can become the default only after that. If the driver or GPU cannot provide it, show the reason in the UI (Blender does this).
5. OptiX denoiser: implement only as a thin second NVIDIA backend if the spike shows it beats OIDN on the 5070 Ti in the viewport (its temporal and 2x-upscale modes are available now). It costs little because it uses the same guide buffers as OIDN plus flow.
6. Do not adopt NRD, FSR Ray Regeneration or XeSS now. Re-evaluate OIDN 3 when a release exists; the issue #299 hardware line suggests it will be the cross-vendor viewport option.
7. Licence handling: do not redistribute any NVIDIA DLL or SDK binary. Fetch SDK headers at build time through a CMake option only (as Blender does); keep headers out of the source tree. Before any commercial release, send the notification the DLSS licence requires, and ask NVIDIA for an application or project ID instead of reusing Blender's (Blender uses ID 100334311, assigned to it; NGX also has a project-ID init path with a custom engine type, VERIFIED in the headers and Programming Guide 2.4).
8. Science outputs: raw and variance/sample-count buffers are never passed to any denoiser as an output target; a denoised image is never written to a science channel. Display EXR layers carry an explicit "denoised" tag.

### Buffers the integrator must produce from day one

Cheap to write during primary-ray shading, expensive to retrofit. All at render resolution, linear, pre-tone-mapping. They keep DLSS-RR, OptiX (AOV/temporal/upscale), OIDN (2 and the announced 3), and NRD all possible.

| Buffer | Format / space | Used by |
|---|---|---|
| Noisy combined radiance (display RGB from spectral accumulation) | RGB float, scene-referred | all |
| Noisy diffuse and specular radiance separately, and their hit distances (diffuse, specular) | RGB(A) float | NRD, FSR RR; lets us demodulate; DLSS specular hit distance |
| Diffuse albedo and specular albedo (RGB projection of spectra; specular via analytic env-BRDF from F0, roughness, NoV) | RGB, [0,1] | DLSS-RR (both), OIDN, OptiX (diffuse), OIDN 3 (reported) |
| Shading normal, world space, plus linear roughness | RGB16F + 1 ch (or packed alpha) | all |
| Linear view-space depth | 1 ch float | DLSS-RR, NRD, OIDN 3 |
| Backward 2D motion vectors in pixels (camera + object + deformation, jitter removed) | RG float | DLSS-RR, OptiX flow, OIDN 3, NRD |
| Specular motion vectors, or specular hit distance together with per-frame view and projection matrices | RG or 1 ch float | DLSS-RR |
| Alpha / coverage | 1 ch | DLSS-RR alpha upscaling, OptiX alpha |
| Sub-pixel jitter offset per frame, frame index, "history valid" reset flag, previous-frame camera and per-instance transforms | metadata | DLSS-RR, OptiX temporal |
| Per-pixel sample count and variance (raw spectral/science accumulators too) | 1 ch each | adaptive sampling, science error bars; never denoised |
| Object/material ID | uint | masking, debugging, future per-object denoise policy |

Two design consequences. First, the "denoising surface" (OIDN advice: follow perfect specular paths and record the first non-delta hit) must be defined in the integrator, separate from the primary hit. Second, the viewport sampler must support the DLSS mode: one decorrelated sample per pixel per frame, per-frame reseed, at least 32 Halton jitter phases, no dependence on screen-space dither or shared patterns.

## What a measurement spike must settle

Hardware: RTX 5070 Ti, driver >= 620 (Windows 621.14 or later). Each item names a pass/fail the owner can read.

1. Does DLSS-RR via the NGX CUDA API initialise and run from a non-Blender CUDA process on driver 620? Specifically: does `GetFeatureRequirements` report supported with no DLL beside the executable; does init with an NVIDIA-supplied or custom project ID work without Blender's application ID; how long do CUDA-buffer to `CUtexObject` conversions take (Blender wrote custom kernels for this)?
2. DLSS-RR cost and memory on the 5070 Ti at 1080p and 1440p output for DLAA, Quality, Balanced, Performance. Compare with OptiX AOV, OptiX temporal upscale2x and OIDN high/balanced/fast on CUDA at the same resolutions, all measured the same way (burn-in plus min-of-N, since GPU boost clocks drift).
3. Image quality against a high-spp reference on at least four scenes: a diffuse Cornell-like scene, a glossy/specular scene, an emission nebula, and a black-hole lensing scene. Metrics: HDR relative error in highlights and in dark regions, convergence when the camera rests (does DLSS-RR approach the reference?), temporal stability under camera orbit, ghosting around lensed regions. Include qualitative review of the saved images.
4. Spectral interaction: compare denoising the RGB projection of hero-wavelength output against denoising the same scene rendered with RGB-only light transport. Does chromatic noise survive? Does the albedo-guide projection choice shift colour?
5. Sampling requirements: does our RNG (stratified/low-discrepancy per frame) meet DLSS-RR's decorrelation requirement, or does it show correlated artifacts until replaced by hashed white noise?
6. OIDN CUDA zero-copy path from our own buffers on our own stream; and the Vulkan/D3D12 external-memory import path if a Vulkan viewport is chosen.
7. Whether a 1-channel band image gives usable results in OIDN, and its failure modes on narrow emission lines (display aid only).
8. Build cost: time to add OIDN (prebuilt zip) and NGX headers (include/ only) to the build; confirm no change to the 30 s incremental budget; confirm denoiser tests live in the GPU integration tier, not the 10 s pre-commit tier.

## Unverified or conflicting

- Minimum driver for DLSS in Blender: 590 (manual, PR #153077 text) versus 620 (5.3 release notes, PR #165500). I resolved this to 620 as the intended path from PR #165500, but I have not run it.
- Whether DLSS-RR support is complete in driver 620 on every RTX generation, and whether a non-Blender application sees the driver-bundled `nvngx_dlssd` without a local copy. The PR #165500 statement is about Cycles; I did not test a third-party exe.
- DLL distribution wording: the RR Integration Guide chapter 4.2 (NDA SDK, contact NVIDIA) versus the LICENSE and Programming Guide 4.2 (ship the `rel` DLL). Moot if we rely on the driver-bundled copy; matters if we ever bundle. NVIDIA's legal contact is nvidia-rtx-license-questions@nvidia.com (LICENSE section 19).
- The licence status of the DLSS feature DLLs inside Streamline release zips. I only read the MIT-style Streamline code licence.
- Whether Blender's official 5.3 binaries use the driver-bundled runtime in every case: PR #164002 says DLSS was enabled in release builds; I did not download a 5.3 build to confirm.
- OIDN 3 release date, inputs, and licence. Only a January news story and three roadmap issues with empty bodies exist.
- Whether any shipping denoiser handles spectral hero-wavelength chroma noise, or lensing reprojection, well. Both are hypotheses.
- I found no OptiX 9.2 or 10. XeSS 3 features and FSR Ray Regeneration pricing/terms beyond the binary-redistribution licence were not read.
- Per-GPU DLSS-RR timings from NVIDIA's own tables: the PDF extraction mislabelled rows, so none are quoted here.

## Verification pass

Independent check 2026-11-14 against primary sources. The Recommendation is unchanged.

- CONFIRMED: DLSS-RR CUDA API exists (`NGX_CUDA_CREATE_DLSSD_EXT` / `NGX_CUDA_EVALUATE_DLSSD_EXT`, `CUtexObject` gbuffers). https://raw.githubusercontent.com/NVIDIA/DLSS/main/include/nvsdk_ngx_helpers_dlssd_cuda.h
- CORRECTED: context and stream are passed as `void*` (`InCUContext`, `InCUStream`), not typed `CUcontext`/`CUstream`. Same URL.
- CONFIRMED: NVIDIA staff (dlacewell) said "released this week" on 2026-01-29, added in SDK 310.5.3, "driver 590+ and at least an Ampere GPU"; Streamline "not likely" (2025-12-05); "RGB only" reply dated 2026-04-23. https://forums.developer.nvidia.com/t/optix-sample-for-real-time-animated-subdivision-surfaces-dlss-rr/331986
- CONFIRMED: Blender PR #153077 merged 2026-09-10; SDK used for type definitions only; implementation loaded via the NVIDIA driver's NGX. https://projects.blender.org/blender/blender/pulls/153077
- CORRECTED (nuance): the same PR body says NGX looks for a DLSS implementation library in the application path and reports unsupported without it; min driver stated as 590. Driver-resident DLSS is only from 620, so the "no DLL needed" path needs driver >= 620. Added to finding 2.
- CONFIRMED: PR #165500 merged 2026-10-21: "DLSS is bundled with the driver starting with the 620 driver releases", the intended way for Cycles. https://projects.blender.org/blender/blender/pulls/165500
- CONFIRMED: LICENSE 4(e) (no use that subjects the SDK to an open-source licence), Supplement 1 (DLSS/NGX only for NVIDIA GPU systems), Supplement 4 (notify before commercial release, including plug-ins). https://raw.githubusercontent.com/NVIDIA/DLSS/main/LICENSE.txt
- COULD NOT VERIFY (this pass): that the Blender Extensions Platform requires GPL-3.0-or-later (earlier researcher read the manual; not re-fetched). The legal conclusion that GPL and the NVIDIA licence conflict is an interpretation, not legal advice.
- CONFIRMED: OIDN Apache-2.0, NVIDIA Turing to Blackwell, `cudaMalloc` pointers and `oidnNewCUDADevice`, RT filter not temporally stable, latest release v2.5.1 (first release page shows no 3.x). https://github.com/RenderKit/oidn and /releases
- CORRECTED: the claim of CUDA external-memory (Vulkan/D3D12 buffer) import was not supported by the 2.5.0 notes; only external semaphore import is CUDA-listed. Now UNVERIFIED. https://github.com/RenderKit/oidn/releases/tag/v2.5.0
- CONFIRMED: issue #299 opened 2026-09-29 by an Intel developer, body is only a "Supported HW" list (x86 AMX-FP16 CPUs; Intel/NVIDIA/AMD/Apple GPUs). https://github.com/RenderKit/oidn/issues/299
- COULD NOT VERIFY: GitHub API releases endpoints returned 403; only the first releases page was read, so a 3.x release on later pages is very unlikely but not strictly excluded (the list is date-ordered).
- Not re-checked (remain as tagged above): driver 620 release date, NRD, FSR/XeSS, OptiX 9.1, Streamline details, PR comment quotes.

# Inherited research from Astroray v1

This folder holds distilled research inherited from Astroray v1, a Blender render addon over a C++/CUDA spectral path tracer with a CPU oracle. It is the only channel from v1 into the rewrite: v1 code, packages and issue history are deliberately not carried over. Each file has the same shape: established findings, sources and reference implementations, measured numbers, dead ends, design ideas worth reconsidering, what no longer applies, and open questions. Numbers were measured on v1's engine (mostly one RTX 5070 Ti workstation) and are order-of-magnitude anchors to re-measure, not predictions. Cycles appears as a source of Blender semantics and reference algorithms, not as a pixel-parity target. Read "Dead ends" before re-trying an idea, and "What no longer applies" before copying a v1 decision.

| File | What it covers | Questions it answers best |
|---|---|---|
| survey.md | Other engines, recent research, adoption ranking, licensing, product direction, generic v1 postmortem lessons | What should we adopt or avoid? Which licences are safe? What bug patterns recur? |
| spectral.md | Observer and colour space, Jakob-Hanika upsampling, hero wavelengths, spectral MIS, dispersion, emission spectra | Why did colours skew? How do we choose wavelengths and pdfs? |
| materials.md | Microfacets, multiple-scattering compensation, glass orientation, layering, thin film, subsurface, hair | How must dielectrics and layers be oriented and energy-checked? |
| transport.md | MNEE/SMS and photon caustics, MIS, adaptive sampling, fireflies, guiding, ReSTIR, RNG, CPU/GPU agreement | Which caustic or sampling method works, and why did others fail? |
| lights.md | Sky models, sun disc, HDRI sampling, light tree, light energy calibration, IES | How do Blender light units map to radiometry? Which sky model? |
| volumes.md | World volumes, null-scattering and tracking, NanoVDB majorants, emission, mesh-volume boundaries, clouds | How to build volumes staged and testable? |
| gpu-build.md | Wavefront design, register pressure, OptiX traversal, two-level BVH, GPU twin bugs, build and test speed | How to structure the GPU backend and keep builds fast? |
| gr-astro.md | Kerr metric, geodesics, invariant radiative transfer, accretion emission, atomic lines, science data I/O | How to get redshift and emission right against GYOTO? |
| testing.md | SSIM vs Welch gating, feature-coverage gate, corpus design, diagnosing dim or chroma residuals | What gates catch real bias without chasing noise? |
| blender.md | Depsgraph sync, viewport threading, node semantics, passes, settings mapping, packaging | How to integrate with Blender without dead controls? |
| misc.md | Camera motion blur, coincident faces, settings-honour audit, mesh caustics, hermetic installer | Is that "GPU bug" actually the scene? How to verify installs? |

## Most valuable inherited findings

- spectral.md: integrate with one observer end to end (CMF, XYZ-to-RGB matrix, white point, upsampling-table fit); D65 must return (1,1,1). Re-pin tests that encoded the old bug.
- materials.md: dielectric enter/exit comes from the geometric front-face flag, never from the sign of dot(wo, normal); furnace on closed solids catches it.
- testing.md: SSIM between independent noisy renders measures noise; use the tile-wise Welch test with Holm correction plus a relative margin.
- testing.md: a colour-blind (luminance) adaptive stop criterion stalls per-channel noise; use per-channel stops or disable adaptivity when comparing.
- misc.md: a "GPU-only dark wedge" was ill-posed scene geometry (coplanar faces, inward winding); check the input first.
- gr-astro.md: redshift must come from photon momentum, not declared inclination; invariant transfer with I/nu^3 matched GYOTO to about 1%.
- gpu-build.md: wavefront with SoA path state and material-bucketed shading; register pressure in the shade stage is the main GPU performance constraint.
- gpu-build.md: checklist of GPU twin bugs found by CPU/GPU ladders; compare backends before blaming the scene.
- transport.md: camera-side SMS/MNEE cannot make a flat prism rainbow; it is forward transport. SMS on triangles needs the analytic Jacobian.
- volumes.md: any parameter with no consumer must fail loudly; upsample volume colour as reflectance before Beer-Lambert, never the attenuated product.
- survey.md: apply gamma once at final write; furnace tests render linear with an upper bound; keep path depth identical across entry points.
- blender.md: every UI control needs a test that changes a render; stamp build identity into the module to detect stale loads.

# 0000 — Owner decisions

Decisions made by the owner. Later records build on these; they change only if the owner says so.

## 2026-11-14

1. **Correctness target:** physics and the OpenPBR specification. No pixel-parity gates against Cycles.
2. **One kernel source** compiled for CPU and GPU. No second hand-written implementation.
3. **Blender:** ship on 5.3, design for 5.4.
4. **Implementation language is the lead's call.** The owner wants whatever is fastest, most efficient and best to work in; C++ is not a requirement.
5. **Licence:** fully open source, no commercial option needed. Engine core Apache-2.0, Blender addon GPL-3.0-or-later.
6. **Repository:** public, on the owner's GitHub profile.
7. **First usable release is feature-complete in breadth:** surfaces, volumes, sky and world lighting, textures, lights. Not a surface-only renderer with volumes later.
8. **First astrophysics target:** a black hole inside an emission nebula.
9. **Viewport targets:** comparable to Cycles. 60 updates per second desirable, 30 acceptable, 24 the floor for interactive work. Time to first frame comparable to Cycles, including on large scenes, where v1 blew up.
10. **Viewport resolution:** stay at or near full resolution; reduce it dynamically only when the update rate falls below the floor. No fixed navigation downscale.
11. **DLSS** should be built into the engine's viewport if it can be.
12. **Merge authority:** the lead merges and keeps working without asking.
13. **GPU testing:** this PC may be used as the GPU test machine.
14. **Addon only; no Blender fork.** Astroray ships as a normal extension for stock Blender. Blender's source is used as a reference: to find the fast paths the Python API really has, to copy how Cycles structures scene sync and display, and to match node behaviour exactly. Proposing small API additions upstream is allowed but nothing may depend on them.
15. **Diagnostics and reporting:** heat maps of sample count, bounce count and similar statistics; benchmark reports with graphs; showcase renders at each milestone.

16. **Custom and advanced material types must exist**, not only standard OpenPBR surfaces. Catalogue: `research/17-materials-and-custom-types.md`.
17. **Diffraction gratings are in the first release.** Consequence accepted by the lead: the first release also needs the light-tracing or photon path that makes a grating useful (a light through a grating onto a surface), so that machinery is designed in from the first milestone rather than added later.

18. **One extension behaviour per material is enough.** Stacks such as a coated grating are not required.
19. **Fluorescence should cover all the use cases:** optical brighteners, fluorescent minerals and pigments, and astrophysical fluorescence. No single one takes priority.
20. The owner has no further items to add to the materials catalogue for now.

## 2026-11-14, checkpoint 2

21. **Viewport rate for 2.0:** a steady 30 updates per second at full resolution is the target; 60 is a later goal; 24 remains the floor.
22. **Progressive viewport batching:** like Cycles, the viewport shows the first samples one at a time for fast feedback and then does more samples per displayed update as the image settles, so less time goes to display and more to rendering.
23. **Time to first frame:** gate at no more than 1.5x Cycles, aim for equal.
24. **In the first release:** fluorescence, hair (reusing the knowledge from v1), DLSS together with the other denoisers, and scientific outputs (filter-band images, photon counts, data cubes).
25. **Caustics:** point lamps and spotlights must cast caustics onto surfaces. Caustics from such lamps that are only visible through glass or in a mirror, or that light the scene again after another bounce, may be missing in 2.0 with a visible warning.
26. **Ambient Occlusion and Bevel nodes** may have no effect in 2.0, reported in the render report.
27. **First milestone** is a small working slice (meshes, lamps, textures in the viewport); everything else is designed in and built in later milestones before the first release.
28. **Metal thin film** is physically computed, even where less saturated than Cycles.
29. **Left to the lead:** no scheduled unattended jobs; releases built for other NVIDIA generations and labelled untested; the engine stays in Blender's process unless GPU faults prove frequent.

## Lead defaults, standing unless the owner objects

- Distribution from a self-hosted extension repository and GitHub releases.
- Unsupported shader nodes render magenta and are listed in a per-render report.
- Windows only for 2.0.
- The CPU target is for correctness and tests, and can also render.

## Lead defaults on the questions the roadmap needs before M1a (owner decision 29's "follow your judgment"; standing unless the owner objects)

- **No contributor licence agreement.** Inbound equals outbound. Consequence: the project cannot be relicensed without every contributor.
- **CIE 1931 values** are shipped from pbrt-v4 at a pinned commit with provenance and DOI recorded (0001 D8.2); no permission request is sent first.
- **OptiX and DLSS licences:** follow the pattern Blender itself ships (compiled OptiX modules in an open-source product; DLSS loaded from the driver, never bundled). This is a reading, not legal advice.
- **Unit anchor:** as 0003 decision 11. Science channels from Blender-unit emitters are flagged, not refused.
- **OptiX denoiser:** not built for 2.0. "DLSS together with the other denoisers" (decision 24) is read as OIDN plus DLSS; it is added only with a measured win (0008).

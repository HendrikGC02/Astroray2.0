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

## Lead defaults, standing unless the owner objects

- Distribution from a self-hosted extension repository and GitHub releases.
- Unsupported shader nodes render magenta and are listed in a per-render report.
- Windows only for 2.0.
- The CPU target is for correctness and tests, and can also render.

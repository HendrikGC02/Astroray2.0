# Licensing constraints for Astroray 2.0

Research date 2026-11-14. This is an engineering reading of licence texts, not legal advice. Tags: VERIFIED = I read the primary source (licence file, SPDX header, official page); REPORTED = secondary source or a search summary; UNVERIFIED = could not confirm.

## Question

What do the GPL, the Blender extensions platform, NVIDIA's SDK licences, and the licences of candidate dependencies, borrowable renderer code and scientific data allow or force for Astroray 2.0? Which licence choices are open for the engine core and the Blender addon, and which of them belong to the owner?

## Findings

### 1. What Blender and the GPL require of an addon

- Blender's source is mostly GPL-2.0-or-later. Examples: `source/blender/render/intern/engine.cc` and `node_shader_bsdf_principled.cc` carry `SPDX-License-Identifier: GPL-2.0-or-later`. VERIFIED: https://raw.githubusercontent.com/blender/blender/main/source/blender/render/intern/engine.cc
- The Blender Foundation's own Cycles addon Python (`intern/cycles/blender/addon/engine.py`, `properties.py`) and the C++ sync code `intern/cycles/blender/shader.cpp` are labelled Apache-2.0 inside the GPL tree. VERIFIED: https://raw.githubusercontent.com/blender/blender/main/intern/cycles/blender/addon/engine.py
  - A copyright holder may license its own bpy-using code permissively; the combined Blender binary is then GPL. This is not a precedent for third parties.
- The extensions platform Terms of Service (last updated 10 Aug 2026): VERIFIED: https://extensions.blender.org/terms-of-service/
  - 1.1 "Uploaded add-ons must be wholly compliant with the GNU General Public License, version 3 or later."
  - 1.2 Binary files and assets in the extension must be CC0.
  - 3.4 Extensions for internal/private use or a closed user group may not be listed.
  - 3.6 "Source code of the extension must be fully written in Python. Depending on external, pre-compiled packages is not allowed."
  - 3.5 No obfuscated code or byte code.
  - 5.1 "Extension must be self-contained and not load remote code for execution." 5.2 "Extension should not require any external functional components (other than Blender itself) that need to be downloaded from elsewhere to access any of its features, even if those features are considered optional." (5.2 says "should", not "must"; reviewers apply it as a rule in practice, which is REPORTED, not read.)
  - 3.10 No additional requirements (purchase, registration, keys) that block functionality.
  - 2.1 The name "Blender" must not appear in the extension name.
- The manual says the platform "only supports free and open source extensions"; add-on licence GPL-3.0-or-later; assets in add-ons CC0. VERIFIED: https://docs.blender.org/manual/en/latest/advanced/extensions/licenses.html
- Blender's licence page text is blocked (HTTP 403) for me. Quotes of it reached me via third parties, so REPORTED:
  - "Blender's Python API is an integral part of the software, used to define the user interface or develop tools for example"; published scripts must be shared under a GPL-compatible licence; software that "operates outside of Blender" and uses no Blender code or API calls can be licensed freely. Sources: https://support.superhivemarket.com/article/247-understanding-the-gnu-gpl-license and https://blenderartists.org/t/how-to-distribute-a-compiled-addon/1297499?page=2
- Blender developer brecht, as personal interpretation (not official policy): the FSF treats addons using the Blender Python API as derived works and "the Blender Foundation follows that"; the question is "not settled"; a permissive engine plus a GPL addon, with a text file saying which is which, is a workable layout. REPORTED: https://forum.blender.org/t/blender-gpl-licences-permissive-licenses/25273
- 2009 mailing-list post: Ton Roosendaal reports the FSF judged the Blender FAQ wording "essentially correct" and "not misleading"; the proposed FAQ says scripts using Blender Python calls are treated as combined with Blender, and a script that links non-GPL-compatible modules or programs makes the GPL apply to all components when distributed. Old (2009), and the FAQ text may have changed since. VERIFIED that the post says this: https://archive.blender.org/lists/bf-committers/2009-April/023171.html
- GPL FAQ on separate programs: "pipes, sockets and command-line arguments are communication mechanisms normally used between two separate programs", but "if the semantics of the communication are intimate enough, exchanging complex internal data structures, that too could be a basis to consider the two parts as combined". Modules in one executable, or designed to run linked in a shared address space, "almost surely" form one program. VERIFIED: https://www.gnu.org/licenses/gpl-faq.html
- Apache-2.0 code can go into GPLv3 works; the FSF has never considered Apache-2.0 compatible with GPLv2. VERIFIED: https://www.apache.org/licenses/GPL-compatibility.html
  - Blender is "GPL-2.0-or-later", so combination happens under GPLv3, which matches the platform's GPLv3-or-later rule.
- Slang's licence adds an explicit waiver for combination with GPLv2 software (Apache-2.0 WITH LLVM-exception). VERIFIED: https://raw.githubusercontent.com/shader-slang/slang/master/LICENSE
- Selling GPL addons is allowed; the GPL attaches on distribution. Marketplace articles state a proprietary-binary workaround is "rare" and costly. The documented example: a GPL Python part talking to a separate non-GPL converter program (Better FBX). REPORTED: https://support.superhivemarket.com/article/294-how-to-make-your-blender-addon-gpl-compliant
- Mailing-list/forum commentary (2012, 2013) disagrees on whether a GPL Python part plus a closed binary called from it is acceptable when shipped together. No staff ruling found. REPORTED via search summary; no primary source read.

### 2. How other engines structure it

| Engine | Structure | Licence evidence |
|---|---|---|
| Octane | Separate `OctaneServer` process; addon talks over TCP. Vendor says "To conform to GPL rules" it ships in two parts. | VERIFIED vendor doc: https://docs.otoy.com/blender/InstallationProcess.html. Forum thread has no Blender-staff ruling: https://forum.blender.org/t/octanerenderer-and-gpl/19188 |
| Radeon ProRender | Python addon plus native core library | Addon repo LICENSE is Apache-2.0 (AMD, 2020). VERIFIED: https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/RadeonProRenderBlenderAddon/master/LICENSE.txt. Core library licence UNVERIFIED. |
| LuxCore | In-process C++ core as Python module | Core Apache-2.0, BlendLuxCore GPL-3.0. VERIFIED: https://raw.githubusercontent.com/LuxCoreRender/LuxCore/master/COPYING.txt and https://raw.githubusercontent.com/LuxCoreRender/BlendLuxCore/master/LICENSE |
| appleseed / blenderseed | Core plus addon | README says all software MIT. VERIFIED: https://raw.githubusercontent.com/appleseedhq/blenderseed/master/README.md |
| Cycles | In-process, Apache-2.0 core in GPL Blender | VERIFIED above |
| Spectra Render | Closed, commercial; Python addon + out-of-process daemon | Per recon (`00-recon.md`); licence UNVERIFIED |
| Redshift | No Blender integration licence information found | UNVERIFIED |

None of these is a legal ruling. They show what is shipped and tolerated, not what is required.

### 3. NVIDIA: OptiX, CUDA, DLSS

- CUDA EULA v13.5 (page header "Last updated: January 26, 2026"; its footer says Nov 02, 2026, so the effective date is ambiguous). VERIFIED: https://docs.nvidia.com/cuda/eula/index.html
  - Attachment A lists redistributable components: CUDA Runtime, cuBLAS, cuFFT, cuRAND, cuSPARSE, cuSOLVER, NPP, nvJPEG, NVRTC, JIT linking library (nvJitLink), common device math functions (libdevice), "NVIDIA CUDA Driver Libraries", plus others (22 entries). Not nvcc.
  - Conditions: your application needs "material additional functionality" (1.1.2); distribution terms must be consistent with the Agreement; you must report known non-compliant use.
  - 1.2: "You may not use the SDK in any manner that would cause it to become subject to an open source software license" (examples: source disclosure, derivative-work licensing, free redistribution).
  - 1.1.6: "you may use the SDK to develop and test applications released under Open Source Initiative (OSI) approved open source software licenses."
  - The page does not say whether nvcc-produced PTX/cubin may be shipped. In practice Cycles ships compiled GPU kernels. Treat as UNVERIFIED as a legal matter.
- OptiX headers (header text itself: use or distribution "without an express license agreement from NVIDIA ... is strictly prohibited"): `NVIDIA/optix-dev` is public on GitHub; the headers carry `SPDX-License-Identifier: LicenseRef-NvidiaProprietary` and `OPTIX_VERSION 90100` (OptiX 9.1). VERIFIED: https://raw.githubusercontent.com/NVIDIA/optix-dev/main/include/optix.h
  - The repo's `LICENSE.txt` is the "DesignWorks ... (with distribution rights)" agreement, V.13.06.2017. VERIFIED: https://raw.githubusercontent.com/NVIDIA/optix-dev/main/LICENSE.txt. Its terms:
    - Incorporate into a product "in binary format only".
    - Run only on NVIDIA-specified hardware ("certain NVIDIA GPUs" if documentation is silent).
    - Recipients must be bound by "enforceable agreements" consistent with the licence.
    - Notify NVIDIA before commercial use (1.5).
    - Do not distribute as a stand-alone product.
    - Do not use "in a way that would make it subject to an Open Source License" (2.1(xii)).
  - The OptiX 9.1 SDK's own licence text was not read. NVIDIA staff said the embedded SDK licence is older and the online version governs. REPORTED: https://forums.developer.nvidia.com/t/optix-7-3-licensing-requirements/196368
  - Same thread (NVIDIA staff, OptiX 7.3 era), REPORTED: commercial use allowed; "The main licensing limitation is that you do not redistribute the OptiX SDK to third parties"; binary-only releases that do not distribute OptiX itself carry no further obligations beyond the licence; releasing source "may" add requirements; hosting the installer publicly "would not be permitted".
  - Blender developers (2020-2022), REPORTED: "You can't redistribute the OptiX SDK or headers"; official Blender builds do not bundle them; PTX is generated at build time (needs headers); the PTX-to-machine-code step runs on the user machine via the driver. A distro packager argued the headers end up transformed in the binary so a distributable open build is impossible; no ruling. https://forum.blender.org/t/blender-2-8-cycles-optix-packaging/12533 and https://forum.blender.org/t/distributing-an-optix-enabled-version-of-blender-in-ubuntu/17889
- NVIDIA RTX SDKs licence (covers DLSS, NGX, RTXGI, RTXDI, NRD), version March 14, 2024. VERIFIED: https://raw.githubusercontent.com/NVIDIA/DLSS/main/LICENSE.txt
  - 4(e): no use "in any manner that would cause it to become subject to an open source software license".
  - 2(c): distribute only under terms "at least as protective".
  - Supplement 1: DLSS/NGX "licensed for you to develop applications only for their use in systems with NVIDIA GPUs".
  - Supplement 3: cloud-service use is outside the licence except vGPU.
  - Supplement 4: notify NVIDIA before commercial release of an application "(including a plug-in to a commercial application)".
  - 19: for unsuitable terms contact nvidia-rtx-license-questions@nvidia.com.
- Streamline source is MIT (NVIDIA, 2023 header); the DLSS-G plugin is prebuilt-only; binary artifacts live in GitHub releases, not the repo. VERIFIED: https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/license.txt and https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/README.md. The DLSS DLLs themselves fall under the RTX SDKs licence above.
- Blender 5.3 ships a viewport-only "DLSS denoiser" needing NVIDIA driver 620+. VERIFIED: https://developer.blender.org/docs/release_notes/5.3/cycles/. Per a Feb 2026 Phoronix report, REPORTED, via search summary only: the PR loads NGX from the driver dynamically (like OptiX), but NGX may look for `nvngx_dlssd.dll` next to the executable, which raised redistribution concerns. https://phoronix.com/news/nvidia-dlss-blender. I could not read the PR (403).

### 4. Dependency licences

All VERIFIED by reading the repo LICENSE file or SPDX headers, 2026-11-14.

| Dependency | Licence | URL |
|---|---|---|
| OIDN | Apache-2.0 | https://github.com/RenderKit/oidn |
| Embree | Apache-2.0 | https://github.com/RenderKit/embree |
| OpenPGL | Apache-2.0 (needs TBB, Embree) | https://github.com/OpenPathGuidingLibrary/openpgl |
| Slang | Apache-2.0 WITH LLVM-exception | https://github.com/shader-slang/slang |
| MaterialX | Apache-2.0 | https://github.com/AcademySoftwareFoundation/MaterialX |
| OpenPBR spec + MaterialX reference graph | Apache-2.0 (README badge text says CC BY-SA; the link and LICENSE are Apache-2.0) | https://github.com/AcademySoftwareFoundation/OpenPBR |
| Adobe openpbr-bsdf | Apache-2.0 | https://github.com/adobe/openpbr-bsdf |
| portsmouth OpenPBR-viewer | MIT | https://github.com/portsmouth/OpenPBR-viewer |
| OpenVDB / NanoVDB | Apache-2.0 | https://github.com/AcademySoftwareFoundation/openvdb |
| OpenColorIO | BSD-3-Clause | https://github.com/AcademySoftwareFoundation/OpenColorIO |
| nanobind | BSD-3-Clause | https://github.com/wjakob/nanobind |
| Dr.Jit | BSD-3-Clause | https://github.com/mitsuba-renderer/drjit |
| Vulkan-Headers | Apache-2.0 OR MIT | https://github.com/KhronosGroup/Vulkan-Headers |
| Vulkan-Loader | Apache-2.0 (exceptions "more permissive") | https://github.com/KhronosGroup/Vulkan-Loader |
| SPIRV-Tools | Apache-2.0 | https://github.com/KhronosGroup/SPIRV-Tools |
| glslang | multi-licence file (BSD/MIT/Apache family; not itemised here) | https://github.com/KhronosGroup/glslang |
| VMA | MIT | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator |
| Streamline source | MIT (see section 3) | |

- The LunarG Vulkan SDK bundles some LGPL-3.0 components in its licence listing; the SDK's own grant text was not read. VERIFIED partially: https://vulkan.lunarg.com/software/license/vulkan-1.4.341.0-windows-license-agreement.txt. Avoid bundling the SDK; use headers, loader and the user's GPU driver.
- OIDN trained weights: the README says the library is Apache-2.0 and ships "pre-trained models"; a separate weights licence is not stated. VERIFIED the README line; separate weights licence UNVERIFIED. https://raw.githubusercontent.com/RenderKit/oidn/master/README.md
- OpenVDB's Apache-2.0 at v12.0.0 matches v1's `THIRD_PARTY.md`. I did not check older tags.

### 5. Code that may be borrowed

Cycles (`blender/cycles`, commit 6abef8c6, 2026-11-11). VERIFIED by cloning and scanning SPDX headers under `src/`: https://github.com/blender/cycles
- Repo LICENSE is Apache-2.0. `src/` has about 875 Apache-2.0 files, 58 BSD-3-Clause, 3 MIT, 2 Zlib, and 1 GPL-2.0-or-later (`src/cmake/dependency_targets.cmake`, not kernel code).
- BSD-3-Clause kernel files are mostly "Adapted code from Open Shading Language" with Sony Pictures Imageworks copyright: `kernel/closure/bsdf_microfacet.h`, `bsdf_diffuse.h`, `bsdf_hair.h`, `kernel/svm/wavelength.h`, `blackbody.h`, `noise.h`, `kernel/sample/mis.h`, others. Keep their notices.
- `third_party/sky/` headers are BSD-3-Clause, MIT or Apache-2.0, including `sky_nishita.h`. (Author's scan; the verification pass could not fetch `third_party/sky/sky_nishita.h` at the guessed path (404; files sit under `include/` and `source/`), so re-check per file before porting.) `src/kernel/sample/mis.h` was confirmed BSD-3-Clause.
- Not in this repo: Blender-tree code. `source/blender/nodes/shader/nodes/node_shader_bsdf_principled.cc` is GPL-2.0-or-later (VERIFIED). Do not copy from `blender/blender` unless the file's header says Apache-2.0.
- Apache-2.0 sections 4(a)-(d): ship the licence, mark changed files, retain notices, carry NOTICE content. Porting maths with citation is the pattern v1 used.

PBRT-v4: Apache-2.0, headers "Copyright 1998-2020 Matt Pharr, Wenzel Jakob, Greg Humphreys"; spectrum code includes PhysLight code by Weta Digital (also Apache-2.0). VERIFIED: https://raw.githubusercontent.com/mmp/pbrt-v4/master/LICENSE.txt and https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/util/spectrum.cpp. The Jakob-Hanika generator `rgb2spec_opt.cpp` in pbrt-v4 is Apache-2.0. VERIFIED: https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/cmd/rgb2spec_opt.cpp

Mitsuba 3: BSD-3-Clause plus a non-standard "Enhancements" paragraph. Anyone who voluntarily publishes enhancements without a separate licence grants the author a perpetual licence to use and sublicense them. Borrowing code is fine; do not contribute changes back without reading that paragraph. VERIFIED: https://raw.githubusercontent.com/mitsuba-renderer/mitsuba3/master/LICENSE

### 6. Data

| Data | Licence / terms | Citation | Source |
|---|---|---|---|
| Jakob-Hanika 2019 code (rgb2spec) | BSD-3-Clause, "Copyright (c) 2020 Wenzel Jakob". Paper page states no licence for supplemental data. | Cite the paper (Jakob and Hanika, CGF 38(2), 2019) | VERIFIED: https://raw.githubusercontent.com/mitsuba-renderer/rgb2spec/master/LICENSE ; https://rgl.epfl.ch/publications/Jakob2019Spectral |
| Precomputed coefficient tables (`.coeff`) | No licence statement found for the binary tables. v1 claims Apache-2.0 via a third-party mirror. Tables can be regenerated from the BSD-3/Apache-2.0 optimiser. | | UNVERIFIED for the blobs |
| CIE CMFs, illuminants | "Copyright (c) 2026 CIE"; no licence stated on the dataset pages; "Other distributors are not authorized" appears in a webshop notice. | Cite "CIE 2019, Colour-matching functions of CIE 1931 standard colorimetric observer", DOI 10.25039/CIE.DS.xvudnb9b | VERIFIED: https://cie.co.at/datatable/cie-1931-colour-matching-functions-2-degree-observer ; https://cie.co.at/publications/open-access-datasets |
| refractiveindex.info | CC0 1.0; commercial use without permission | Asks to cite Polyanskiy, Sci. Data 11, 94 (2024), doi 10.1038/s41597-023-02898-2, plus original papers named in each file | VERIFIED: https://www.refractiveindex.info/about ; https://raw.githubusercontent.com/polyanskiy/refractiveindex.info-database/main/README.md |
| NIST ASD (SRD 78, v5.12) | "copyright by the U.S. Department of Commerce on behalf of the United States. All rights reserved"; NIST "reserves the right to charge"; no redistribution grant on the pages read. NIST's SRD page: the Secretary of Commerce secures copyright in SRD; licences are per product. | DOI 10.18434/T4W30F; Kramida, Ralchenko, Reader and NIST ASD Team | VERIFIED: https://www.nist.gov/pml/atomic-spectra-database ; https://www.nist.gov/open/copyright-fair-use-and-licensing-statements-srd-data-software-and-technical-series-publications |
| CHIANTI | No licence text found on the pages read; the site asks for acknowledgement. Redistribution terms UNVERIFIED. The site's paper list stops at v10 "submitted", so the page may be stale. | Cite Dere et al. 1997 plus the paper for the version used; acknowledge the collaboration (GMU, Michigan, Cambridge, NASA GSFC); when incorporated into other databases, state the version | VERIFIED: https://www.chiantidatabase.org/referencing.html |
| ExoMol | CC BY-SA 4.0 | Cite the relevant ExoMol articles per dataset | VERIFIED: https://www.exomol.com/data/licence/ |
| Cloudy | FreeBSD-style licence in `license.txt`; "use of this program is not restricted provided each use is acknowledged" | Cite the release paper for the version used; `print citation` command exists | REPORTED via search summary of C23 Hazy docs: https://data.nublado.org/workshop/15Belfast/Ferland.G98CLOUDY-90--Numerical-Simulation-of-Plasmas.pdf (older doc) |

ShareAlike on ExoMol applies to adaptations of the data; a table derived from it (cross-sections, opacity grids) is likely an adaptation. Whether code that merely reads the data is covered is not settled here.

### 7. What v1 chose

- `LICENSE`: MIT, "Copyright (c) 2026 HendrikGC02". VERIFIED (read-only): `<v1 checkout>\LICENSE`
- `THIRD_PARTY.md` lists:
  - Jakob-Hanika `.coeff` (9.4 MB, taken from `geometrian/simple-spectral`, claimed Apache-2.0).
  - CIE CMF and D65 tables generated via Colour-Science (BSD-3), "public domain (CIE standards)".
  - stb (MIT / public domain).
  - A Cycles BSSRDF port (Apache-2.0, per-function citations).
  - A vendored reference copy of Cycles light-tree code (Apache-2.0).
  - NanoVDB v12.0.0 subset (Apache-2.0).
- It does not mention CUDA, OptiX, or any NVIDIA SDK terms, and it has no data-pack provenance for NIST/CHIANTI/ExoMol. The CIE "public domain" claim is not supported by the CIE pages above.
- v1's addon licence is not stated in the files I read. UNVERIFIED.

## Options and tradeoffs

The axes are independent. I list the forks that really exist.

**A. How the engine reaches Blender (licence consequences only)**
- In-process native module (a `.pyd` loaded by Blender's Python): by the GPL FAQ test this is one program with Blender (shared address space). Fine if the module's licence is GPL-compatible; MIT, BSD and Apache-2.0 all qualify (LuxCore, Cycles). It cannot be closed source if shipped in one package with a GPL addon (contested; unresolved by any staff statement).
- Out-of-process daemon over pipes or sockets: the GPL FAQ treats this as normally separate programs, so the engine may carry any licence, unless the protocol exchanges "intimate" internal structures. The addon is still GPL. Octane documents this structure.
- Distribution channel is the same for both: extensions.blender.org rejects each (3.6 forbids compiled packages; 5.2 forbids external functional components). The engine ships from the owner's own repository or site.
- Neither choice by itself constrains the owner's own relicensing: a sole copyright holder can offer the same code under several licences. That changes once third-party GPL code or outside contributions are merged.

**B. Engine core licence**
- MIT / BSD-3: simplest, v1 precedent, GPLv3-compatible, no patent grant. Allows anyone, including a closed commercial fork, to reuse.
- Apache-2.0: same practical permissions, adds a patent grant and NOTICE handling; matches Cycles, pbrt, OIDN, Embree, OpenPBR, MaterialX, Slang. Incompatible with GPLv2-only, irrelevant here since Blender is GPLv2-or-later and the platform wants GPLv3+.
- GPL-3.0-or-later: no closed forks; matches the addon. Conflicts with NVIDIA clauses (CUDA EULA 1.2, DesignWorks 2.1(xii), RTX 4(e)) wherever NVIDIA SDK material ends up in the shipped GPL work; Blender developers disputed this for OptiX and no one resolved it. Also blocks some borrowed code reuse in the opposite direction (GPL code cannot go into the Apache/MIT core).
- Dual licence (GPL plus commercial) or proprietary / source-available: possible if the owner keeps all copyrights (needs a contributor policy such as a CLA); worse fit with the "linking NVIDIA binaries" and "research reuse" goals.

**C. Addon licence**
- GPL-3.0-or-later pure Python: required for extensions.blender.org, defensible on every reading, thin by design.
- GPL-2.0-or-later (Blender's own): allowed in the wild; the platform wants GPLv3+.
- Permissive or proprietary addon: appleseed, RPR and Cycles show permissive bpy-using Python exists; the Foundation's stated rule (REPORTED) says API-using scripts must be GPL-compatible, so closed is off the table, permissive is contested and unavailable on the official platform.

**D. NVIDIA coupling**
- NVIDIA-only (OptiX/CUDA): OptiX headers are proprietary and not redistributable in source; the licence binds to NVIDIA hardware, asks for notification and end-user pass-through terms. Practical pattern: fetch headers at configure time, ship only compiled kernels, load driver libraries dynamically (Cycles uses `cuew` for this: `third_party/cuew` in the Cycles repo).
- Vendor-neutral (Vulkan, Slang, OIDN): all components are Apache/MIT/BSD; no per-SDK end-user terms, no notification duty, no hardware lock. Decide the GPU backend in the GPU workstream; this one only notes that licensing favours the vendor-neutral stack.
- Denoise: OIDN is Apache-2.0 and unconstrained. DLSS-RR carries the RTX SDKs licence (NVIDIA GPUs only, notify before release, pass-through terms); only the driver-hosted route Blender uses avoids shipping NVIDIA DLLs.

**E. Data**
- Vendor into the repo only what has a clear licence (refractiveindex.info CC0, generated JH tables). Everything else (NIST ASD, CHIANTI, ExoMol, CIE tables) goes into separately obtained data packs with a manifest listing licence and citation.

## Recommendation for Astroray 2.0

These rest on the evidence above. Items marked (owner) are not mine to settle.

1. **Addon: GPL-3.0-or-later, pure Python, thin.** Evidence: ToS 1.1; Blender source is GPL-2.0-or-later; the Foundation's FAQ position as quoted. Keep engine code out of the addon directory and put a `LICENSE` file in each directory stating which licence applies, as brecht suggested.
2. **Engine core: a permissive licence, Apache-2.0 by default (owner).** Evidence: most borrowable code (Cycles, pbrt-v4, OpenPBR, MaterialX, Slang, OIDN) is Apache-2.0; Apache is GPLv3-compatible; the engine can run in-process or out-of-process under it. MIT is an acceptable alternative (v1's choice). Do not choose GPL for the core unless the owner accepts the NVIDIA-clause conflict (3 clauses in section 3).
3. **Do not distribute through extensions.blender.org.** ToS 3.6 (fully Python, no external pre-compiled packages) excludes a bundled compiled engine; 5.2 (no externally downloaded functional components; "should") excludes a separately downloaded daemon or engine. Use a self-hosted extension repository (manifest supports `platforms` and `wheels`; remote repositories take a JSON index, REPORTED) plus GitHub releases. Use a name without "Blender" in it (2.1).
4. **NVIDIA material:**
   - Do not commit OptiX headers. Fetch `NVIDIA/optix-dev` at configure time (works without a developer login) and ship only compiled kernel output.
   - Never ship `nvngx_dlss*.dll`, Streamline DLLs or the OptiX SDK.
   - Link `cudart` statically, or ship only Attachment A items. Load the driver libraries dynamically.
   - Make OIDN the default denoiser.
   - Keep the product open and document the open-source clauses as a risk in `THIRD_PARTY.md`.
   - Send NVIDIA the notification forms if the project is ever released commercially (OptiX 1.5, DLSS supplement 4).
5. **Borrowing policy.** Port from Apache-2.0, BSD and MIT files only, after reading the file's SPDX header. Never copy from the Blender tree. Record each borrowed file with source commit SHA and licence in `THIRD_PARTY.md`; keep NOTICE and OSL/Sony copyright lines. Add a CI check that every source file carries an SPDX header.
6. **Data.** Regenerate the Jakob-Hanika tables from the BSD-3/Apache-2.0 optimiser instead of vendoring a blob of unclear licence. For CIE data, record a source with a stated terms; do not claim "public domain". Ship refractiveindex.info (CC0) with the citation. Keep NIST ASD, CHIANTI, ExoMol and Cloudy outputs out of the repo; use data packs with manifest entries (SPDX id or URL, citation text, version).
7. **Contributor policy (owner).** Decide DCO or CLA before the first outside contribution, because it fixes whether the owner can later dual-license or relicense.
8. **Out-of-process vs in-process** is an architecture choice made elsewhere. If the owner wants a possible closed commercial engine later, the out-of-process daemon with a data-only protocol is the licence-safest shape; otherwise licensing does not decide it.

### Choices that are the owner's

- Whether any commercial use of the engine is intended (sale, closed fork, hosted service). The recon notes this is unanswered.
- Core licence (item 2) and contributor policy (item 7).
- Whether to accept GPL-with-NVIDIA-SDK risk (only matters if the core goes GPL).
- Whether to seek written answers from NVIDIA (end-user pass-through terms for shipped kernels; header redistribution) and NIST (redistribution of ASD data).
- Whether an open-source release is intended at all in the near term.

## What a measurement spike must settle

1. Clean-machine build of a minimal prototype that fetches `NVIDIA/optix-dev` (no account) and produces PTX/OptiX-IR; confirm no NVIDIA header text ships in the artifact.
2. `dumpbin /dependents` (or equivalent) on every shipped binary: confirm only driver DLLs (`nvcuda`, `nvoptix`) and permissive-licensed libraries are loaded; confirm `cudart` is static.
3. Run an SPDX scanner (`reuse lint` or ScanCode) over the prototype's full dependency closure and emit the `THIRD_PARTY.md` automatically; any file without an identifiable licence fails.
4. Install a self-hosted extension repository containing a platform wheel with a `.pyd` into a clean Blender 5.3 profile; confirm install, enable and load without the official platform.
5. Verify the JH tables regenerate bit-compatibly (or within tolerance) from `rgb2spec_opt.cpp` at the pinned version; time the generation.
6. Send the written questions in the owner's list above and record dated replies in `docs/decisions/`.
7. If DLSS-RR is ever wanted, test the driver-hosted NGX route on driver 620+ without placing any DLL beside the executable.

## Unverified or conflicting

- Blender's licence page and FAQ (blender.org) returned HTTP 403; every quote of them here is second-hand. The platform manual and ToS wording (GPLv3-or-later, "wholly compliant") is stricter than the 2009-era "GPL-compatible" FAQ text, and a Debian copy of the 4.3 manual is looser than the current English manual (REPORTED). Manual issue #104850 asked whether GPL is "required" or merely "compatible" (https://projects.blender.org/blender/blender-manual/issues/104850, page blocked).
- Whether a GPL addon plus a closed binary shipped together is allowed: no Blender-staff answer; brecht's view is explicitly personal.
- Whether an in-process permissive `.pyd` is legally "one program" with Blender: GPL FAQ says "almost surely"; Cycles and LuxCore do exactly this with Apache-2.0 cores.
- Whether shipping OptiX-compiled kernels requires end-user pass-through terms (DesignWorks 1.1(ii)(b)) in an open-source product: NVIDIA's staff answer is informal and for OptiX 7.3. The 9.1 SDK licence text was not read.
- Whether DesignWorks 2.1(xii), CUDA EULA 1.2 and RTX 4(e) bite on a GPL-licensed engine core: Blender developers argued both ways in 2022, no resolution.
- The `.coeff` blobs' licence (v1 says Apache-2.0 from a mirror; the original code repo is BSD-3; no licence on the paper page). The CIE "public domain" claim in v1.
- v1 memory says only `sky_nishita.h` and `sky_math.h` in Cycles' sky sources are GPL; at Cycles commit 6abef8c6 both are BSD-3-Clause. The old finding may predate a relicensing, or concern the Blender tree. Not resolved.
- OIDN weights licence; OpenVDB licence before v12; Redshift, RPR-core, Spectra Render licences.
- NIST ASD redistribution rights; CHIANTI redistribution rights (site content may be stale); Cloudy current licence text (summary of old Hazy docs only).
- How Blender 5.3's DLSS denoiser is actually distributed (PR #153077 unreadable; Phoronix summary only).
- Microsoft VC++ runtime and Python redistribution terms: not researched.

## Verification pass

Independent check, 2026-11-14. Primary sources re-read; no build or run.

1. Extensions ToS (1.1, 1.2, 2.1, 3.4, 3.5, 3.6, 3.10, 5.1, 5.2, "Last updated: 10 August 2026"): CONFIRMED verbatim. CORRECTED: 5.2 says "should not", not "must not"; 5.1 is the "no remote code" clause, so the researcher's "5.1/5.2" paraphrase was loose. Conclusion that a compiled engine cannot be listed still holds via 3.6. https://extensions.blender.org/terms-of-service/
2. OptiX: CONFIRMED `optix.h` is `LicenseRef-NvidiaProprietary`, `OPTIX_VERSION 90100`, header text forbids use without an express licence. CONFIRMED optix-dev LICENSE.txt is the DesignWorks agreement V.13.06.2017: binary-format-only (1.1(ii)), NVIDIA-specified hardware (1.1(i)), enforceable recipient agreements (1.1(ii)(b)), prior notification (1.5, breach if skipped), open-source clause 2.1(xii), no stand-alone distribution (2.1(vii)). Reading of header-only redistribution as outside the licence is consistent with the text. COULD NOT VERIFY that this 2017 text is the agreement governing the 9.1 SDK. https://raw.githubusercontent.com/NVIDIA/optix-dev/main/LICENSE.txt ; https://raw.githubusercontent.com/NVIDIA/optix-dev/main/include/optix.h
3. CUDA EULA v13.5 1.2 and 1.1.6, 1.1.2: CONFIRMED. CORRECTED: page header date (26 Jan 2026) conflicts with footer (2 Nov 2026). RTX SDKs licence 4(e), 2(c), Supplement 1 (NVIDIA GPUs only), Supplement 4 (notify), v. March 14, 2024: CONFIRMED. https://docs.nvidia.com/cuda/eula/index.html ; https://raw.githubusercontent.com/NVIDIA/DLSS/main/LICENSE.txt
4. GPL FAQ: CONFIRMED quotes. CORRECTED nuance: "almost surely" is stated for modules "designed to run linked together in a shared address space"; plug-ins have a separate FAQ entry, not read in full. Precedent claims (Cycles, LuxCore, Octane) not re-fetched: kept as VERIFIED by the author; Blender-staff ruling still absent. https://www.gnu.org/licenses/gpl-faq.html
5. Licences: CONFIRMED Cycles repo LICENSE is Apache-2.0; `mis.h` BSD-3-Clause with Sony Pictures Imageworks copyright; Mitsuba 3 BSD-3 plus Enhancements clause (perpetual non-exclusive sublicensable grant); Slang Apache-2.0 WITH LLVM-exception including a GPLv2 combination waiver; Apache-2.0 into GPLv3 allowed, never deemed GPLv2-compatible by FSF. https://www.apache.org/licenses/GPL-compatibility.html ; https://raw.githubusercontent.com/shader-slang/slang/master/LICENSE ; https://raw.githubusercontent.com/mitsuba-renderer/mitsuba3/master/LICENSE ; https://raw.githubusercontent.com/blender/cycles/main/LICENSE. COULD NOT VERIFY in this pass: commit 6abef8c6, the 58 BSD-3 file count, `sky_nishita.h` header (path guess 404), pbrt-v4, OIDN, Embree, OpenPGL, MaterialX, OpenPBR, OpenVDB, OCIO, nanobind (not re-fetched; left as the author's VERIFIED claims).
6. CUDA Attachment A contents: CONFIRMED (22 entries, includes runtime, math libs, NVRTC, JIT linking, device math functions, driver libraries; nvcc absent).

Recommendation after verification: unchanged. Addon GPL-3.0-or-later pure Python; engine core permissive (Apache-2.0 default, owner decides); distribute outside extensions.blender.org; keep NVIDIA SDK material out of the repo and artifacts, fetch OptiX headers at configure time, OIDN default.

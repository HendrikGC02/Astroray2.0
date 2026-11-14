# 022 CIE observer data, specgen observer part and data/MANIFEST.toml
Goal: there is no observer -> the CIE 1931 2-degree tables come from pbrt-v4 at a pinned commit with recorded provenance, `tools/specgen` generates Y_n, K, the D65 illuminant and the Rec.709 matrix into one hashed data file, the data is embedded in the DLL, and lint checks the data manifest.
Read first: 0003 d11 (Y, anchor, K), d14 (one observer, source, generated together), d15 (working space), d18 (table generation, committed, not in default build); 0001 D8.1-D8.4 (data policy, CIE provenance, manifest), V9 (file part).
Scope: tools/specgen/specgen.cpp, tools/specgen/fetch_pbrt_cie.py, data/spectral/cie1931_2deg.csv, data/spectral/observer.bin, data/MANIFEST.toml, cmake/data.cmake (embedding), tools/lint.py (section `data`), tests/cpu/lint_cases/data/, tests/cpu/test_observer.cpp.
Out of scope: Jakob-Hanika tables (023); Rec.2020 and AP1 (M4); the film's response conversion (046).
Reference: pbrt-v4 `src/pbrt/util/spectrum.cpp` arrays `CIE_X`, `CIE_Y`, `CIE_Z`, `CIE_Illum_D6500` (Apache-2.0); CIE dataset DOI 10.25039/CIE.DS.xvudnb9b; ITU-R BT.709-6 primaries and D65 white; CIE 018:2019 (K_m = 683 lm/W).
Precondition: owner question 0001 q5 (CIE data) is answered "ship with the recorded risk"; otherwise stop and report.
Do:
1. fetch_pbrt_cie.py downloads spectrum.cpp from raw.githubusercontent.com at a pinned commit, extracts the four arrays (360-830 nm, 1 nm, 471 samples) and writes cie1931_2deg.csv; MANIFEST.toml records source URL, commit, the CIE DOI, a licence statement per 0001 D8.2 (never "public domain"), SHA-256 and the command.
2. specgen.cpp (target `specgen`, EXCLUDE_FROM_ALL; double precision): Y_n = sum of ybar at 1 nm (must equal 106.856895); K = 683 * Y_n; D65 SPD scaled so Y = 1; M_ws for lin_rec709_scene computed from the BT.709 primaries and D65 white chromaticities, no chromatic adaptation. Writes data/spectral/observer.bin: header {magic, version, observer_hash (first 8 bytes of SHA-256 of the CMF float data), counts}, then CMFs (float), Y_n and K (double), D65 SPD, M_ws Rec.709 (double 3x3). Run only by hand: `pwsh tools/build.ps1 -Target specgen` then `out/bin/specgen.exe observer`.
3. cmake/data.cmake embeds every data file listed in MANIFEST.toml into astroray_core as RCDATA; src/host/data_resource.{h,cpp} loads by name and refuses a set whose observer_hash values differ (0003 d14).
4. lint section `data` (0001 V9 file part): every file under data/ except MANIFEST.toml and LICENSE has a manifest entry with SPDX or URL, citation, version, SHA-256 matching the file, and command.
5. tests/cpu/test_observer.cpp (fast), reading data files by path: check 1 for Rec.709 (D65 through CMFs and M_ws gives (1,1,1) within 5e-4); check 2 (Y_n = 106.856895 within 1e-6 relative; hash consistent); check 11 K part (K = 683 * Y_n to 1e-9).
Acceptance: `out/bin/specgen.exe observer` reproduces observer.bin byte for byte; `pwsh tools/check.ps1 fast` passes; editing one byte of observer.bin makes lint fail naming the file.
Look at: none.
Canary: tasks/022.canary.patch must turn red: astroray_tests_cpu "observer/d65 white is 1 1 1", "observer/Y_n value".
Budget: specgen run time recorded in Progress (0009 B-37 specgen row).
Depends on: 018, 008.
Size: M (about 400 lines); stop and report at 2x.
Progress:

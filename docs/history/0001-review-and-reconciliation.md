# 0001: review and reconciliation

Moved unchanged from the end of `docs/decisions/0001-product-shape.md` (ruling 86). History only: nothing here is binding (ruling 50), and the record's Decision section is current.

## Review and resolution

History of the independent review; the Decision section and the Reconciliation section below supersede it where they differ.

1. Sticky CUDA error: ACCEPTED. D2.5 now says CUDA is finished for the process, the UI says restart Blender, the CPU device keeps working, and it counts toward D6 (b). D1.4 makes the CLI and the test executables the main development host. Added V14 and an alternatives row.
2. ABI versioning: ACCEPTED. `AR_ABI_HASH` is a SHA-256 of the normalised header. The layout table is generated as `sizeof`/`offsetof`, so MSVC and ctypes compute it independently. A separate function-signature table was not added, because the hash already changes on any signature change (V1 tests this). Its only gain would be naming the function, and the remedy, a rebuild, is the same.
3. Generated binding and recorder: ACCEPTED. D2.10 defines the grammar, the annotations `AR_IN`/`AR_IN_ARRAY`/`AR_OUT`/`AR_STAGED`/`AR_QUERY`, the recording of inputs only, and replay to N samples. V2 and V11 are updated.
4. OIDN: ACCEPTED, with a different remedy. dumpbin on Blender 5.3's copy confirms the CPU device imports `tbb12.dll`. We now use our own OIDN build with renamed modules, static CRT and static oneTBB, shipped in both zips and required from the start with V5. The Blender OIDN version check at load time was not added: renamed modules make Blender's OIDN version irrelevant.
5. D6 evidence: ACCEPTED. Both ratios (1.1x and 1.5x) are quoted, the caveats on synthetic present and omitted sync are stated, and owner decision 9 is recorded as unproven. Spine challenge 3 is added.
6. Failure handling: ACCEPTED. D2.5 adds the per-context log file, `__try/__except` with a minidump on engine threads and no global handler, ctypes' `OSError` on the calling thread, and a launch limit under TDR. The last is a constraint on 0002 and 0004; its numbers are now 0009's (ruling 14).
7. "Single front end": ACCEPTED. D4.2 and "Why not Rust" are reworded around owner decision 2 and the measured `-fmad=false` agreement.
8. Multi-architecture compile: ACCEPTED in substance. The compile runs in `check full` on demand (V13); nothing is scheduled (G3).
9. Citations: ACCEPTED, all five. The Cycles per-file licences are stated. The CIE terms are restated and made owner question 5. The LTC example is removed, and third-party generators need a recorded licence. 35216 is corrected to the measured shipped runtime 14.44.35211. The DLSS floor is deferred to 0008 and spike F.
10. Interpreter and `_abi.py` licence: ACCEPTED. Blender's bundled `python.exe` runs the tools (D4.1), and `_abi.py` carries Apache-2.0 (D1.3, D7.1, V4).
11. Update, lock and tables: update and uninstall ACCEPTED (V12, D10.5). Generated tables ACCEPTED: committed and outside the default target (D8.4, per 0003). Dev shadow copy REJECTED: Blender always loads the copy inside an installed extension (0009 17a installs the staged zip into an isolated profile), never `out/bin`, so the lock never blocks a rebuild. Reinstalling into a running Blender is the update case that V12 covers.

## Reconciliation (rulings applied)

- G1: "What lands when" reduced to the two lists; milestone assignments removed from D9.4, V5, Risks 1, 2 and 6 and review item 4; D10.1 points to `docs/ROADMAP.md`.
- G2: time budgets (launch length, compile gates) and the first-frame gate cited from 0009 and 0007 instead of restated; the D6 soak threshold cited from 0007 decision 44; the DLSS floor number removed (0008).
- G3: V13 and review item 8 run in `check full` on demand; nothing nightly.
- G4: no reserved hooks remain; the AMD-port "hook" wording in D5.2 reduced to the cost of the interface the CPU device already needs.
- Ownership (report): D2.11 added with the entry schema, levels, codes, deduplication, lifetime and polling; D2.2 allows `uint8_t` text arrays for it; V15 tests it; D2.5, D9.4 and V14 name the codes they raise.
- Ownership (function lists): D2.9 and D2.10 give scene-edit and render-control functions to 0007 instead of 0004.
- Ruling 3: D2.10 and the 0003 assumption cite 0003 for the sampler, with no special seed.
- Ruling 6: cross-reference to 0003's table generation corrected to decision 18.
- Ruling 10: D2.9 and the 0007 assumption bind every record to `astroray.h`, the D2.1 handles and `ar_status`.
- Ruling 11: the 0006 assumption names sparse 8³ leaves.
- Ruling 12: OIDN at one pinned version, built once as a release asset outside the normal build (D9.1, D9.2).
- Ruling 13: D1.1 embeds release kernels and allows loose modules in development builds only.
- Ruling 14: D2.5 and V14 cite 0009's launch target and hard cap.
- Ruling 15: D3.6, D3.7, D2.8 and V6 require AVX2 for the CPU device; Risk 11 added.
- Ruling 16: NVRTC removed from D4.2, D4.3, D9.3, "Why not Slang" and the 0002 assumption; D11 lists runtime kernel compilation; V3 forbids `nvrtc*` includes.
- Ruling 17: V13 checks capped-build spills and stack; uncapped registers are warnings.
- Ruling 19: the VRAM reserve rule is attributed to 0007 in the assumptions.
- Ruling 20: stb_image and tinyexr added to the D9.1 allow-list as the only texture decoders; the tinyexr-as-EXR-writer entry removed (0008 writes no EXR), then restored by ruling 34 below.
- Ruling 21: D6 and Risk 6 measure against 0007's first-frame gate instead of CHECKPOINT-1's 2x.
- Ruling 22: D9.1 states that mipmaps are generated by our own code, needing no library.
- Ruling 29: launch length, V14 and the 0002 assumption cite 0009 B-17; "Why not Slang" cites B-01 and B-05.
- Ruling 31: CUDA, MSVC, OptiX and Python versions cited from 0009 decision 2 (D3.5, D4.1-D4.3, D9.1, Assumptions); measured evidence keeps its versions.
- Ruling 32: D4.1 runs tools on Blender's bundled Python and NumPy with no separate environment.
- Ruling 33: `ar_edit` handle and device selection on `ar_context` (D2.1, D2.8); `ar_key` typedef (D2.2, D2.10); `AR_OUT_ARRAY` annotation (D2.10); D2.9 gives readback lists and descriptors to 0007 and readback contents to 0008.
- Ruling 34: MikkTSpace, tinyexr reading and writing, and the fetched NGX headers on the D9.1 list; D9.2 fetches the NGX headers.
- Ruling 37: D2.10's replay argument uses the epoch.
- Ruling 38: D1.4 lists `dump` with info, render and bench.
- Ruling 39: D5.2 states the include rule (CUDA/OptiX in `src/device/` and `src/kernel_gpu/`, DLSS in `src/denoise/dlss/`).
- Ruling 40: D2.11 gains `KERNEL_MODULE_FALLBACK`, `UNTESTED_DEVICE`, `WORKING_SPACE_UNSUPPORTED`, `LARGE_WORLD_PRECISION`, `GRATING_TABLE_LIMIT`, `SKY_DISC_NOT_SEPARABLE`, `CASTER_RAY_TYPE_DEPENDENT`, `CRYPTOMATTE_OVERFLOW`, `DLSS_DOF_UNGUIDED`, `MEDIUM_PATH_LIMIT`, `MEDIUM_MESH_OPEN`; existing codes widened to the other conditions raised in 0002-0008, raised-by columns corrected.
- Ruling 41: the 0002 assumption says the double CPU build is not shipped.
- Ruling 48: `DELTA_LIGHT_PATH_GAP` is a WARNING (owner decision 25).
- Ruling 49: Review section headed as history; owner questions 3 and 6 answered by owner decision 29 (D3.4, D6 (b)).
- Ruling 33 (second pass): D2.1 lists `ar_context_desc`, `ar_device_info`, device enumeration and context create/destroy; D2.8, D3.5, D2.5 and V6 report every unavailable device as `AR_ERR_DEVICE` (synchronous checks at enumeration and context creation, asynchronous init failures per session).
- Ruling 29 (second pass): spike times in D2.5, D4.2, "Why not Slang", D6 and Risk 6 replaced by 0009 B-ids and ratios.
- Ruling 48 (second pass): `DELTA_LIGHT_PATH_GAP` no longer names a camera type (every camera type gets light-pass connections, 0004 D24).
- Ruling 49 (second pass): D6 (b) states the soak's own criterion instead of a circular threshold in 0007 decision 44; D2.5's assertion text no longer cites it; D2.11 names 0007 decisions 15 and 47 as host diagnostics; `MEDIUM_PATH_LIMIT` covers 0005's random-walk limit; the ruling-20 line notes ruling 34's tinyexr entry.

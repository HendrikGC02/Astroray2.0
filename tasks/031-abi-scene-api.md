# 031 Scene, edit, session, display and readback declarations in astroray.h
Goal: the header lacks 0007's function list -> every function of 0007 d5, the report polling functions and every descriptor of the ABI appendix are declared, bindings regenerate, unimplemented functions fail loudly, and the descriptor fields for meshes, instances, point and area lamps are decided and written back into the appendix.
Read first: 0007 d4-d9 (conventions, function list, staging, layouts, graph transport, validation), docs/decisions/0007-appendix-abi.md (all), 0001 D2.2-D2.3, D2.10, D2.11 (polling functions), R33, R63 (array counts and light descriptor fields: designed when built).
Scope: include/astroray/astroray.h, src/capi/unimplemented.cpp, docs/decisions/0007-appendix-abi.md, tests/abi/test_unimplemented.py.
Out of scope: implementations (032 onward); the recorder (033).
Reference: none.
Do:
1. Declare every function of 0007 d5 with annotations; `ar_edit_update_transforms(ar_edit, uint32_t n, AR_IN_ARRAY(n, uint32_t) const uint32_t* idx, AR_IN_ARRAY(n, ar_xform) const ar_xform* xf)` with `ar_xform {float m[12];}` so every count names an integer parameter; display and readback functions are `AR_QUERY`; add `ar_session_report_info` and `ar_session_report_entry` (0001 D2.11) and `ar_scene_last_error`, `ar_session_last_error`, `ar_edit_last_error` like `ar_context_last_error`.
2. Declare every descriptor and staged-buffer struct of the appendix with its fields, and the constants it implies (`AR_SESSION_MODE_*`, `AR_STATE_*`, `AR_TARGET_*`, `AR_LIGHT_*`, `AR_IMAGE_SOURCE_*`, `AR_QUANTITY_*`, `AR_DISPLAY_*`, `AR_KIND_*` for `ar_edit_remove`, `AR_DENOISER_*`, `AR_SPECTRUM_*`, visibility and instance-flag bits).
3. Designed when built (R63): decide widths, order and array count fields for `ar_mesh_desc`/`ar_mesh_buffers`, `ar_positions_buffers`, `ar_instances_desc`, `ar_light_desc` (POINT radius = `size_x`, AREA_RECT `size_x` x `size_y`, AREA_ELLIPSE with equal sizes = disk) and `ar_spectrum_ref`; write the decided field lists into the appendix in this diff, replacing "fixed when astroray.h is written" for those rows. Other descriptors keep the appendix's field lists; their decision rows stay open for M4, M6 and M7 as the roadmap says.
4. src/capi/unimplemented.cpp: each declared function without an implementation returns AR_ERR_UNSUPPORTED and sets the handle's last error to `<function>: not implemented in this build`; later tasks delete entries as they implement them.
5. tests/abi/test_unimplemented.py: through `_abi.py`, `ar_edit_curves_begin` (curves land in M7) returns AR_ERR_UNSUPPORTED; every function the header declares is either exported by the DLL or listed in unimplemented.cpp (the test compares `_abi.py`'s function list with the DLL's exports).
Acceptance: `pwsh tools/check.ps1 gate` passes (abigen accepts the header; V1 test still passes with the new structs); `<python> tools/abigen.py --hash include/astroray/astroray.h` differs from the previous hash; the appendix diff names the decided fields for the five descriptor rows.
Look at: none.
Canary: tasks/031.canary.patch must turn red: tests/abi/test_unimplemented.py "TestUnimplemented.test_curves_begin_unsupported".
Budget: none.
Depends on: 013, 030.
Size: M (about 450 lines of header); stop and report at 2x.
Progress:

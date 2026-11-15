# 104 Session readback: progress, report polling, display frames, passes, counters and FITS
Goal: a running session is opaque to its host -> `ar_session_get_progress`, report polling, `ar_display_acquire`/`release`, pass enumeration and reads, counters and `ar_session_write_fits` expose 0008 d13a's readback through the ABI.
Read first: 0007 d5 (display and readback functions), d26 (polling), 0008 d13a (readback contents), d13 (film dump), 0001 D2.11 (report polling, scene entries merged into sessions); docs/decisions/0007-appendix-abi.md (`ar_progress`, pass descriptors).
Scope: src/capi/session_api.cpp, src/host/session/readback.{h,cpp}, src/host/session/session.{h,cpp} (lead ruling: additive readback surface; Session's film, ring and scheduler are private), src/capi/unimplemented.cpp, tests/cpu/test_readback.cpp, tests/abi/test_session_api.py.
Out of scope: scene-state readback (R59: none exists); denoised output (M1b); Blender passes beyond Combined (M1b).
Reference: none.
Do:
1. `ar_session_get_progress` fills `ar_progress` (state, active/pending/failed generation, epoch, spp, last pass time, ledger per category, device free/total).
2. Report polling: `ar_session_report_info`/`_entry` snapshot the session Report (016) merged with the scene's SCENE entries (032), in 016's snapshot order.
3. `ar_display_acquire`/`release` over 051's ring (`AR_QUERY`, never blocking).
4. `ar_session_pass_count`/`_info`/`read_pass`: Combined as the display mean, sample count, bounces, invalid, rel error, AOVs (049); quantities MEAN, VARIANCE, COUNT; rows bottom-up. `ar_session_read_counters` (045's per-pass counters). `ar_session_write_fits` (DUMP through 050, restricted to the requested channels).
5. Tests: test_readback.cpp (fast, synthetic film): read_pass MEAN and VARIANCE equal 047's helpers; a scene GENERATION_REJECTED entry appears in the session's polled report; test_session_api.py (CPU device): progress state sequence INITIALIZING -> RENDERING -> CLOSED, report polling, display frames with non-decreasing generation, read_pass of an empty render, write_fits output parsed by fits_read.py.
Acceptance: `pwsh tools/check.ps1 gate` passes with `readback/*` cases; `<python> -m unittest discover -s tests/abi -p "test_session_api.py"` passes and prints the state sequence.
Look at: none.
Canary: tasks/104.canary.patch must turn red: astroray_tests_cpu "readback/scene entries merged", tests/abi/test_session_api.py "TestSessionApi.test_state_sequence".
Budget: host only.
Depends on: 052, 050, 049.
Size: M (about 300 lines); stop and report at 2x.
Progress:
- Scope widened by the lead to session.{h,cpp}: read_film (render lock), display ring forwarders, counters copy, last_pass_ms, merged report, layout built in the ctor.
- ABI choices: frame_id 0 = no newer frame; read_pass n must equal the float count exactly; read_counters n >= word count (error text names it); report and progress stay readable after CLOSED, film reads do not.
- CPU sessions report device free/total as 0 (no device memory); CUDA fills them (102). FITS provenance strings for working space and wavelength pdf are descriptive ("observer.bin", "default") until a selection exists.

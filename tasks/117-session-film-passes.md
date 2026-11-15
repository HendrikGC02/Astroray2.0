# 117 Session film passes follow the latched settings
Goal: a session's film never allocates the primary-hit AOVs (`FilmRequest::primary_aov` is never set), so Position, Normal, UV and IndexMA never reach readback -> the session derives its film request from the latched `ar_settings.passes`, and the F12 render, the camera NDC test (082) and the exporter equivalence (084) can read those passes.
Read first: 0008 d10 (pass list; a pass exists only when the feature that produces it exists), d13a (readback enumerates what the layout allocated), 0007 d25 (latch and epochs), RULINGS 69 and 70 (latch, pinned table sets), src/host/session/session.cpp (`Session::Session`, the film creation on the render thread, `begin_epoch`, the latch in the loop), src/host/film/film_layout.h, src/host/session/readback.cpp (Blender names of the AOV channels).
Scope: src/host/session/session.cpp, session.h, include/astroray/astroray.h and the generated bindings (`PASS_UNSUPPORTED`, ruling 74), src/capi/session_api.cpp (read the pass list inside the film read; ruling 75), src/host/report_codes.def, tests/abi/test_session_passes.py, src/host/film/film_layout.h/.cpp (only a helper mapping pass names to a `FilmRequest`, if needed), tests/cpu/test_session_passes.cpp, CMakeLists.txt or tests/cpu/CMakeLists.txt (register the test).
Out of scope: the light-path, Cryptomatte, denoising and band passes (their producers do not exist); the Blender side of the request (084).
Reference: none beyond the records; the AOV meanings are 0008 d10 and readback.cpp.
Do:
1. The film request comes from the latched settings: `primary_aov` is set when `passes` names any of Position, Normal, UV or IndexMA (readback.cpp's names, one mapping, not a second copy). Combined and the statistics passes stay as today.
2. The film is built from the first latched settings, before the first pass. A later commit whose requested set changes the request rebuilds the film at that epoch's start (begin_epoch clears it anyway), re-uploads what holds the film's address (`upload_session_tables` takes `film->data()`), and publishes it under the render lock as today, so readback never sees half a film.
3. A requested pass the engine cannot produce yet (any name outside the produced set) raises one visible report entry naming it per latched settings change, using an existing report code that fits; if none fits, stop and ask the lead. It never fails the render.
4. tests/cpu/test_session_passes.cpp (CPU session, gate): no passes requested -> `ar_session_pass_count` lists no AOV; Position requested -> Position, Normal, UV and IndexMA are listed and Position reads finite values on a hit pixel; a commit that drops the request rebuilds the film without them; an unknown name gives the report entry once.
Acceptance: `pwsh tools/check.ps1 fast` and `gate` pass.
Look at: none.
Canary: tasks/117.canary.patch must turn red: tests/cpu/test_session_passes.cpp (the patch leaves `primary_aov` false).
Budget: none.
Depends on: 071.
Size: S (about 150 lines); stop and report at 2x.
Progress:
- Session::sync_film (session.cpp) builds the film from scene_.settings() in allocate() and at every epoch start; rebuilds only when the channel list changes, re-runs upload_session_tables, publishes layout_ under mutex_.
- Pass names are matched against list_passes of a film with every producible channel, so readback.cpp's table stays the one mapping (readback.cpp untouched).
- Unknown names: AR_REPORT_PASS_UNSUPPORTED 44 (ruling 74; astroray.h, report_codes.def), subject 0, detail = the name, once per settings object; the render continues.
- Ruling 75: Session::pass_revision_ counts channel-list changes; ar_session_s::pass_revision is set by pass_count; pass_info and read_pass (inside read_film) refuse another. The ABI stale-index test goes red when read_pass's check is dropped (checked by hand); the canary's primary_aov hunk turns it red too. StoredSettings carries no generation, so PASS_UNSUPPORTED keeps latch_.generation().
- ar_session_read_pass takes the pass list from the film's layout inside read_film (pass_info reads one layout copy and needs no film). tests/abi/test_session_passes.py goes through the C API; the canary turns it red too.
- Session::layout() now returns a copy under mutex_ (the film can be rebuilt on the render thread); session_api.cpp compiles unchanged.
- Test reads list_passes(Session::layout()) as ar_session_pass_count does (the CPU test exe does not link session_api.cpp). Canary red: 5 checks in case 1, 1 in case 2.

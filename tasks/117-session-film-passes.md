# 117 Session film passes follow the latched settings
Goal: a session's film never allocates the primary-hit AOVs (`FilmRequest::primary_aov` is never set), so Position, Normal, UV and IndexMA never reach readback -> the session derives its film request from the latched `ar_settings.passes`, and the F12 render, the camera NDC test (082) and the exporter equivalence (084) can read those passes.
Read first: 0008 d10 (pass list; a pass exists only when the feature that produces it exists), d13a (readback enumerates what the layout allocated), 0007 d25 (latch and epochs), RULINGS 69 and 70 (latch, pinned table sets), src/host/session/session.cpp (`Session::Session`, the film creation on the render thread, `begin_epoch`, the latch in the loop), src/host/film/film_layout.h, src/host/session/readback.cpp (Blender names of the AOV channels).
Scope: src/host/session/session.cpp, session.h, include/astroray/astroray.h and the generated bindings (`PASS_UNSUPPORTED`, ruling 74), src/capi/session_api.cpp (read the pass list inside the film read), tests/abi/test_session_passes.py, src/host/film/film_layout.h/.cpp (only a helper mapping pass names to a `FilmRequest`, if needed), tests/cpu/test_session_passes.cpp, CMakeLists.txt or tests/cpu/CMakeLists.txt (register the test).
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

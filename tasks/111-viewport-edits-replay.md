# 111 Viewport edit matrix (0007 V4 GUI part) and viewport record/replay (0001 V11)
Goal: viewport sync is checked by one edit only and no viewport session has been replayed -> every 0007 V4 edit class is scripted in a GUI Blender with its expected action class, call counts and a changed frame, and a recorded viewport sync replays bit-identically through the CLI.
Read first: 0007 V4 (GUI part), d20 (classification), d25 (generations); 0001 V11; blender/astroray/viewport.py; tests/blender/gui_present.py (085).
Scope: tests/blender/gui_edits.py, tests/blender/gui_record_replay.py, blender/astroray/viewport.py (frame log fields only). Lead ruling: blender/astroray/render_final.py (scan() keeps an evaluated original, not the freed instance copy) and blender/astroray/properties.py (update callbacks tag the scene, so addon settings reach view_update) join Scope.
Out of scope: fixing sync defects found here beyond one-line fixes (report them; the lead writes tasks); batching (086).
Reference: none.
Do:
1. gui_edits.py (GUI Blender run): scripted edits per 0007 V4 (move, instancer move, material, vertex, add and delete object, collection instance, collection exclude toggle, armature playback, selection, node group, image, world, light, rename, an unlisted type); for each, the expected action class and call counts, no full resync except the unlisted type, and the frame after the edit differs from the pre-edit frame (captured with the frame log, not by eye).
2. gui_record_replay.py: record a viewport sync of the smoke scene with progress and two edits (ASTRORAY_RECORD), replay with `astroray_cli render --device cpu --seed 7`; bit-identical to the same calls driven directly through the ABI in-process; the recording holds no AR_QUERY call (0001 V11).
3. From 085: the viewport's delta sync never sends ar_edit_remove for the geometry of deleted objects (the geometry set only grows); the add-and-delete edit of Do 1 must remove it (blender/astroray/viewport.py joins Scope).
4. From 086: classify.settings_signature ignores scene.astroray, so editing viewport_samples (or any addon setting) never reaches a live viewport session; include an addon-setting edit in Do 1's matrix and fix the signature (blender/astroray/sync/classify.py joins Scope).
Acceptance: both GUI scripts pass through `Invoke-ArBlender -Gui` and print one line per edit class.
Look at: none.
Canary: tasks/111.canary.patch must turn red: tests/blender/gui_edits.py "TestEdits.test_move_patches_transforms_only", "TestEdits.test_geometry_set_follows_the_scene", "TestEdits.test_data_edits_use_their_own_setter", "TestEdits.test_inert_edits_send_nothing", "TestEdits.test_only_the_unlisted_type_resyncs_fully", "TestEdits.test_every_edit_changes_the_frame", "TestEdits.test_instanced_geometry_is_the_instanced_mesh", tests/blender/gui_record_replay.py "TestReplay.test_bit_identical", "TestReplay.test_recording_shape" (lead ruling: all nine cases).
Budget: none on tiers (`check full` Blender suite).
Depends on: 085, 110, 107.
Size: M (about 300 lines Python); stop and report at 2x.
Progress:
- started (implementer)
- gui_edits (19 classes, 6 cases) and gui_record_replay (2 cases) pass; canary 8 hunks each red alone and together. Outside Scope, flagged for lead: render_final.scan (dead temp object for dupli geometry), properties.py update callbacks (scene.astroray raised no depsgraph update).
- Over-sends reported to the lead (exact counts kept in EDITS, each row commented): material/node-group/image edits re-read the user's mesh (Blender's extra tag); add/delete/exclude/include/collection-instance/settings edits re-send the lamp (ours: _object_kinds returns LIGHTS for any LIGHT update); settings edits re-send the whole instance table (cause not isolated); world/light node trees classed MATERIALS (ours); first move after a full sync re-sends the instance table (ours: _after_full leaves table None).

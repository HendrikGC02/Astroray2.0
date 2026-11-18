# 111 Viewport edit matrix (0007 V4 GUI part) and viewport record/replay (0001 V11)
Goal: viewport sync is checked by one edit only and no viewport session has been replayed -> every 0007 V4 edit class is scripted in a GUI Blender with its expected action class, call counts and a changed frame, and a recorded viewport sync replays bit-identically through the CLI.
Read first: 0007 V4 (GUI part), d20 (classification), d25 (generations); 0001 V11; blender/astroray/viewport.py; tests/blender/gui_present.py (085).
Scope: tests/blender/gui_edits.py, tests/blender/gui_record_replay.py, blender/astroray/viewport.py (frame log fields only).
Out of scope: fixing sync defects found here beyond one-line fixes (report them; the lead writes tasks); batching (086).
Reference: none.
Do:
1. gui_edits.py (GUI Blender run): scripted edits per 0007 V4 (move, instancer move, material, vertex, add and delete object, collection instance, collection exclude toggle, armature playback, selection, node group, image, world, light, rename, an unlisted type); for each, the expected action class and call counts, no full resync except the unlisted type, and the frame after the edit differs from the pre-edit frame (captured with the frame log, not by eye).
2. gui_record_replay.py: record a viewport sync of the smoke scene with progress and two edits (ASTRORAY_RECORD), replay with `astroray_cli render --device cpu --seed 7`; bit-identical to the same calls driven directly through the ABI in-process; the recording holds no AR_QUERY call (0001 V11).
3. From 085: the viewport's delta sync never sends ar_edit_remove for the geometry of deleted objects (the geometry set only grows); the add-and-delete edit of Do 1 must remove it (blender/astroray/viewport.py joins Scope).
Acceptance: both GUI scripts pass through `Invoke-ArBlender -Gui` and print one line per edit class.
Look at: none.
Canary: tasks/111.canary.patch must turn red: tests/blender/gui_record_replay.py "TestReplay.test_bit_identical", tests/blender/gui_edits.py "TestEdits.test_move_patches_transforms_only".
Budget: none on tiers (`check full` Blender suite).
Depends on: 085, 110, 107.
Size: M (about 300 lines Python); stop and report at 2x.
Progress:

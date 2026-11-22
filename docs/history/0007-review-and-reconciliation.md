# 0007: review and reconciliation

Moved unchanged from the end of `docs/decisions/0007-blender-integration.md` (ruling 86). History only: nothing here is binding (ruling 50), and the record's Decision section is current.

## Review and resolution

History of the independent review; the Decision section and the Reconciliation section below supersede it where they differ (for example item 5's 1.2× threshold, now 1.5×).

Independent review, 2026-11-14: 1 blocker, 6 major, 6 minor.

1. BLOCKER, continuous orbit starves the display: ACCEPTED. Decision 25 now coalesces view changes and commits at pass boundaries and never aborts a generation's first pass; decision 34 also reads pass time and the age of the newest frame in every `view_draw`, with an immediate step-down at 83 ms; V11/V12 gain a continuous-orbit trace.
2. MAJOR, atomic commit vs pipelined submit: ACCEPTED. Commit does O(1) checks and returns the pending generation; O(n) validation runs on workers before upload; a failed generation is dropped with the previous scene kept and reported via progress and messages; decode and memory failures have defined routes (decision 9); `ar_edit_reset` added.
3. MAJOR, topology cache can show wrong geometry: ACCEPTED, with a narrower rule than proposed. The settle timer is gone; positions-only is used only for deform-only stacks with no original-mesh update, or allow-listed modal operators (decision 22); everything else is a full read; V6 extended.
4. MAJOR, catch-all full resync: ACCEPTED. Decision 20 is now an explicit per-ID-type table modelled on Cycles' `sync_recalc`, full resync only for unlisted types, each row a V4 case.
5. MAJOR, TTFF compares unlike quantities: ACCEPTED. Gate moved to an end-to-end screen-capture measure for both engines (V7a), with log-bounded phases named line by line (V7b); table recomputed (instance loop counted once, 637/514 serial worker sum, session creation outside Cycles' window); the threshold went from 1.5× to a PROVISIONAL 1.2× pending the owner.
6. MAJOR, volume transport unworkable: ACCEPTED. Decision 47 uses Blender's bundled `openvdb` Python module (present in the 5.3 install) and brick-wise `copyToArray`, so the engine needs no OpenVDB and host memory is bounded; P6 and V21 added; the axis-order conflict with 0006 is listed.
7. MAJOR, spill before trim: ACCEPTED. Decision 14 orders trim, spill, fail: F12 starts with spill disallowed while paused viewports hold scenes, trims them on out-of-memory and re-syncs once.
8. MINOR, controller counts its own scale changes as interaction: ACCEPTED. Decision 49 gives one definition (host commits and view-hash changes only), used by decisions 30 and 34.
9. MINOR, depth deferred though volumes and displacement ship in R: REJECTED in substance. True displacement is after the first release (0005), and volumes have no single depth (Cycles also leaves depth to Blender). Decision 31's wording now says so, and true displacement is in the L column.
10. MINOR, omitted scope: ACCEPTED. OpenGL backend (decision 29, V8), F12 view layers and pass registration (decision 48), the view hash from `view_draw` (decision 49).
11. MINOR, create/destroy block the main thread: ACCEPTED. Create returns at once and builds the context on the render thread, overlapping the first sync; close is non-blocking and destroy is called once CLOSED (decision 23); V22 added.
12. MINOR, three agent guesses: ACCEPTED. Keys come from a counter and a dict keyed on identity tuples, so they cannot collide (decision 18); the move map is keyed on `session_uid` (decision 19); the custom-node uuid side channel is removed and custom nodes wait for P1, matching 0005 (decision 39).
13. MINOR, POST_PIXEL handler may be unnecessary: ACCEPTED. P4 tries drawing in `view_draw` without the display-space shader first and keeps the handler only as the fallback (decision 40).

## Reconciliation (rulings applied)

- G1: "What lands when" replaced by the two lists; milestone labels removed from decisions 10, 11, 12, 13, 15, 28, 31, 35, 36, 39 and from Risks; decision 36's M/R/L table became a coverage table.
- G2: restated content replaced by citations: report (0001), errors and faults (0001 D2.5), out-of-process gate (0001 D6), packaging (0001 D10), pass readback and names (0008), grid leaf format (0006 D5), mips (0005 item 34), mask allocation and motion pipeline (0002), time budgets (0009).
- G3: no scheduled job existed in this record; none added.
- G4: kept `motion_steps`, visibility bits, link sets and `ar_edit_set_astro` because removing them would change the C ABI and path state later; "hook, not in UI until it lands" wording removed.
- Ruling 1: decision 25 latches after 0008's per-sample-buffer resolve and discards an interrupted pass's buffer; decision 12 refers resets to 0004 D25.
- Ruling 10: header `astroray.h`, `ar_context`/`ar_scene`/`ar_session` handles, every function returns `ar_status`, `ar_abi_hash` with exact match, no `struct_size` minor versioning, `_begin`/`commit` staging names, no `AR_NO_FRAME` status.
- Ruling 11: `ar_grid_desc` and decision 47 carry sparse 8³ leaves (64³ extraction chunks, as 0006 D5); V21 updated; the axis-order conflict is gone.
- Ruling 12: packaging ships our renamed `astroray_oidn*` build; the OIDN/TBB risk is closed by 0001 D9.
- Ruling 13: no loose kernel modules in the zip; kernels and tables embedded; V16 checks it.
- Ruling 14: launch sizing in decision 25 cites the 20 ms target and 100 ms cap (numbers in 0009).
- Ruling 19: decision 14 holds the only reserve rule (R = 2 GiB, P5) and the VRAM budget table; 0002's and 0008's reserves defer to it.
- Ruling 20: decoders stb_image and tinyexr used only as 0001 D9.1 list entries.
- Ruling 21: TTFF gate ≤ 1.5× Cycles, goal 1.0×, PROVISIONAL, stated once in decision 35.
- Ruling 22: decision 13 builds mip chains (0005 item 34); no demand loading.
- Ruling 29: time numbers replaced by 0009 ids (decisions 5, 23, 25, 30, 34; V3 B-31, V11 B-14/B-33, V12 B-32, V22 B-21).
- Ruling 30: decision 50 states the progressive batching rule; decision 34 steps on B-14's floor and target; decision 30's publish rates moved to B-34; V23 tests batching.
- Ruling 33: `ar_edit` handle and `ar_key` cited (decision 4); device selected on `ar_context` and removed from `ar_scene_desc`; decision 5 owns the readback list (`ar_session_pass_count`, `pass_info`, `read_pass`, `read_counters`, `write_fits`); every descriptor's field list moved to `0007-appendix-abi.md` (decision 7).
- Ruling 37: decision 25 defines generation and epoch; decision 12 uses them.
- Ruling 42: decision 14's budget table adds the side buffers (shadow hit records at 84 B, shadow-pass split, geodesic and random-walk records).
- Ruling 43: `ar_grid_desc` carries the object-to-world matrix (appendix).
- Ruling 45: light and shadow linking removed from `ar_instances_desc`, `ar_light_desc`, decision 36, must-exist, can-wait and Consequences; listed as not designed.
- Ruling 49: Review section headed as history; the TTFF owner question replaced by owner decision 23 (decision 35, Risks).
- Ruling 29 (second pass): decisions 23, 26, 27, 28, 34 and 49 cite 0009 B-18 and B-37 instead of times (decision 35's TTFF estimate table kept as the gate's evidence, raised with the lead).
- Ruling 33 (second pass): decision 5 cites 0001's device and context functions; the appendix defines `ar_spectrum_ref`, carries Cryptomatte ids and the material pass index; Context no longer attributes `ar_spectrum_ref` to 0003.
- Ruling 37 (second pass): decision 25 interrupts a pass only for an epoch-starting change, whose reset clears the interrupted pass's buffers (0008 decision 3).
- Ruling 42 (second pass): decision 14's film row adds 0008's per-sample buffer.
- Ruling 49 (second pass): 0005 cited by item numbers; decision 15 states the previous-frame data 0008's motion buffer reads; decision 40 adds the Invalid and Rays statistics; decision 41 states the static camera frame 0006 D24 needs; the dangling "path_state list" and "0009 Risks g" references fixed.

# Status

Overwritten by the lead only. Live facts (branches, PRs, last gate) come from
`tools/status.ps1` once it exists, never from this page.

## Phase
Build phase. The owner signed off the planning records (O30) and asked for
speed (O31): several implementers at once, review for kernel, physics, ABI and
process diffs.

## Current milestone
M1a "First pixels" (`docs/ROADMAP.md`): textured, lamp-lit meshes render
progressively in the Blender 5.3 viewport and F12 on CUDA and CPU through the
final C ABI, with every gate live from the first task. Open task files and
their order: `tasks/README.md`.

## Open owner questions
During M1a: Nsight counter enable (0002 q1, needed by task 093); deleting
superseded records (0009 q4). Answered: open-weight worker (O32). Later ones:
ROADMAP "Open items".

## Red items
None. Follow-ups noted for later tasks: the ABI exposes no CUDA probe reason or
driver version (017); JH table round trip is 1.1-1.6e-3 off-grid near white and
up to 4.3e-3 per channel on saturated emitters (023); `check fast` runs 9-10 s
after a rebuild (B-08 is 10 s). The material VM copies 056's kSlotSemantic into
vm/store_slot.h (nvcc rejects the shared inline constexpr; a test pins the rows),
which should become one device-visible definition in constant_block.h. 0006's
required interval rules in the opcode generator have no owning task yet. M1b (Mix/Add
Shader): 058's compiler rejects one value feeding two EMIT_LE nodes; insert a
copy or clone shared constants. Tooling: a doctest case outside a tier suite is
skipped without a warning; a new test file once needed a CMakeLists touch to
reconfigure. 032's CUDA init uses its own Ledger, not the context's
(fix with 035/041), and the CUDA context is created on the init thread, so
workers must make it current.

## Budget headroom
Not measured yet: no gate has run. Spike numbers: docs/spikes/*/RESULTS.md.

# 087 API-surface check against bpy 5.3.x
Goal: a Blender update could silently break the addon's fragile reads -> one script lists every `bpy` attribute, operator, module and pass name the addon depends on, checks each against the pinned `bpy` 5.3.x wheel in hosted CI, and fails naming what disappeared.
Read first: 0007 d46 (API-surface check, fragile points: `.corner_vert`, pass names, `Window.modal_operators`, bundled `openvdb`), d17, d22, d47; 0009 d12 (`ci`), d17a (5.4 informational); docs/ROADMAP.md "Blender 5.4".
Scope: blender/astroray/api_surface.txt, tools/api_surface.py, tools/check.ps1 (`ci` step), tests/blender/headless/test_api_surface.py.
Out of scope: the 5.4 leg (added in the first milestone closing after `bpy` 5.4 is on PyPI, informational until 5.4 ships).
Reference: none.
Do:
1. api_surface.txt: one line per dependency, kinds `attr` (dotted RNA path such as `Mesh.attributes['.corner_vert']` checked on a generated mesh, `Window.modal_operators`, `Material.inline_shader_nodes`, `ColorManagedInputColorspaceSettings.interop_id`, `bpy.data.colorspace.working_space_interop_id`), `module` (`openvdb` with `evalActiveVoxelBoundingBox`, `copyToArray`), `pass` (pass names the addon registers), `call` (functions such as `RenderEngine.bind_display_space_shader`, `gpu.types.GPUStorageBuf`). Every attribute the addon uses that 0007 d46 calls fragile must be listed; a comment line per section cites the decision.
2. tools/api_surface.py (runs under the wheel's Python or inside Blender): checks each line and prints `OK`/`MISSING <line>`; exit 1 on any MISSING.
3. check.ps1 `ci` runs it after installing the wheel; locally it runs inside Blender through Invoke-ArBlender.
4. test_api_surface.py: a fixture list with one invented attribute must fail with that line named.
Acceptance: `pwsh tools/check.ps1 ci` (with `$env:CI='true'`) passes and prints the count of checked entries; the fixture test passes.
Look at: none.
Canary: tasks/087.canary.patch must turn red: tests/blender/headless/test_api_surface.py "TestApiSurface.test_missing_attribute_named".
Budget: a few seconds of CI.
Depends on: 080, 009, 086, 110, 111 (the fragile list is final only after the last Blender task).
Size: S (about 150 lines); stop and report at 2x.
Progress:
- started

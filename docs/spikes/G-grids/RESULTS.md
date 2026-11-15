# Spike G: volume grids through Blender 5.3's bundled `openvdb` module as sparse 8^3 leaves

Date 2026-11-15. Throwaway code under `out/spikes/G/` (git-ignored; scripts named below). Numbers are the product.

## 1. Question

Can volume grids reach the engine fast enough through Blender's bundled `openvdb` Python module, as sparse 8^3 leaves (0006 decision 5, 0007 decision 47), that the scene's first frame meets 0007 decision 35's gate as ruling 21 states it (V7a: at most 1.5x Cycles, goal 1.0x, PROVISIONAL)? 0006 Risk 5 / 0007 P6 / V20 ask for this. The engine does not exist: the Python side is measured end to end; ABI transfer, NanoVDB build and upload are an **estimate** (section 5).

## 2. Setup

| item | value |
|---|---|
| Blender | 5.3.0, hash 02f0899d78e3 (2026-11-10), Python 3.13.13, `--background --factory-startup`, `BLENDER_USER_CONFIG/SCRIPTS/EXTENSIONS/DATAFILES` under `out/spikes/G/home/` (`bl.ps1`, `blc.ps1`) |
| `import openvdb` | works. `openvdb.__file__` = `...\Blender 5.3\5.3\python\Lib\site-packages\openvdb.cp313-win_amd64.pyd`; no `__version__`; `openvdb.LIBRARY_VERSION` = `(13, 0, 0)`, `openvdb.FILE_FORMAT_VERSION` = 225 (`probe.py`, `probe2.py`) |
| Machine | RTX 5070 Ti (Cycles OptiX only), Ryzen 7 7800X3D |
| Scenes | `gen.py N out`: a Volume object with a geometry-nodes modifier: Ico Sphere (6 subdivisions) displaced along normals by a 3D Noise (scale 2.2, detail 8), then **Mesh to Volume** (Resolution Mode Size, voxel size 2.4/N, interior band 4 voxels, density 1). Evaluated grid saved with `VolumeGrids.save`. Grid `density`, FloatGrid, background 0, fog volume. Synthetic: not the owner's clouds, not bunny_cloud, not a fluid bake. |

API calls used (each confirmed with `dir()` / `__doc__` first): `openvdb.read(filename, gridname)`, `GridBase.evalActiveVoxelBoundingBox()` (returns inclusive `(lo, hi)`), `FloatGrid.copyToArray(array, ijk=(i,j,k))`, `.background`, `.activeVoxelCount()`, `.leafCount()`, `.memUsage()`, `.getConstAccessor().getValue(ijk)`, `.transform.voxelSize()/indexToWorld()/typeName`, `Vec3SGrid.copyFromArray/copyToArray`, `openvdb.readAll`; Blender: `bpy.types.VolumeGrids.save(filepath)` (exists on the instance `volume.grids`, absent from `dir(bpy.types.VolumeGrids)`; signature `(filepath) -> success`), `Object.evaluated_get(depsgraph).data.grids`.

Stand-in grids (`python stat.py`):

```
smoke256.vdb float active 5181364 bbox ((-129,-119,-125),(126,115,122)) leaves 6232 bg 0.0 mem 15829668
smoke512.vdb float active 41410557 bbox ((-259,-240,-251),(252,230,245)) leaves 26791 bg 0.0 mem 61699776
```
Voxel size 9.375e-3 (UniformScaleMap) for the 256 grid. Bounding boxes are 256x235x248 and 512x471x497 voxels.

## 3. The Python path (`bench.py`; 3 runs per configuration in one Blender process, run 0 is cold)

Method per run: `openvdb.read`; bounding box; the box is rounded out to multiples of 64 so VDB's 8^3 leaves align; for each 64^3 chunk `copyToArray` into one reused 1 MiB float32 buffer; select the 8^3 leaves holding any value `!= background`; pack `i32 origin[n][3]` and `f32 voxels[n][8][8][8][1]` (x, y, z with z fastest, from the C-order chunk). One batch per chunk (kept in a list, as the staged-array batches of 0007 d47 would be).

Correctness: the number of non-background voxels inside the selected leaves equals `activeVoxelCount()` exactly in both grids; 2000 random voxels per grid compared with `getConstAccessor().getValue` : 0 mismatches. Vec3 (`Vec3SGrid`, array shape `[x][y][z][3]`, channel last, z fastest) round-trips exactly (`misc.py`). A non-zero background (0.25) is returned for inactive voxels by `copyToArray`.

### 3.1 First version: mask by `(b != bg).any(axis=(3,4,5))` on the transposed view (`out/spikes/G/bench512.txt`)

| step | 256^3 | 512^3 |
|---|---|---|
| `VolumeGrids.save` of the generated grid (`gen.py`) | 49 ms | 213 ms (file 5.5 MB / 22.9 MB) |
| `openvdb.read` | 15-18 ms | 63-75 ms |
| `copyToArray`, all chunks | 5-6 ms (80 chunks) | 39-40 ms (576 chunks) |
| leaf selection | 58-59 ms | 435-441 ms |
| origin + voxel packing | 7-10 ms | 54-70 ms |
| **total, read to packed** | **105-245 ms** (245 cold, 105-108 warm) | **693-707 ms** |

Selection dominates. Dead end tried: `any(axis=(1,3,5))` on the untransposed bool array was no faster (0.43 s).

### 3.2 Tuned selection (`selbench.py`, then `bench.py --fast`)

Compare as a bool array, reinterpret it as `uint64` (eight z-voxels per word), reduce y and x with `max`, test `!= 0`. Same mask as the first version (checked equal on every chunk of the 512 grid). Selection time on the 512 grid: any(1,3,5) 0.434 s, staged any 0.297 s, uint64 trick **0.058 s**, min/max per leaf 0.861 s.

| step | 256^3 | 512^3 |
|---|---|---|
| `openvdb.read` | 15-19 ms | 62-77 ms |
| `copyToArray`, all chunks | 4.6-4.8 ms | 34-35 ms |
| leaf selection | 12.6-13.9 ms | 93 ms |
| packing | 6.9-10.4 ms | 50-73 ms |
| **total, read to packed** | **59-80 ms** (80 cold) | **314-363 ms** (363 cold) |

Pasted tail (`out/spikes/G/bench512_fast.txt`, run 0):
```
"read_s": 0.0769, "active": 41410557, "copy_s": 0.0339, "select_s": 0.0933, "pack_s": 0.0733, "chunks": 576,
"leaves": 91048, "nonbg_voxels_in_leaves": 41410557, "spot_mismatch": 0, "total_s": 0.3630, "fast": true,
"mem_MiB_before_after_peak": [193.2, 388.7, 626.8]
```

## 4. Sizes and memory

| | 256^3 | 512^3 |
|---|---|---|
| active voxels | 5,181,364 | 41,410,557 |
| bounding box voxels / dense float bytes | 15.0 M / 59.7 MB (56.9 MiB) | 119.7 M / 479 MB (457 MiB) |
| VDB leaves / **selected 8^3 leaves** | 6,232 / **12,605** | 26,791 / **91,048** |
| leaf payload (4,096 B voxels + 12 B origin per leaf) | 25,966,300 B (24.8 MiB) | 187,558,880 B (178.9 MiB) |
| `GridBase.memUsage()` | 15.8 MB | 61.7 MB |
| peak working set of the Blender process (baseline about 193 MiB) | 262-267 MiB | 627-648 MiB |

Peak memory is an upper bound for the engine path: the harness keeps all batches (188 MB) and concatenates them once more for the spot check (another 188 MB). With batches handed to the engine as they are made, host memory is the VDB grid (62 MB) plus one 1 MiB chunk plus the staging copy.

Finding for 0006 d5: the selected leaf count is 2.0x (256) and 3.4x (512) the VDB leaf count, because Mesh to Volume leaves constant interior tiles (active voxels per VDB leaf come to 831 and 1,546, above 512) and `copyToArray` expands tiles to voxels. The ABI as written has no tile, so such tiles cost memory and transfer as leaves. Not measured on real smoke or cloud data, where tiles may be rarer.

## 5. Estimate for ABI transfer + NanoVDB build + upload (ESTIMATE, nothing here was run)

Inputs: 25.97 MB (256) and 187.6 MB (512) of leaves; the NanoVDB grid is about the same size plus 1-2% for upper nodes (leaf stats and masks about offset the origin field).

| stage | basis | 256^3 | 512^3 |
|---|---|---|---|
| ABI copy into staged arrays | memcpy 8-12 GB/s, one thread | 2-3 ms | 16-23 ms |
| NanoVDB build from leaves | about three passes over the leaf data (read, stats, write) at 3-5 GB/s effective, single thread; pessimistic bound 0.5 GB/s | 8-30 ms (pessimistic 160 ms) | 110-190 ms (pessimistic 1.1 s) |
| host-to-device upload | pageable about 10 GB/s, pinned about 25 GB/s | 3 ms | 9-25 ms |
| **estimated sum** | | **13-36 ms** | **135-240 ms** |

The NanoVDB build is the term with real uncertainty and no measurement. It must be measured in the engine (V20). The "pessimistic" row alone would break the gate at 512.

## 6. Cycles first frame on the same grids (`cycles_bench.py`, `blc.ps1`)

Method as spike B section 3: Cycles OptiX on the 5070 Ti only, 1920x1080, 1 sample, denoise and adaptive off, persistent data off, `--debug-cycles --verbose 3`; Volume object file-backed (`volume.filepath`), Principled Volume density 20, camera outside. First `bpy.ops.render.render(write_still=False)` of a fresh process is "cold"; two processes per scene; a no-volume scene as the control. The log's `Total render time - Render time (without synchronization)` does **not** contain the Blender-side grid load (sync shows 1 ms and is the same for 256 and 512), so the render-op wall time is used as the first-frame figure. The render itself was looked at (`cy_0.png`, 256 run: the cloud renders).

```
none          run0 wall_ms=276,241,231,220  pre_first_sample_ms(log)=214,179,180,168
none          run1 wall_ms=281,243,239,220  pre_first_sample_ms(log)=216,179,186,168
smoke256.vdb  run0 wall_ms=406,352,335,333  pre_first_sample_ms(log)=265,228,213,212
smoke256.vdb  run1 wall_ms=456,345,350,340  pre_first_sample_ms(log)=311,220,225,218
smoke512.vdb  run0 wall_ms=510,383,380,370  pre_first_sample_ms(log)=312,263,255,248
smoke512.vdb  run1 wall_ms=517,431,365,373  pre_first_sample_ms(log)=305,299,245,246
```
Cycles first frame (cold render-op wall, includes its own file read and NanoVDB conversion, 1 sample): **406-456 ms (256), 510-517 ms (512)**. Part is the fixed scene cost (control 276-281 ms); the volume adds 130-175 ms (256) and 230-240 ms (512).

## 7. Verdict against ruling 21 / 0007 d35

V7a (screen-capture first frame in the GUI, second switch too) was not run: no engine. Proxy: ours = Python path (tuned, cold) + estimate, against the Cycles cold render-op wall above. Both exclude the engine's own scene sync, shader build and first sample, which the gate includes. Gate 1.5x, goal 1.0x.

| case | ours | Cycles | ratio | gate |
|---|---|---|---|---|
| 256^3 file-backed | 80 + (13-36) = 93-116 ms | 406-456 ms | 0.2-0.3x | pass, below the goal |
| 256^3 generated (add `save`, 49 ms) | 142-165 ms | 406-456 ms | 0.3-0.4x | pass |
| 512^3 file-backed | 363 + (135-240) = 0.50-0.60 s | 0.51-0.52 s | 1.0-1.2x | pass, at the goal |
| 512^3 generated (add `save`, 213 ms) | 0.71-0.82 s | 0.51-0.52 s | 1.4-1.6x | at the gate; not a clear pass |
| 512^3 with the pessimistic NanoVDB build | 1.4+ s | 0.51 s | 2.7x | fail |

Cycles on a generated (not file-backed) grid was not measured, so the generated rows compare ours-with-save against the file-backed Cycles figure.

**Verdict: pass-with-estimate.** The measured Python side is cheap: 59-80 ms at 256^3 and 314-363 ms at 512^3 once selection is vectorised the way section 3.2 shows (the first naive version took 0.7 s at 512^3). 256^3 passes with a wide margin. 512^3 passes for file-backed grids on the estimate, sits at the gate for generated grids, and the whole 512^3 result depends on the NanoVDB build running at 3 GB/s or better.

## 8. What 0006 Risk 5 says follows

Risk 5: pass means the first frame meets 0007's gate; on failure the remaining remedies need a dependency decision (native `.vdb` reading through OpenVDB, 0001 D9). Here the route is not refuted: the D5 route can be built, with these conditions recorded:
1. The NanoVDB build time on 91 k leaves is the open number; V20 must measure it in the engine. If it is single-threaded and slow (below 1 GB/s), 512^3 breaks the gate and Risk 5's failure path (native OpenVDB reading, a dependency decision) applies, though that route would still need the same build.
2. Generated grids at 512^3 pay `VolumeGrids.save` (213 ms) on top; that is the first thing to cut if the gate is tight. The `openvdb` module offers no way to read the evaluated grid without the file.
3. Vectorised selection (uint64 reinterpretation) is required; the straightforward NumPy version costs 0.44 s at 512^3.
4. The ABI has no constant tile; synthetic Mesh to Volume interiors expand 2-3.4x into leaves. Check on real clouds before fixing the ABI.
5. The module has no version attribute and is not a stable Blender API; the 5.4 CI leg must `import openvdb` and run a `copyToArray` smoke test. Python 3.13.13 / NumPy 2.3.4 here.

## 9. Not measured, limits

- No ABI transfer, NanoVDB build, upload, engine sync, or V7a/V7b in the GUI (section 5 is an estimate).
- Synthetic Mesh to Volume shells only; no owner's clouds, no bunny_cloud, no fluid bake, no time-varying grids. Active fraction of the box is 8.6% (512^3); a denser or sparser grid moves selection and packing times.
- Vec3 and time-varying frames: only a synthetic 16^3 Vec3 round trip.
- Background Blender only; GUI viewport grid load not timed (the Cycles log gave no grid-load line, so the Cycles figure is the render-op wall).
- Two processes per Cycles configuration; run-to-run spread about 50 ms.

## 10. Files

`out/spikes/G/`: `gen.py`, `bench.py`, `selbench.py`, `cycles_bench.py`, `stat.py`, `misc.py`, `probe*.py`, `bl.ps1`, `blc.ps1`, `bench512.txt`, `bench256_fast.txt`, `bench512_fast.txt`, `cycles256.txt`, `cycles512.txt`, `smoke256.vdb`, `smoke512.vdb`. Git-ignored.

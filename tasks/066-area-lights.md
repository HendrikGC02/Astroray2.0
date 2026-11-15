# 066 Rectangle and disk area lamps
Goal: area lamps do not exist -> one-sided rectangle (and square) lamps with spherical-rectangle sampling and disk lamps with area sampling implement the light interface in Blender's energy convention, with unsupported area-lamp settings reported.
Read first: 0004 D6 (area row: L = P/(pi A) s, one-sided, spread), D7 (interface), D8 (lamps as primitives), 0003 d11 (unit anchor); 0001 D2.11 (`NODE_APPROXIMATED` for light settings).
Scope: src/kernel/light/area_rect.h, src/kernel/light/area_disk.h, src/kernel/light/light.h (dispatch rows), tests/cpu/test_lights_area.cpp (+ f64).
Out of scope: spread other than 180 degrees, ellipses with unequal axes and collimated lamps (M4: the host reports them, 068); lamp primitives in acceleration structures (068).
Reference: Urena, Fajardo and King 2013, "An Area-Preserving Parametrization for Spherical Rectangles", CGF 32(4); Cycles `kernel/light/area.h` (Apache-2.0) for the rectangle; pbrt-v4 `src/pbrt/shapes.h` `Disk::Sample` (Apache-2.0) for the disk.
Do:
1. area_rect.h: L = P/(pi A) s(lambda), one-sided (front = the lamp's -Z axis in Blender's convention; verify against Blender's area lamp orientation and state it), spherical-rectangle sampling for NEE with its solid-angle pdf, `pdf_li` for BSDF hits, `sample_le` cosine-weighted from a uniform area point, `power`, `bounds` with theta_e = pi/2.
2. area_disk.h: same radiance law with A = pi r^2, uniform area sampling converted to solid angle, `pdf_li`, `sample_le`, `power`, `bounds`.
3. Tests (fast unless noted): `pdf_li(sample.wi) == sample.pdf` within 1e-4 relative for points in front at several distances and angles; zero contribution from behind; irradiance at a point on the axis under a disk and under a square matches closed forms (disk: E = pi L r^2/(r^2 + d^2); square: the form-factor formula) within 0.5%; power recovered from L by quadrature within 1e-4; gate: chi-squared of rectangle sample directions against the spherical-rectangle pdf.
Acceptance: `pwsh tools/check.ps1 fast` passes with `lights_area/*`; `out/bin/astroray_tests_cpu.exe --tier=gate -tc="lights_area/*"` passes.
Look at: none.
Canary: tasks/066.canary.patch must turn red: astroray_tests_cpu "lights_area/disk on-axis irradiance", "lights_area/rect pdf consistency", "lights_area/disk power".
Budget: none.
Depends on: 065.
Size: M (about 350 lines); stop and report at 2x.
Progress:
- Area lamps in src/kernel/light/area_rect.h and area_disk.h, rows in light.h (LightType AREA_RECT, AREA_DISK; light_make_area; area checks in light_problem).
- Record convention: xf columns 0 and 1 span the rectangle, front = -Z column; rect size[0] x size[1] are full width and height; a disk's size[0] is the RADIUS (size[1] = 0), so 068 halves Blender's disk diameter.
- Rectangle: spherical sampling inside pbrt-v4's [3e-4, 6.22] sr (shapes.h Min/MaxSphericalSampleArea), area sampling outside; both ends tested.

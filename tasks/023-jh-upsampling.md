# 023 Jakob-Hanika upsampling: Rec.709 table and kernel lookup
Goal: RGB inputs cannot become spectra -> specgen generates the Rec.709 Jakob-Hanika coefficient table from our own observer, and kernel code turns RGB into reflectance, unbounded and illuminant spectra at four wavelengths with pbrt-v4's conventions.
Read first: 0003 d12 (RGB inputs), d16 (method, table layout, conventions), d17 (where and how often), d18 (generation, committed); 0005 item 10 (slot semantics) and item 11 (RGB_LIFT); 0003 Verification checks 3, 5, 6.
Scope: tools/specgen/specgen.cpp (subcommand `jh`), tools/specgen/rgb2spec_opt_port.cpp, data/spectral/rec709.jh, data/MANIFEST.toml, third_party/deps.toml `[[borrowed]]` + THIRD_PARTY.md, src/kernel/color/rgb2spec.h, src/kernel/color/observer.h, tests/cpu/test_rgb2spec.cpp.
Out of scope: Rec.2020 and AP1 tables (M4); the VM's RGB_LIFT opcode (057); film conversion (046).
Reference: Jakob and Hanika 2019, CGF 38(2); mitsuba-renderer/rgb2spec `rgb2spec_opt.cpp` (BSD-3, adapted with notice kept, changed file marked); pbrt-v4 `src/pbrt/util/color.h` `RGBSigmoidPolynomial`, `RGBToSpectrumTable`, and `src/pbrt/util/spectrum.h` `RGBAlbedoSpectrum`, `RGBUnboundedSpectrum`, `RGBIlluminantSpectrum` (Apache-2.0).
Do:
1. Port the optimiser into rgb2spec_opt_port.cpp, replacing its built-in CIE data with data/spectral/cie1931_2deg.csv and its illuminant with our D65 (Y = 1), so the table shares observer_hash. Resolution 64 (PROVISIONAL), scale axis smoothstep(smoothstep(k/63)), layout float[3][64][64][64][3].
2. `specgen jh rec709` writes data/spectral/rec709.jh with header {res, observer_hash, illuminant id, generator version}; MANIFEST entry; the file is committed and embedded through task 022's mechanism.
3. color/rgb2spec.h (kernel): `fetch_coeffs(table, rgb)` (pbrt `FindInterval` bisection on the scale axis, trilinear interpolation), `sigmoid_eval(c, lambda)`; conventions: `albedo(rgb)` clamps to [0,1] first; `unbounded(rgb)` with m = max(r,g,b), s = 2m * S(rgb/2m); `illuminant(rgb)` = unbounded times the working-space illuminant at lambda; zero RGB gives exactly 0; negative components clamp to 0 and return a flag the caller reports. color/observer.h: CMF and illuminant lookup at a wavelength (linear between 1 nm samples, 0 outside 360-830).
4. Tests (fast unless noted): check 3 (7^3 grid on [0.05, 0.95], reflectance round trip through D65 and CMFs back to RGB, max abs error <= 1e-3); check 5 (white (1,1,1) gives 1 +- 1e-4 at all 471 CMF wavelengths); check 6 (an RGB emitter through the illuminant convention, `film_weights` and XYZ by deterministic quadrature over lambda returns its RGB within 1e-3 relative); float and double objects agree within 1e-6.
Acceptance: `out/bin/specgen.exe jh rec709` reproduces rec709.jh byte for byte and prints its run time; `pwsh tools/check.ps1 fast` passes with the three checks named in the output.
Look at: none.
Canary: tasks/023.canary.patch must turn red: astroray_tests_cpu "rgb2spec/round trip rec709", "rgb2spec/white is one".
Budget: specgen total run time against 0009 B-37's specgen row (report it).
Depends on: 019, 022.
Size: M (about 450 lines including the port); stop and report at 2x.
Progress:
- specgen jh rec709: 2.9 s, 9437504 bytes, sha256 d367857a; two runs byte-identical; round trip max abs error 7.7e-4 (NumPy and C++ agree).
- Optimiser: rgb2spec_opt.cpp at mitsuba-renderer/rgb2spec 721145de (its LM variant), double precision, strict FP, 1 nm quadrature with D65 Y=1; ported into rgb2spec_opt_port.cpp and #included by specgen.cpp (CMakeLists is outside Scope).
- specgen cannot include rgb2spec.h (kernel headers need the kernel include path), so the .jh header constants are repeated in specgen.cpp; test_rgb2spec checks the committed file through jh_view.
- Float vs double: 1e-6 is not reachable in float (measured 1.0e-5 worst); the f32 copy is held to 2e-5*max(1,v), the f64 copy to 1e-6, both against NumPy literals.
- Canary tasks/023.canary.patch: sigmoid without the 1/2 factor and without the infinity guard; turns round trip and white is one red in both copies (verified, reverted).

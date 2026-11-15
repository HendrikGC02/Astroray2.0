# 024 Wavelength table and Planck's law
Goal: paths cannot draw wavelengths and blackbodies cannot be evaluated -> a host-built `WavelengthTable` (visible sech^2 component, 5% floor, empty `lines` hook) and its stratified kernel sampler exist, and Planck's law is a float-safe kernel function with a Y-normalised host helper.
Read first: 0003 d2 (stratification in CDF space, never stored), d3 (table, support, components, floor, bin cap), d24 (precision), d25 (Planck form and constants), d11 (Blackbody Y normalisation); 0003 Verification checks 7, 8, 10, 11.
Scope: src/host/spectral/wavelength_table.{h,cpp}, src/kernel/spectral/wavelength.h, src/kernel/spectral/planck.h, src/host/spectral/blackbody.{h,cpp}, tests/cpu/test_wavelengths.cpp, tests/cpu/test_planck.cpp (+ f64 copies).
Out of scope: `response` and `lines` component contents and the bin cap (M4, spike E); film use (046); generate_camera (054).
Reference: pbrt-v4 `src/pbrt/util/spectrum.h` `SampleVisibleWavelengths` / `VisibleWavelengthsPDF` and `SampledWavelengths::SampleVisible` (Apache-2.0); Wilkie et al. 2014 (CDF-space stratification); Hesterberg 1995 (defensive mixture); CODATA 2018 (Tiesinga et al. 2021) for c1L and c2.
Do:
1. Host builder (double): support [360, 830] nm while only display channels exist; `visible` component = pbrt's sech^2 pdf on 1024 uniform bins; 5% uniform floor over the support; `add_lines(component list)` exists and accepts an empty list (M4 fills it); output K+1 bin edges and K+1 CDF values as float; asserts pdf > 0 wherever a channel responds. Built once per epoch, immutable.
2. Kernel `sample_film_wavelengths(const WavelengthTableView&, Real u) -> {Lambda lf, Spec pf}`: lane i uses u_i = frac(u + i/4), binary search over the CDF, pdf constant per bin; never stored.
3. planck.h: `planck_radiance(Real lambda_nm, Real T)` = c1L lambda^-5 * e^-x / (-expm1(-x)), x = c2/(lambda T), lambda in micrometres, c1L = 1.191042972e8 W um^4 m^-2 sr^-1, c2 = 14387.7688 um K, times 1e-3 per nm; no overflow on [0.1 nm, 1 m] x [3 K, 1e9 K].
4. blackbody.{h,cpp} (host, double): `blackbody_y_normalised(T)` returns the scale 1/Y(Planck(T)) with Y over 360-830 nm from the observer data (023's observer.h), for the Blackbody node and lamp temperatures later.
5. Tests: check 8 (lane CDF positions exactly frac(u + i/4), fast); check 7 display-only part (2^20 samples estimate the integral of ybar times a smooth test spectrum; mean passes the harness against deterministic quadrature; pdf > 0 everywhere on the support; gate); check 10 (float vs double over a log grid of lambda in [0.1 nm, 1 m] and T in [3 K, 1e9 K]: relative error <= (2x + 8) * 2^-23 where B >= 1e-30 of the peak, no Inf or NaN; the 6500 K chromaticity xy within 1e-3 of x = 0.3135, y = 0.3237 from the Planckian locus table of Wyszecki and Stiles 1982, Table 1(3.11), fast); check 11 Blackbody part (Y of the normalised spectrum = 1 within 1e-4 at 1000, 6500 and 20000 K, fast).
Acceptance: `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=gate -tc="wavelengths/*"` passes; the check 10 test prints the worst relative error and its (lambda, T).
Look at: none.
Canary: tasks/024.canary.patch must turn red: astroray_tests_cpu "wavelengths/stratified lane positions", "planck/float vs double".
Budget: none.
Depends on: 021, 023.
Size: M (about 350 lines); stop and report at 2x.
Progress:
- Done: host table (1024 visible bins, exact bin masses from the tanh antiderivative, 5% floor, add_lines throws on non-empty), kernel sampler, planck_radiance_t<R> (double for the host), blackbody_y_normalised. Check 10 worst: 5.566e-07 at 2.06326 nm, 9.69001e+07 K (0.573 of bound); 102 subnormal-reference points skipped (below FLT_MIN, bound not defined). Canary tasks/024.canary.patch verified red.

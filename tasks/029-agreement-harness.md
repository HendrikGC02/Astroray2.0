# 029 CPU/GPU agreement harness (ladder steps ii and iii)
Goal: one-source divergence between CPU and GPU cannot be measured -> a harness evaluates a function or stage on 10^6 fixed host-generated inputs on the nofma and production GPU builds and on CPU float and double, and applies 0002 D12's thresholds and outlier rule; Planck and the Jakob-Hanika lookup are its first users.
Read first: 0002 D12 (thresholds, outlier rule, raw f never gated), C1 (why p99.9), D4 (per-stage fixture); 0009 d17 (ladder), Verification row 17; spike A results section 6 (docs/spikes/A-kernel-budget/RESULTS.md).
Scope: tests/gpu/agree.h, tests/gpu/agree.cpp, tests/gpu/kernels/agree_pure.cu, tests/gpu/test_agree_pure.cpp, tests/gpu/test_agree_pure_f64.cpp (CPU double reference compiled with `AR_REAL_DOUBLE`), CMakeLists.txt.
Out of scope: per-stage fixtures (each stage task adds its own using this harness); AOV agreement (077); beauty-image Welch tests (M2).
Reference: spike A `measure_agree.ps1` and its compare program (read with `git show spikes-final:spikes/A-kernel-budget/measure_agree.ps1`) for the protocol.
Do:
1. agree.h: `Inputs` generated on the host from a fixed seed through the sampler's hash (no rand); `compare(results_a, results_b, metric)` computing per input the 4-lane relative L2 error (or the angle in radians for directions, or exact equality for choices); returns max, p99.9 and the fraction above 10x the threshold.
2. Threshold presets exactly as 0002 D12: `NOFMA_VS_CPU_FLOAT` {eval max <= 1e-5, throughput p99.9 <= 1e-4, direction p99.9 <= 1e-5 rad, choice disagreement <= 1e-5 of inputs}; `PROD_VS_CPU_F64` {throughput p99.9 <= 5e-4, eval p99.9 <= 1e-4, direction p99.9 <= 1e-5 rad}; outliers: fewer than 0.01% above 10x a p99.9 threshold. Every max is printed.
3. Test modules under tests/gpu/kernels/ are built in both GPU variants (027); the harness runs the same input array through nofma, production, CPU float (same test binary) and CPU double (the f64 object) and checks (ii) nofma vs CPU float and (iii) production vs CPU double.
4. First users (gate): `planck_radiance` on a log grid of (lambda, T) inside the range where B >= 1e-30 of the peak; `sigmoid_eval(fetch_coeffs(rgb), lambda)` on random RGB and lambda; both as the eval class.
5. Detector check (0009 Verification row 17): building the nofma test module once with `--fmad=true` through a test-only CMake option `AR_AGREE_CANARY_FMA` makes step (ii) fail on a cancellation-sensitive function included in agree_pure.cu (a*b - c*d with near-equal products); acceptance runs it once.
Acceptance: `out/bin/astroray_tests_gpu.exe --tier=gate -tc="agree/*"` passes and prints max and p99.9 per function and per step; configuring with `-DAR_AGREE_CANARY_FMA=ON`, rebuilding and re-running makes `agree/fma detector` fail (paste the line), then the option is turned off.
Look at: none.
Canary: tasks/029.canary.patch must turn red: astroray_tests_gpu "agree/planck nofma vs cpu float".
Budget: adds about one second of GPU test time; report it.
Depends on: 028, 023, 024.
Size: M (about 350 lines); stop and report at 2x.
Progress:
- Harness: tests/gpu/agree.h and agree.cpp (compare, judge, run_gpu); agree_pure.cu holds planck, sigmoid and the cancel function; step (iii) CPU double is test_agree_pure_f64.cpp.
- Results (1e6 inputs): planck (ii) max 4.4e-7, (iii) max 7.0e-6; sigmoid (ii) max 0 (bit-equal), (iii) max 6.8e-5 p99.9 2.1e-5; all inside D12.
- AR_AGREE_CANARY_FMA: no -D through build.ps1, so CMake also reads the env var of that name; delete out/build/sources.txt to force the configure. ON makes sigmoid (ii) and agree/fma detector fail (checked).
- Canary: planck T and sigmoid lambda scaled by 1.0001f in agree_pure.cu; reds planck and sigmoid (ii) and (iii). Also mutation-checked compare (no sqrt), p99.9 rank, outlier < to <=.

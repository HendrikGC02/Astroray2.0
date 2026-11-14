# 064 E_spec table and the M1a lobe stack (glossy-diffuse layering)
Goal: diffuse and specular cannot be combined without gaining energy -> a host tool generates the directional-albedo table of the M1a GGX dielectric lobe from the engine's own lobe code, and one lobe-stack function layers specular over diffuse by OpenPBR albedo scaling with one selection function shared by eval, sample and pdf.
Read first: 0005 item 4 (layering tree, one lobe-selection function), item 5 (albedo scaling f = f_top + (1 - E_top) f_bottom), item 6 (tables generated from our own lobe code, checked in with a hash, roughness axis r), 0003 d6 (surface decisions use permutation-invariant lane reductions); 0005 Verification 1 (furnace targets).
Scope: tools/etables/etables.cpp, data/tables/e_diel_refl.bin, data/MANIFEST.toml, src/kernel/material/lobes/tables.h, src/kernel/material/lobes/stack.h, tests/cpu/test_lobe_stack.cpp.
Out of scope: the other E tables (E_avg, E_glass, metal, coat: M1b and M2), Turquin compensation (M2); emission attenuation by coat (no coat yet).
Reference: OpenPBR 1.1.2 specification, layering by albedo scaling; Kulla and Conty 2017 (SIGGRAPH course) for directional-albedo tables; Cycles `src/app/cycles_precompute.cpp` (Apache-2.0) as the pattern of a table generator.
Do:
1. etables.cpp (target `etables`, EXCLUDE_FROM_ALL, links the CPU float kernel headers): E_diel(mu, r, eta) 16^3 reflection-only, mu = cos theta_o, r = (alpha_x alpha_y)^(1/4) as 0005 item 6 states, eta over the IOR range the table must cover (1.0 to 3.0, recorded in the header); importance-sampled with 063's sampler at 2^16 samples per cell; writes data/tables/e_diel_refl.bin with a header (dims, axes, generator version, hash of the lobe source file) and a MANIFEST entry. Run by hand: `out/bin/etables.exe e_diel_refl`.
2. tables.h: trilinear lookup of the table (embedded via 022's mechanism, uploaded once per scene in the tables ledger category).
3. stack.h: `stack_eval/pdf/sample(const OpenPBRParams&, frame, wo, wi|u, mode)`: dielectric base = specular (063) over diffuse (062) with weight (1 - specular_weight * E_spec(mu_o)) applied per lane on the diffuse part; lobe choice probability from the lane-mean of each lobe's albedo estimate (one function used by all three entry points); delta specular samples carry DELTA; result multiplied by closure_weight.
4. Tests: fast: table lookup at grid points equals stored values; selection probabilities sum to 1 and eval of the mixture equals the sum of weighted lobes; gate: white furnace (base_color 1, specular_weight 1) of the stack at roughness 0.25, 0.5, 1 and IOR 1.5: 0.99 <= E <= 1.002 per 0005 Verification 1 for the lossless configuration; chi-squared of the mixture sampler.
Acceptance: `out/bin/etables.exe e_diel_refl` reproduces the committed table byte for byte and prints its run time; `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=gate -tc="lobe_stack/*"` prints the furnace E per configuration.
Look at: none.
Canary: tasks/064.canary.patch must turn red: astroray_tests_cpu "lobe_stack/white furnace", "lobe_stack/selection shared".
Budget: none on build (generator outside the default target).
Depends on: 063, 022.
Size: M (about 350 lines); stop and report at 2x.
Progress:

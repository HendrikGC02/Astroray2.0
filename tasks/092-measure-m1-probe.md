# 092 Measurement: M-1 probe (record, register file, VM and lobes in the capped kernel) and upsampling cost
Goal: it is unknown whether the full-scope fused kernel fits its stack and spill allowances -> a gate-only probe TU with the real 304 B record, the 32-register file, the full `ops.def` dispatch and spike A's lobes standing in for every OpenPBR lobe is compiled under (256,2), and the cost of Jakob-Hanika upsampling inside shade_surface is measured; each outcome has a stated consequence.
Read first: 0005 Risks M-1 and items 2-3 (record, stack split), 0002 D10 (allowances, ladder), Risks 1; 0003 Risks 2 (upsampling cost); docs/ROADMAP.md Measurements table row M-1; docs/spikes/A-kernel-budget/RESULTS.md sections 2-3, 8.
Scope: src/kernel_gpu/probe_m1.cu (built only by `check full` and this task, never embedded), src/kernel/budget.toml (probe row, gated `full`), tools/measure_upsampling.ps1 (one-off; deleted when this task lands); lead ruling 2026-11-19: cmake/kernels.cmake (probe_*.cu also built capped into kernels_probe_capped/, with the probe target only), src/kernel/launch_config.h (one row ar_probe_m1), tools/kernelgate.py (--full gates the capped probe rows, --probe labels capped vs uncapped); lead ruling 2026-11-19 (finish): tools/check.ps1 (build probe before kernelgate full), tests/budgets/kernelgate_cases/ (probe-path self-test case).
Out of scope: changing record layouts (an outcome may require a record amendment by the lead, not this task).
Reference: spike A `include/lobes.h` and `include/stages.h` (read with `git show spikes-final:spikes/A-kernel-budget/include/lobes.h`), project code, as the stand-in lobes.
Do:
1. probe_m1.cu: one `__launch_bounds__(256, 2)` kernel that resolves `OpenPBRParams` through the real interpreter with every M1a opcode plus placeholder cases sized like the ~100-opcode target (a switch over ops.def rows), then evaluates spike A's fused lobe set (fuzz, coat, metal with complex Fresnel, dielectric reflection and transmission with thin film, diffuse) with NEE calling an emission program in the same register file; built in the production configuration.
2. Read registers, stack and spills (cuobjdump and ptxas) and compare with 0002 D10's VM allowances (spills <= 128/192 B, stack <= 512 B) and 0005 item 3's split (record <= 304 B, register file 128 B).
3. Upsampling: B-1M-like scene (b1m recording) kernel time per 1080p sample with RGB_LIFT as implemented vs a test build that replaces the lookup by a constant spectrum; report the share.
4. Result (PR body `Result:` with commands and numbers). On each outcome: stack within 512 B and spills within allowance -> M-1 probe passes, record noted in STATUS for ratification in M1b; stack over -> propose fp16 record scalars (-48 B) to the lead as a 0005 amendment before M1b; spills over -> start 0002 D10's ladder (attribute by stubbing, then local fixes, then the cap A/B) as follow-up tasks the lead writes; upsampling share above 20% of kernel time -> report to the lead with the profile (0003 Risk 2), otherwise record and close.
Acceptance: the PR body holds the probe's ptxas line, cuobjdump line, the two kernel times and the outcome chosen; `pwsh tools/check.ps1 full` builds the probe and prints its registers as a warning only.
Look at: none.
Canary: tasks/092.canary.patch must turn red: `tools/kernelgate.py --self-test` (case probe_breach_warns: the patch keeps a capped probe breach as a failure and prints no probe warning).
Budget: adds a `full`-only TU.
Depends on: 030, 057, 064, 089.
Size: M (probe TU about 300 lines plus result); stop and report at 2x.
Progress:
- started
- stopped before code: budget row needs a launch_config.h row (out of Scope); probe_ TUs build only uncapped; b1m never runs RGB_LIFT. Questions to lead.
- lead rulings: Scope grows (kernels.cmake, launch_config.h, kernelgate.py); upsampling measured both on b1m (resolve() RGB branch) and b_tx (RGB_LIFT).
- probe capped (256,2): 128 regs, 480 B stack, 130/376 B spills; uncapped 160 regs, 384 B stack. Stack within 512, spills over 128/192: D10 ladder (lead).
- upsampling (tools/measure_upsampling.ps1): b1m resolve() RGB branch 8.4% of kernel time (5.7% per closest ray); b_tx op_rgb_lift 0.5%. Both below 20%: record and close.
- finish rulings: check full builds the probe before kernelgate full; self-test case probe_breach_warns with canary; measure_upsampling.ps1 deleted (commands in the PR body).

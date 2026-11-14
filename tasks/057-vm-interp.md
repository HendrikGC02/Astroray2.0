# 057 The VM interpreter and the M1a opcodes
Goal: material programs cannot run -> a forward-only interpreter executes bytecode against a kernel-owned register file passed by pointer, fills `OpenPBRParams` and emission through STORE_SLOT and EMIT_LE, and every M1a opcode is a kernel-library function whose host constant-folded result equals its interpreted result bit for bit.
Read first: 0005 item 30 (ops are kernel-library functions; folding calls the CPU build), item 31 (encoding, registers, forward-only control flow), item 32 (entry table, one register file per kernel), item 33 (evaluation context), item 35 (constant folding, length-0 programs), 0005 Verification 6 (VM checks).
Scope: src/kernel/vm/interp.h, src/kernel/vm/context.h, src/kernel/vm/ops/*.h, tests/cpu/test_vm.cpp, tests/gpu/test_vm_agree.cpp, tests/gpu/kernels/vm_agree.cu.
Out of scope: TEX_IMAGE filtering (061 implements the op body; here it is declared and returns magenta until then); the compiler (058); BRANCH_MIX/BRANCH_ADD (M1b/M2).
Reference: Cycles `kernel/svm/svm.h` (Apache-2.0) for interpreter structure.
Do:
1. Encoding: 32-bit words; instruction word = opcode u8 | a u8 | b u8 | c u8 (register operands); immediates follow; f32x3 values occupy three consecutive registers.
2. `run_entry(const uint32_t* prog, uint32_t offset, VmRegs* regs, const ShadeContext&, const SceneTables&, OpenPBRParams* rec, Spec* le)`: loop until END; only forward jumps (`JMP` asserts off > 0 in debug); length bounds time; never declares its own register file.
3. context.h: `ShadeContext` {P, Ng, N, front_face, wo, uv sets by index, object and instance ids, ray type flags and depths, lambda_t}.
4. Op bodies (one file each under vm/ops/): CONST_*, LOAD_CTX_UV, LOAD_CTX_P, MUL_*, ADD_F32X3, STORE_SLOT (writes a scalar or an RGB colour into a record slot; RGB colours stay RGB until lifted), RGB_LIFT (023's lookup at lambda_t with the slot's semantic), EMIT_LE (adds colour x strength into Le using the ILLUMINANT lift), UNSUPPORTED (unreachable at run time: the compiler replaces such programs), TEST_INJECT_NAN (gate hook builds only).
5. Tests (fast): per opcode, 10^4 random inputs: the host fold (CPU build of the same function) equals the interpreted CPU result bit-exactly; a program run twice in sequence in one register file (surface then an emission program) gives Le bit-identical to the emission program run alone (register-file reuse, 0005 V6); GPU (gate): vm_agree.cu runs the same programs, nofma within 1e-6 relative of CPU float.
Acceptance: `pwsh tools/check.ps1 gate` passes with `vm/*` and `vm_agree/*` cases; the GPU test prints max relative error per opcode.
Look at: none.
Canary: tasks/057.canary.patch must turn red: astroray_tests_cpu "vm/fold equals interpret", "vm/register file reuse".
Budget: none yet (no shade kernel); the interpreter's register use is measured in 092.
Depends on: 056, 028, 029.
Size: M (about 400 lines); stop and report at 2x.
Progress:

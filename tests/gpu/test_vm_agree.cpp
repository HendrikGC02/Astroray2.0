// SPDX-License-Identifier: Apache-2.0
// VM agreement (0005 Verification 6, 0009 d17 step ii): the same bytecode programs run through
// the interpreter on the CPU (float) and on the GPU built without FMA contraction, on 10^5 fixed
// host-generated inputs; every program's result (registers, Le, record fields) agrees within
// 1e-6 relative error, and the largest error per opcode is printed. The programs cover every
// opcode; TEX_IMAGE runs without an image table (the missing-image path), and its filtering
// agreement is test_tex_image_agree.cpp.
// Ref: docs/decisions/0005-material-model.md Verification 6; docs/decisions/0002-kernel-
// architecture.md D12; 0009 d17.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "agree.h"
#include "host/data_resource.h"
#include "host/device.h"
#include "vm/interp.h"

using namespace astroray;
using namespace astroray::agree;
using astroray::host::DeviceBuffer;
using astroray::host::Ledger;
using astroray::host::LedgerCategory;
using astroray::host::Owner;
using ar::Op;

namespace {

constexpr uint32_t kN = 100000;
constexpr uint32_t kIn = 27, kOut = 30;
constexpr double kMaxRel = 1e-6;  // Verification 6: GPU no-FMA within 1e-6 relative of CPU float

std::string nofma_module(const char* name) { return std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/" + name + ".cubin"; }

std::vector<uint8_t> read_data(const std::string& rel) {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/" + rel, std::ios::binary);
  if (!f) f.open("data/" + rel, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/" << rel << " not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Tables {
  std::vector<uint8_t> jh;
  ObserverTables obs;
  ar::Rgb2SpecTable table = {};
  Tables() {
    const auto ob = read_data("spectral/observer.bin");
    const std::string err = astroray::parse_observer(ob.data(), ob.size(), &obs);
    REQUIRE_MESSAGE(err.empty(), err);
    jh = read_data("spectral/rec709.jh");
    const char* e = ar::jh_view(jh.data(), jh.size(), &table);
    REQUIRE_MESSAGE(e == nullptr, (e ? e : ""));
  }
};

// Row: r0..r15 in [-4, 4) (so colours go below 0 and above 1, where the lifts clamp), P in
// [-4, 4), two UV sets in [0, 1), four wavelengths in [360, 830).
Inputs make_inputs() {
  Inputs in;
  in.n = kN;
  in.stride = kIn;
  in.v.resize(size_t(kN) * kIn);
  for (uint32_t i = 0; i < kN; ++i) {
    float* r = in.v.data() + size_t(i) * kIn;
    for (uint32_t d = 0; d < 19; ++d) r[d] = float(double(unit(i, d)) * 8.0 - 4.0);
    for (uint32_t d = 19; d < 23; ++d) r[d] = float(unit(i, d));
    for (uint32_t d = 23; d < 27; ++d) r[d] = float(360.0 + 470.0 * unit(i, d));
  }
  return in;
}

struct Prog {
  std::vector<uint32_t> w{0, 0, 0, 0};  // entry table, then code from word 4
  Prog& op(Op o, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, std::initializer_list<uint32_t> imm = {}) {
    w.push_back(uint32_t(o) | (a << 8) | (b << 16) | (c << 24));
    for (uint32_t v : imm) w.push_back(v);
    return *this;
  }
};
uint32_t fb(float f) { return ar::bit_cast<uint32_t>(f); }

struct Named {
  const char* name;
  Prog prog;
};

std::vector<Named> programs() {
  const uint32_t base_weight = uint32_t(ar::RecordSlot::base_weight), base_color = uint32_t(ar::RecordSlot::base_color),
                 spec_ior = uint32_t(ar::RecordSlot::specular_ior);
  std::vector<Named> p;
  p.push_back({"CONST_F32,CONST_F32X3", Prog()
                                            .op(Op::CONST_F32, 16, 0, 0, {fb(0.1f)})
                                            .op(Op::CONST_F32X3, 17, 0, 0, {fb(1.5f), fb(-2.25f), fb(1e-3f)})
                                            .op(Op::END)});
  p.push_back({"MUL_F32", Prog().op(Op::MUL_F32, 16, 0, 1).op(Op::END)});
  p.push_back({"MUL_F32X3", Prog().op(Op::MUL_F32X3, 16, 0, 3).op(Op::END)});
  p.push_back({"MUL_F32X3_F32", Prog().op(Op::MUL_F32X3_F32, 16, 0, 3).op(Op::END)});
  p.push_back({"ADD_F32X3", Prog().op(Op::ADD_F32X3, 16, 0, 3).op(Op::END)});
  p.push_back({"ADD_F32,SUB_F32", Prog().op(Op::ADD_F32, 16, 0, 1).op(Op::SUB_F32, 17, 2, 3).op(Op::END)});
  // A Mix selection: the selection uniform is UV set 0's u and the factor UV set 1 (both in
  // [0, 1)); output column 15 is the rescaled uniform and r22 names the side taken.
  p.push_back({"BRANCH_MIX", Prog()
                                 .op(Op::LOAD_CTX_UV, 19, 1)                // word 0
                                 .op(Op::BRANCH_MIX, 19, 0, 0, {6u})        // 1-2, to word 7
                                 .op(Op::CONST_F32, 22, 0, 0, {fb(1.0f)})   // 3-4
                                 .op(Op::JMP, 0, 0, 0, {4u})                // 5-6, to word 9
                                 .op(Op::CONST_F32, 22, 0, 0, {fb(2.0f)})   // 7-8
                                 .op(Op::END)});                            // 9
  p.push_back({"CHANNEL_F32X3", Prog()
                                    .op(Op::CHANNEL_F32X3, 16, 3, 0, {0u})
                                    .op(Op::CHANNEL_F32X3, 17, 3, 0, {1u})
                                    .op(Op::CHANNEL_F32X3, 18, 3, 0, {2u})
                                    .op(Op::END)});
  p.push_back({"LOAD_CTX_UV,LOAD_CTX_P", Prog()
                                             .op(Op::LOAD_CTX_UV, 16, 0)
                                             .op(Op::LOAD_CTX_UV, 19, 1)
                                             .op(Op::LOAD_CTX_P, 22)
                                             .op(Op::END)});
  // No image table here: every slot is missing and samples magenta (tex_agree.cu covers filtering).
  p.push_back({"TEX_IMAGE", Prog().op(Op::TEX_IMAGE, 16, 0, 0, {0u, 0u}).op(Op::TEX_IMAGE, 19, 0, 0, {5u, 3u}).op(Op::END)});
  p.push_back({"JMP", Prog()
                          .op(Op::CONST_F32, 16, 0, 0, {fb(1.0f)})
                          .op(Op::JMP, 0, 0, 0, {4u})
                          .op(Op::CONST_F32, 16, 0, 0, {fb(2.0f)})
                          .op(Op::MUL_F32, 17, 16, 0)
                          .op(Op::END)});
  p.push_back({"RGB_LIFT", Prog()
                               .op(Op::RGB_LIFT, 16, 0, 0)
                               .op(Op::RGB_LIFT, 20, 0, 1)
                               .op(Op::RGB_LIFT, 24, 0, 2)
                               .op(Op::END)});
  p.push_back({"STORE_SLOT", Prog()
                                 .op(Op::STORE_SLOT, 0, 0, 0, {base_weight})
                                 .op(Op::STORE_SLOT, 1, 0, 0, {base_color})
                                 .op(Op::STORE_SLOT, 4, 0, 0, {spec_ior})
                                 .op(Op::END)});
  p.push_back({"EMIT_LE", Prog().op(Op::EMIT_LE, 0).op(Op::EMIT_LE, 4).op(Op::END)});
  // The two spectral sources: warm and cool Blackbody, and two glasses (the normal and a low-n one).
  p.push_back({"BLACKBODY", Prog()
                                .op(Op::BLACKBODY, 16, 0, 0, {fb(2700.0f), fb(1.0f)})
                                .op(Op::BLACKBODY, 20, 0, 0, {fb(12000.0f), fb(0.5f)})
                                .op(Op::END)});
  p.push_back({"CAUCHY_ABBE", Prog()
                                  .op(Op::CAUCHY_ABBE, 16, 0, 0, {fb(1.5168f), fb(64.17f), fb(1.0f)})
                                  .op(Op::CAUCHY_ABBE, 20, 0, 0, {fb(1.33f), fb(55.0f), fb(0.7f)})
                                  .op(Op::END)});
  return p;
}

// The CPU float reference: the interpreter on the same inputs, packed like the kernel's output.
Results cpu_run(const Prog& p, const Inputs& in, const Tables& tb) {
  Results r;
  r.n = in.n;
  r.stride = kOut;
  r.v.assign(size_t(in.n) * kOut, 0.0);
  ar::SceneTables tables = {};
  tables.images = nullptr;  // no image table: TEX_IMAGE samples magenta, as in vm_agree.cu
  tables.image_count = 0;
  tables.rgb2spec = &tb.table;
  tables.observer.cmf = &tb.obs.cmf[0][0];
  tables.observer.illum = tb.obs.d65;
  for (uint32_t i = 0; i < in.n; ++i) {
    const float* row = in.v.data() + size_t(i) * kIn;
    ar::VmRegs regs = {};
    for (uint32_t k = 0; k < 16; ++k) regs.r[k] = row[k];
    ar::ShadeContext ctx = {};
    ctx.P = ar::Real3{row[16], row[17], row[18]};
    ctx.uv[0] = ar::Real2{row[19], row[20]};
    ctx.uv[1] = ar::Real2{row[21], row[22]};
    for (int l = 0; l < 4; ++l) ctx.lambda_t.nm[l] = row[23 + l];
    ar::OpenPBRParams rec = {};
    ar::Spec le = {};
    float select_u = row[19];  // the selection uniform BRANCH_MIX reads and rescales
    const ar::VmStatus st = ar::run_entry(p.w.data(), 4, &regs, ctx, tables, &rec, &le, &select_u);
    double* o = r.v.data() + size_t(i) * kOut;
    for (uint32_t k = 0; k < 16; ++k) o[k] = regs.r[16 + k];
    o[15] = select_u;  // in r31's column: no program writes r31
    for (int l = 0; l < 4; ++l) {
      o[16 + l] = le.v[l];
      o[21 + l] = rec.base_color.v[l];
      o[25 + l] = rec.specular_ior.v[l];
    }
    o[20] = rec.base_weight;
    o[29] = double(uint32_t(st));
  }
  return r;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("vm_agree/gpu nofma vs cpu float per opcode") {
  const Tables tb;
  const Inputs in = make_inputs();
  Ledger ledger;
  auto dev = host::make_cuda_device(ledger, 0);

  const uint64_t z_bytes = sizeof(float) * uint64_t(tb.table.res);
  const uint64_t c_bytes = sizeof(float) * 9ull * uint64_t(tb.table.res) * tb.table.res * tb.table.res;
  const uint64_t il_bytes = sizeof(float) * uint64_t(ar::kObsSamples);
  DeviceBuffer z = dev->allocate(LedgerCategory::TABLES, z_bytes, Owner{});
  DeviceBuffer c = dev->allocate(LedgerCategory::TABLES, c_bytes, Owner{});
  DeviceBuffer il = dev->allocate(LedgerCategory::TABLES, il_bytes, Owner{});
  dev->upload(z, 0, tb.table.z_nodes, z_bytes);
  dev->upload(c, 0, tb.table.coeffs, c_bytes);
  dev->upload(il, 0, tb.obs.d65, il_bytes);

  for (const Named& np : programs()) {
    DeviceBuffer pb = dev->allocate(LedgerCategory::TABLES, sizeof(uint32_t) * np.prog.w.size(), Owner{});
    dev->upload(pb, 0, np.prog.w.data(), sizeof(uint32_t) * np.prog.w.size());
    uint64_t prog_p = reinterpret_cast<uint64_t>(pb.ptr), z_p = reinterpret_cast<uint64_t>(z.ptr),
             c_p = reinterpret_cast<uint64_t>(c.ptr), il_p = reinterpret_cast<uint64_t>(il.ptr);
    uint32_t entry = 4;
    int32_t res = tb.table.res;
    const Results gpu = run_gpu(*dev, nofma_module("vm_agree"), "ar_vm_agree", in, kOut,
                                {&prog_p, &entry, &z_p, &c_p, &res, &il_p});
    const Results cpu = cpu_run(np.prog, in, tb);
    dev->free(pb);

    const Stats s = compare(gpu, cpu, Metric::REL_L2);
    std::printf("vm_agree %-24s max relative error %.3e (p99.9 %.3e, n=%u)\n", np.name, s.max, s.p999, s.n);
    INFO(np.name);
    CHECK(s.max <= kMaxRel);
    // Every run finished with VmStatus::OK on both targets (column 29).
    double status_sum = 0;
    for (uint32_t i = 0; i < in.n; ++i) status_sum += gpu.v[size_t(i) * kOut + 29] + cpu.v[size_t(i) * kOut + 29];
    CHECK(status_sum == 0.0);
  }

  dev->free(z);
  dev->free(c);
  dev->free(il);
  CHECK(ledger.total() == 0);
}

}  // TEST_SUITE

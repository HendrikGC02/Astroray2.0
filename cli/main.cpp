// SPDX-License-Identifier: Apache-2.0
// astroray_cli: command-line host over the public C ABI only (0001 D1.4).
// Ref: 0001 D2.7 (exact hash match on load), D2.8 (explicit device, no fallback).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "abi_hash.h"
#include "astroray/astroray.h"

namespace {

const char* status_name(ar_status s) {
  switch (s) {
    case AR_OK: return "AR_OK";
    case AR_ERR_INVALID_ARGUMENT: return "AR_ERR_INVALID_ARGUMENT";
    case AR_ERR_UNSUPPORTED: return "AR_ERR_UNSUPPORTED";
    case AR_ERR_DEVICE: return "AR_ERR_DEVICE";
    case AR_ERR_OUT_OF_MEMORY: return "AR_ERR_OUT_OF_MEMORY";
    case AR_ERR_ABI_MISMATCH: return "AR_ERR_ABI_MISMATCH";
    default: return "AR_ERR_INTERNAL";
  }
}

std::string text(const uint8_t* p, size_t cap) {
  size_t n = 0;
  while (n < cap && p[n]) ++n;
  return std::string(reinterpret_cast<const char*>(p), n);
}

int info_cmd(int argc, char** argv) {
  const char* create = nullptr;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--create") && i + 1 < argc) {
      create = argv[++i];
    } else {
      fprintf(stderr, "info: unknown argument '%s'\n", argv[i]);
      return 1;
    }
  }

  uint64_t hash = 0;
  ar_status st = ar_abi_hash(&hash);
  if (st != AR_OK) { fprintf(stderr, "ar_abi_hash failed: %s\n", status_name(st)); return 1; }
  // The ABI exposes no driver query; the build record names the toolkit versions.
  ar_build_record b;
  st = ar_build_info(&b);
  if (st != AR_OK) { fprintf(stderr, "ar_build_info failed: %s\n", status_name(st)); return 1; }
  printf("driver: not exposed by the ABI (built with cuda=%s optix=%s)\n", text(b.cuda, sizeof b.cuda).c_str(),
         text(b.optix, sizeof b.optix).c_str());
  printf("abi hash: 0x%016llx\n", static_cast<unsigned long long>(hash));

  uint32_t n = 0;
  st = ar_device_count(&n);
  if (st != AR_OK) { fprintf(stderr, "ar_device_count failed: %s\n", status_name(st)); return 1; }
  printf("index kind name cc memory status reason\n");
  bool any_cuda = false;
  for (uint32_t i = 0; i < n; ++i) {
    ar_device_info d;
    st = ar_device_get_info(i, &d);
    if (st != AR_OK) { fprintf(stderr, "ar_device_get_info(%u) failed: %s\n", i, status_name(st)); return 1; }
    if (d.kind == AR_DEVICE_CUDA) any_cuda = true;
    printf("%u %s '%s' cc=%u memory=%llu MiB status=%s reason='%s'\n", i, d.kind == AR_DEVICE_CPU ? "cpu" : "cuda",
           text(d.name, sizeof d.name).c_str(), d.compute_capability,
           static_cast<unsigned long long>(d.memory_bytes >> 20), status_name(d.status),
           text(d.reason, sizeof d.reason).c_str());
  }
  if (!any_cuda)
    printf("no CUDA device: the probe listed none (device count %u, CPU only); the ABI carries no probe text\n", n);

  if (!create) return 0;
  ar_context_desc desc;
  memset(&desc, 0, sizeof desc);
  if (!strcmp(create, "cpu")) {
    desc.device_kind = AR_DEVICE_CPU;
  } else if (!strncmp(create, "cuda", 4) && (create[4] == 0 || create[4] == ':')) {
    desc.device_kind = AR_DEVICE_CUDA;
    if (create[4] == ':') desc.device_index = static_cast<uint32_t>(strtoul(create + 5, nullptr, 10));
  } else {
    fprintf(stderr, "info --create: expected cpu or cuda[:i], got '%s'\n", create);
    return 1;
  }
  // The ABI requires a log path; it lands in the working directory.
  desc.log_path = "astroray_cli.log";
  ar_context ctx = nullptr;
  st = ar_context_create(&desc, &ctx);
  if (st != AR_OK) {
    printf("context failed: %s (device %s; no fallback to another device)\n", status_name(st), create);
    return st == AR_ERR_DEVICE ? 3 : 1;
  }
  st = ar_context_destroy(ctx);
  if (st != AR_OK) { printf("context destroy failed: %s\n", status_name(st)); return 1; }
  printf("context ok\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  ar_build_record b;
  if (ar_build_info(&b) == AR_OK)
    printf("build: sha=%s dirty=%u kernels=%s\n", text(b.git_sha, sizeof b.git_sha).c_str(), b.dirty,
           text(b.kernel_source, sizeof b.kernel_source).c_str());

  uint64_t hash = 0;
  ar_status st = ar_abi_hash(&hash);
  if (st != AR_OK || hash != AR_ABI_HASH_VALUE) {
    printf("ABI mismatch: cli 0x%016llx, core 0x%016llx\n", static_cast<unsigned long long>(AR_ABI_HASH_VALUE),
           static_cast<unsigned long long>(hash));
    return 3;
  }
  if (argc < 2) {
    fprintf(stderr, "usage: astroray_cli info [--create cpu|cuda[:i]] | render | bench | dump\n");
    return 1;
  }
  if (!strcmp(argv[1], "info")) return info_cmd(argc, argv);
  if (!strcmp(argv[1], "render") || !strcmp(argv[1], "bench") || !strcmp(argv[1], "dump")) {
    fprintf(stderr, "%s: not implemented until task 069\n", argv[1]);
    return 2;
  }
  fprintf(stderr, "unknown command '%s'\n", argv[1]);
  return 1;
}

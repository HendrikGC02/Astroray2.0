// SPDX-License-Identifier: Apache-2.0
// astroray_cli bench: renders a recording and prints one history line per metric.
// Ref: 0009 d22 (history line fields: sha, machine, driver, cuda, scene, metric, value, unit, n,
// spread; the date is the file's concern, Ruling 71); 0004 D29 (per-pass counters; words 0-3 are rays_closest, rays_shadow, steps, launches).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "cli.h"

namespace cli {
namespace {

std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) o += c;
  }
  return o;
}

std::string stem(const std::string& path) {
  size_t b = path.find_last_of("/\\");
  b = b == std::string::npos ? 0 : b + 1;
  const size_t d = path.rfind('.');
  return path.substr(b, d == std::string::npos || d < b ? std::string::npos : d - b);
}

const char* state_name(uint32_t state) {
  switch (state) {
    case AR_STATE_INITIALIZING: return "INITIALIZING";
    case AR_STATE_RENDERING: return "RENDERING";
    case AR_STATE_PAUSED: return "PAUSED";
    case AR_STATE_CONVERGED: return "CONVERGED";
    case AR_STATE_TRIMMED: return "TRIMMED";
    case AR_STATE_CLOSED: return "CLOSED";
    case AR_STATE_FAILED: return "FAILED";
    default: return "unknown";
  }
}

// What a failed bench prints about its session: the state and the generation counters, so a scene
// whose generation never activates is told apart from one that renders slowly.
void print_session_state(const char* why, const ar_progress& p) {
  printf("bench: %s: state %s, generation pending %llu, active %llu, failed %llu, %u spp\n", why,
         state_name(p.state), static_cast<unsigned long long>(p.pending_generation),
         static_cast<unsigned long long>(p.active_generation), static_cast<unsigned long long>(p.failed_generation),
         p.samples_per_pixel);
  if (p.active_generation < p.pending_generation && p.failed_generation != p.pending_generation)
    printf("bench: generation %llu has not activated\n", static_cast<unsigned long long>(p.pending_generation));
}

// A neutral hardware id for the history lines: GPU name with the vendor words and blanks removed, physical
// cores and logical threads ("rtx5070ti-8c16t"), the form bench/history carries. No host or user name.
std::string hardware_id() {
  std::string gpu = "cpu";
  uint32_t n = 0;
  ar_device_count(&n);
  for (uint32_t i = 0; i < n; ++i) {
    ar_device_info d;
    if (ar_device_get_info(i, &d) != AR_OK || d.kind != AR_DEVICE_CUDA) continue;
    gpu.clear();
    for (char c : text(d.name, sizeof d.name)) {
      if (c != ' ') gpu += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const char* word : {"nvidia", "geforce"}) {
      const size_t at = gpu.find(word);
      if (at != std::string::npos) gpu.erase(at, std::strlen(word));
    }
    break;
  }
  DWORD bytes = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
  std::vector<char> buf(bytes);
  unsigned cores = 0;
  if (bytes && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &bytes)) {
    for (DWORD off = 0; off < bytes;) {
      ++cores;
      off += reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off)->Size;
    }
  }
  return gpu + "-" + std::to_string(cores) + "c" + std::to_string(std::thread::hardware_concurrency()) + "t";
}

struct Line {
  std::string sha, machine, driver, cuda, scene;
  void emit(const char* metric, double value, const char* unit, size_t n, double spread) const {
    printf("{\"sha\": \"%s\", \"machine\": \"%s\", \"driver\": \"%s\", \"cuda\": \"%s\", \"scene\": \"%s\", "
           "\"metric\": \"%s\", \"value\": %.9g, \"unit\": \"%s\", \"n\": %zu, \"spread\": %.9g}\n",
           json_escape(sha).c_str(), json_escape(machine).c_str(), json_escape(driver).c_str(),
           json_escape(cuda).c_str(), json_escape(scene).c_str(), metric, value, unit, n, spread);
  }
};

}  // namespace

int bench_cmd(int argc, char** argv) {
  RunArgs a;
  if (int rc = parse_run_args("bench", argc, argv, false, true, a); rc != kContinue) return rc;
  // The pass count is the sample target, so the session stops by itself after warmup + passes.
  if (a.overrides.samples == kReplayUnset) {
    a.overrides.samples = a.warmup + a.passes;
    a.auto_samples = true;
  }
  Replayed r;
  if (int rc = r.open(a); rc != kContinue) return rc;
  if (r.res.sessions.size() != 1) {
    printf("bench: the recording creates %zu sessions; bench times exactly one\n", r.res.sessions.size());
    return kExitFailure;
  }
  ar_session s = r.res.sessions[0];
  ar_progress p{};
  if (ar_session_get_progress(s, &p) != AR_OK) { printf("bench: progress failed\n"); return kExitFailure; }
  if (p.state == AR_STATE_INITIALIZING && ar_session_start(s) != AR_OK) { printf("bench: start failed\n"); return kExitFailure; }

  // One pass is one sample per pixel. The session keeps only the last pass time, so the loop
  // polls and keeps the time of each pass it sees; a pass that ends between two polls is not
  // counted, which is why n is the number of passes observed.
  std::vector<double> ms;
  uint32_t last_spp = 0;
  const auto end = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  for (;;) {
    if (ar_session_get_progress(s, &p) != AR_OK) { printf("bench: progress failed\n"); return kExitFailure; }
    if (p.state == AR_STATE_FAILED || p.state == AR_STATE_CLOSED || p.state == AR_STATE_TRIMMED) {
      print_session_state("session ended", p);
      return kExitFailure;
    }
    // Nothing resumes a bench session, so a paused one would spin until the timeout.
    if (p.state == AR_STATE_PAUSED) {
      print_session_state("session is paused and nothing resumes it", p);
      return kExitFailure;
    }
    if (p.samples_per_pixel != last_spp) {
      last_spp = p.samples_per_pixel;
      if (last_spp > a.warmup) ms.push_back(p.last_pass_ms);
    }
    if (settled_and_converged(p)) break;
    if (std::chrono::steady_clock::now() > end) {
      print_session_state("not converged after 10 minutes", p);
      return kExitFailure;
    }
    std::this_thread::yield();
  }
  if (ms.empty()) {
    printf("bench: no pass after the %u warmup passes was observed; raise --passes\n", a.warmup);
    return kExitFailure;
  }
  uint64_t words[128] = {};
  if (ar_session_read_counters(s, 128, words) != AR_OK) { printf("bench: counters unreadable\n"); return kExitFailure; }
  const int highest = print_reports(r);

  ar_build_record b{};
  ar_build_info(&b);
  Line l;
  l.sha = text(b.git_sha, sizeof b.git_sha);
  l.machine = hardware_id();
  l.driver = "unknown";  // the ABI exposes no driver query (see info)
  l.cuda = text(b.cuda, sizeof b.cuda);
  l.scene = stem(a.recording);

  std::sort(ms.begin(), ms.end());
  const double median = ms[ms.size() / 2], spread = ms.back() - ms.front();
  l.emit("pass_ms", median, "ms", ms.size(), spread);
  // A pass is one sample for every pixel, so the samples in a pass are the film's pixels. The time is the
  // pass wall time, host work included, until kernel timers exist. Ref: Ruling 71.
  ar_pass_info pi{};
  if (ar_session_pass_info(s, 0, &pi) != AR_OK || pi.width == 0 || pi.height == 0) {
    printf("bench: the film size is unreadable\n");
    return kExitFailure;
  }
  const double pixels = double(pi.width) * double(pi.height);
  l.emit("kernel_ms_per_sample", median / pixels, "ms", ms.size(), spread / pixels);
  l.emit("rays_closest", double(words[0]), "rays", 1, 0.0);
  l.emit("rays_shadow", double(words[1]), "rays", 1, 0.0);
  l.emit("steps_per_pass", double(words[2]), "steps", 1, 0.0);
  return highest >= AR_LEVEL_ERROR ? kExitFailure : 0;
}

}  // namespace cli

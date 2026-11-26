// SPDX-License-Identifier: Apache-2.0
// astroray_cli render and dump: replay a recording through the public ABI, render every session
// to its sample count, write the film as FITS, print the final report, fail on ERROR entries.
// Ref: 0001 D1.4 (recordings), D2.10 (a replay renders each session to its sample count),
// D2.11 (the CLI prints the report; non-zero exit on ERROR), R38, R59 (no scene readback).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "cli.h"

namespace cli {

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

namespace {

bool parse_u32(const char* s, uint32_t* out) {
  char* end = nullptr;
  const unsigned long long v = strtoull(s, &end, 10);
  if (!*s || *end || v >= kReplayUnset) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

const char* level_name(uint32_t l) {
  return l == AR_LEVEL_ERROR ? "ERROR" : l == AR_LEVEL_WARNING ? "WARNING" : "INFO";
}

std::string session_error(ar_session s) {
  uint8_t msg[256] = {};
  ar_session_last_error(s, sizeof msg, msg);
  return text(msg, sizeof msg);
}

// <path without its extension><suffix>.
std::string with_suffix(const std::string& path, const char* suffix) {
  const size_t dot = path.rfind('.');
  const size_t slash = path.find_last_of("/\\");
  const bool ext = dot != std::string::npos && (slash == std::string::npos || dot > slash);
  return (ext ? path.substr(0, dot) : path) + suffix;
}

// Reads the MEAN of the pass named `name` through the ABI and writes it as a little-endian PFM
// (rows bottom-up, as the film's and PFM's). PFM, not FITS: every FITS file says DENOISED = F
// (0008 V9), and a denoised Combined must never look like science data (d17).
bool write_pass_pfm(ar_session s, const char* name, const std::string& path, bool* denoised) {
  uint32_t count = 0;
  if (ar_session_pass_count(s, &count) != AR_OK) {
    printf("pass list unreadable: %s\n", session_error(s).c_str());
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    ar_pass_info info{};
    if (ar_session_pass_info(s, i, &info) != AR_OK) {
      printf("pass %u unreadable: %s\n", i, session_error(s).c_str());
      return false;
    }
    if (text(info.name, sizeof info.name) != name) continue;
    if (info.component_count != 3) {
      printf("pass %s has %u components, not 3\n", name, info.component_count);
      return false;
    }
    std::vector<float> px(size_t(info.width) * info.height * 3);
    if (ar_session_read_pass(s, i, AR_QUANTITY_MEAN, uint32_t(px.size()), px.data()) != AR_OK) {
      printf("reading %s failed: %s\n", name, session_error(s).c_str());
      return false;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
      printf("cannot write %s\n", path.c_str());
      return false;
    }
    fprintf(f, "PF\n%u %u\n-1.0\n", info.width, info.height);
    const bool ok = fwrite(px.data(), sizeof(float), px.size(), f) == px.size();
    if (fclose(f) != 0 || !ok) {
      printf("writing %s failed\n", path.c_str());
      return false;
    }
    *denoised = (info.flags & AR_PASS_DENOISED) != 0;
    return true;
  }
  printf("the session has no pass %s\n", name);
  return false;
}

}  // namespace

int parse_run_args(const char* cmd, int argc, char** argv, bool needs_out, bool is_bench, RunArgs& a) {
  auto usage = [&](const char* why, const char* arg) {
    fprintf(stderr, "%s: %s '%s'\n", cmd, why, arg);
    fprintf(stderr, is_bench ? "usage: astroray_cli bench <recording> [--passes N] [--warmup W] [--spp N] "
                               "[--device cpu|cuda[:i]] [--seed S] [--size WxH]\n"
                             : "usage: astroray_cli %s <recording> -o <file.fits> [--spp N] [--device cpu|cuda[:i]] "
                               "[--seed S] [--size WxH] [--denoise oidn|none [--dump]]\n",
            cmd);
    return kExitUsage;
  };
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      printf("usage: astroray_cli %s <recording> %s[--spp N] [--device cpu|cuda[:i]] [--seed S] [--size WxH]%s\n", cmd,
             needs_out ? "-o <file.fits> " : "",
             is_bench ? " [--passes N] [--warmup W]" : " [--denoise oidn|none [--dump]]");
      if (needs_out)
        printf("The file is the full film dump (ar_session_write_fits AR_FITS_DUMP), not a science file: the\n"
               "science file kind is not implemented yet, so render and dump write the same thing.\n"
               "--denoise sets the recorded final session's denoiser; Combined (denoised when the report has no\n"
               "DENOISER_UNAVAILABLE entry) is then also written as <file>_combined.pfm, and with --dump the\n"
               "film's Noisy Image as <file>_noisy_image.pfm. A denoised image is never written to FITS (0008 d17).\n");
      return 0;
    }
    const char* k = argv[i];
    const bool has_val = i + 1 < argc;
    if (!strcmp(k, "-o") && needs_out && has_val) {
      a.out = argv[++i];
    } else if (!strcmp(k, "--spp") && has_val) {
      if (!parse_u32(argv[++i], &a.overrides.samples) || a.overrides.samples == 0) return usage("bad sample count", argv[i]);
    } else if (!strcmp(k, "--seed") && has_val) {
      if (!parse_u32(argv[++i], &a.overrides.seed)) return usage("bad seed", argv[i]);
    } else if (!strcmp(k, "--size") && has_val) {
      const char* v = argv[++i];
      const char* x = strchr(v, 'x');
      const std::string w = x ? std::string(v, x) : std::string();
      if (!x || !parse_u32(w.c_str(), &a.overrides.width) || !parse_u32(x + 1, &a.overrides.height) ||
          a.overrides.width == 0 || a.overrides.height == 0)
        return usage("expected WxH, got", v);
    } else if (!strcmp(k, "--device") && has_val) {
      const char* v = argv[++i];
      if (!strcmp(v, "cpu")) {
        a.overrides.device_kind = AR_DEVICE_CPU;
        a.overrides.device_index = 0;
      } else if (!strncmp(v, "cuda", 4) && (v[4] == 0 || v[4] == ':')) {
        a.overrides.device_kind = AR_DEVICE_CUDA;
        a.overrides.device_index = 0;
        if (v[4] == ':' && !parse_u32(v + 5, &a.overrides.device_index)) return usage("bad device", v);
      } else {
        return usage("expected cpu or cuda[:i], got", v);
      }
    } else if (needs_out && !strcmp(k, "--denoise") && has_val) {
      const char* v = argv[++i];
      if (!strcmp(v, "oidn")) a.overrides.denoiser = AR_DENOISER_OIDN;
      else if (!strcmp(v, "none")) a.overrides.denoiser = AR_DENOISER_NONE;
      else return usage("expected oidn or none, got", v);
    } else if (needs_out && !strcmp(k, "--dump")) {
      a.dump_noisy = true;
    } else if (is_bench && !strcmp(k, "--passes") && has_val) {
      if (!parse_u32(argv[++i], &a.passes) || a.passes == 0) return usage("bad pass count", argv[i]);
    } else if (is_bench && !strcmp(k, "--warmup") && has_val) {
      if (!parse_u32(argv[++i], &a.warmup)) return usage("bad warmup", argv[i]);
    } else if (k[0] != '-' && a.recording.empty()) {
      a.recording = k;
    } else {
      return usage("unknown or incomplete argument", k);
    }
  }
  if (a.recording.empty()) return usage("missing recording", "");
  if (needs_out && a.out.empty()) return usage("missing -o", "");
  if (a.dump_noisy && a.overrides.denoiser != AR_DENOISER_OIDN) return usage("--dump needs", "--denoise oidn");
  return kContinue;
}

Replayed::~Replayed() {
  // Sessions first, then scenes, then the context: the replay ran with keep_alive.
  for (ar_session s : res.sessions) {
    ar_session_close(s);
    ar_session_destroy(s);
  }
  for (ar_scene s : res.scenes) ar_scene_destroy(s);
  if (res.context) ar_context_destroy(res.context);
}

int Replayed::open(const RunArgs& a) {
  ArReplayOptions opt = a.overrides;
  opt.log_path = "astroray_cli.log";  // the ABI requires a log path; it lands in the working directory
  opt.keep_alive = true;
  const ar_status st = ar_replay_file(a.recording.c_str(), opt, &res);
  if (res.size_refused) {
    printf("--size refused: %s\n", res.message.c_str());
    return kExitUsage;
  }
  if (st == AR_ERR_ABI_MISMATCH) {
    printf("ABI mismatch: %s\n", res.message.c_str());
    return kExitAbi;
  }
  if (st != AR_OK) {
    printf("replay failed: %s: %s (after %llu calls)\n", status_name(st), res.message.c_str(),
           static_cast<unsigned long long>(res.calls));
    return kExitFailure;
  }
  if (!res.matched) {
    printf("replay differs from the recording in %llu calls: %s\n", static_cast<unsigned long long>(res.mismatches),
           res.message.c_str());
    return kExitFailure;
  }
  // Ref: Ruling 71: an override the replay could not apply is an error; each applied one is printed.
  struct Check {
    const char* flag;
    bool requested;
    uint32_t bits;
    std::string value;
    bool user;
  };
  const ArReplayOptions& o = a.overrides;
  const Check checks[] = {
      {"--device", o.device_kind != kReplayUnset, kAppliedDevice,
       o.device_kind == AR_DEVICE_CUDA ? "cuda:" + std::to_string(o.device_index) : "cpu", true},
      {"--seed", o.seed != kReplayUnset, kAppliedSeed, std::to_string(o.seed), true},
      {"--size", o.width != kReplayUnset, kAppliedSize, std::to_string(o.width) + "x" + std::to_string(o.height), true},
      {"--spp", o.samples != kReplayUnset, kAppliedSamples, std::to_string(o.samples), !a.auto_samples},
      {"--denoise", o.denoiser != kReplayUnset, kAppliedDenoiser, o.denoiser == AR_DENOISER_OIDN ? "oidn" : "none", true}};
  for (const Check& c : checks) {
    if (!c.requested) continue;
    if ((res.applied & c.bits) != c.bits) {
      printf("override %s was not applied (the recording has no matching call)\n", c.flag);
      return kExitFailure;
    }
    if (c.user) printf("override applied: %s %s\n", c.flag, c.value.c_str());
  }
  if (res.sessions.empty()) {
    printf("the recording creates no session: nothing to render\n");
    return kExitFailure;
  }
  return kContinue;
}

bool settled_and_converged(const ar_progress& p) {
  return p.state == AR_STATE_CONVERGED &&
         (p.active_generation == p.pending_generation || p.failed_generation == p.pending_generation);
}

int run_sessions(Replayed& r, uint32_t timeout_ms) {
  using clock = std::chrono::steady_clock;
  const auto end = clock::now() + std::chrono::milliseconds(timeout_ms);
  for (size_t i = 0; i < r.res.sessions.size(); ++i) {
    ar_session s = r.res.sessions[i];
    ar_progress p{};
    if (ar_session_get_progress(s, &p) != AR_OK) { printf("session %zu: progress failed\n", i); return kExitFailure; }
    if (p.state == AR_STATE_INITIALIZING) {
      const ar_status st = ar_session_start(s);
      if (st != AR_OK) {
        uint8_t msg[256] = {};
        ar_session_last_error(s, sizeof msg, msg);
        printf("session %zu: start failed: %s: %s\n", i, status_name(st), text(msg, sizeof msg).c_str());
        return kExitFailure;
      }
    }
    // Converged means the sample target is reached; a generation committed late (pending ahead of
    // active and not rejected) restarts the render, so both must hold.
    for (;;) {
      ar_session_wait(s, 100);
      if (ar_session_get_progress(s, &p) != AR_OK) { printf("session %zu: progress failed\n", i); return kExitFailure; }
      // ar_session_wait returns at once on PAUSED, and nothing here resumes a session the recording
      // paused, so it would never converge: fail now rather than spin until the timeout.
      if (p.state == AR_STATE_FAILED || p.state == AR_STATE_CLOSED || p.state == AR_STATE_TRIMMED ||
          p.state == AR_STATE_PAUSED) {
        uint8_t msg[256] = {};
        ar_session_last_error(s, sizeof msg, msg);
        printf("session %zu ended in state %u: %s\n", i, p.state, text(msg, sizeof msg).c_str());
        return kExitFailure;
      }
      if (settled_and_converged(p)) break;
      if (clock::now() > end) {
        printf("session %zu: not converged after %u ms (state %u, %u spp)\n", i, timeout_ms, p.state,
               p.samples_per_pixel);
        return kExitFailure;
      }
      if (p.state == AR_STATE_CONVERGED) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    printf("session %zu: %u spp, generation %llu\n", i, p.samples_per_pixel,
           static_cast<unsigned long long>(p.active_generation));
  }
  return kContinue;
}

int print_reports(Replayed& r) {
  int highest = -1;
  // The report is printed with the build that produced it. The ABI has no call that writes to the
  // context log, so the table goes to stdout only.
  ar_build_record b{};
  if (ar_build_info(&b) == AR_OK)
    printf("report build: sha=%s cuda=%s\n", text(b.git_sha, sizeof b.git_sha).c_str(), text(b.cuda, sizeof b.cuda).c_str());
  for (size_t i = 0; i < r.res.sessions.size(); ++i) {
    ar_report_info info{};
    if (ar_session_report_info(r.res.sessions[i], &info) != AR_OK) {
      printf("session %zu: report unreadable\n", i);
      return AR_LEVEL_ERROR;
    }
    printf("report session %zu: revision %llu, %u entries, %u dropped\n", i,
           static_cast<unsigned long long>(info.revision), info.entry_count, info.dropped);
    if (info.entry_count) printf("level code subject count detail\n");
    for (uint32_t e = 0; e < info.entry_count; ++e) {
      ar_report_entry en{};
      if (ar_session_report_entry(r.res.sessions[i], e, &en) != AR_OK) {
        printf("session %zu: report entry %u unreadable\n", i, e);
        return AR_LEVEL_ERROR;
      }
      printf("%s %u %llu %llu %s\n", level_name(en.level), en.code, static_cast<unsigned long long>(en.subject),
             static_cast<unsigned long long>(en.count), text(en.detail, sizeof en.detail).c_str());
      if (static_cast<int>(en.level) > highest) highest = static_cast<int>(en.level);
    }
  }
  return highest;
}

int render_cmd(int argc, char** argv, bool dump) {
  const char* cmd = dump ? "dump" : "render";
  RunArgs a;
  if (int rc = parse_run_args(cmd, argc, argv, true, false, a); rc != kContinue) return rc;
  Replayed r;
  if (int rc = r.open(a); rc != kContinue) return rc;
  if (int rc = run_sessions(r, 600000); rc != kContinue) {
    print_reports(r);
    return rc;
  }
  // The ABI has one film file kind that is implemented: the full film dump (display mean,
  // variance and sample counts plus every other allocated channel). `render` writes it too.
  for (size_t i = 0; i < r.res.sessions.size(); ++i) {
    std::string path = a.out;
    if (r.res.sessions.size() > 1) {
      const size_t dot = path.rfind('.');
      const std::string tag = "_" + std::to_string(i);
      path = dot == std::string::npos ? path + tag : path.substr(0, dot) + tag + path.substr(dot);
    }
    const ar_status st = ar_session_write_fits(r.res.sessions[i], path.c_str(), AR_FITS_DUMP);
    if (st != AR_OK) {
      uint8_t msg[256] = {};
      ar_session_last_error(r.res.sessions[i], sizeof msg, msg);
      printf("%s: writing %s failed: %s: %s\n", cmd, path.c_str(), status_name(st), text(msg, sizeof msg).c_str());
      print_reports(r);
      return kExitFailure;
    }
    printf("wrote %s\n", path.c_str());
    if (a.overrides.denoiser != AR_DENOISER_OIDN) continue;
    struct Out {
      const char* pass;
      const char* suffix;
      bool wanted;
    };
    for (const Out& o : {Out{"Combined", "_combined.pfm", true}, Out{"Noisy Image", "_noisy_image.pfm", a.dump_noisy}}) {
      if (!o.wanted) continue;
      const std::string pfm = with_suffix(path, o.suffix);
      bool denoised = false;
      if (!write_pass_pfm(r.res.sessions[i], o.pass, pfm, &denoised)) {
        print_reports(r);
        return kExitFailure;
      }
      // A failed denoiser leaves Combined noisy with one DENOISER_UNAVAILABLE entry (0008 d16), printed below.
      printf("wrote %s (%s, %s)\n", pfm.c_str(), o.pass, denoised ? "denoised" : "not denoised");
    }
  }
  const int highest = print_reports(r);
  return highest >= AR_LEVEL_ERROR ? kExitFailure : 0;
}

}  // namespace cli

// SPDX-License-Identifier: Apache-2.0
// Shared by the render, dump and bench commands: argument parsing, the replay and its sessions.
// Public ABI and the generated replayer only (0001 D1.4).
#pragma once
#include <string>
#include <vector>

#include "astroray/astroray.h"
#include "replayer.h"

namespace cli {

// Exit codes: 1 an ERROR report entry or any failure, 2 a usage error, 3 an ABI mismatch (D2.11).
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;
constexpr int kExitAbi = 3;
constexpr int kContinue = -1;  // "no exit yet" from the helpers below

struct RunArgs {
  std::string recording;
  std::string out;           // render and dump only
  ArReplayOptions overrides;  // every field kReplayUnset unless given
  uint32_t passes = 16;       // bench only
  uint32_t warmup = 2;        // bench only
};

// Returns kContinue or the exit code. `needs_out` is true for render and dump.
int parse_run_args(const char* cmd, int argc, char** argv, bool needs_out, bool is_bench, RunArgs& a);

// The replayed recording: its context, scenes and sessions stay alive until destruction.
struct Replayed {
  ArReplayResult res;
  ~Replayed();
  // Replays with the overrides; returns kContinue or the exit code (3 for an ABI mismatch).
  int open(const RunArgs& a);
};

// Starts every session that has not started and waits until each is CONVERGED; returns
// kContinue or kExitFailure (a FAILED or CLOSED session, a timeout).
int run_sessions(Replayed& r, uint32_t timeout_ms);

// Prints each session's report table and returns the highest level seen (-1 when empty).
int print_reports(Replayed& r);

const char* status_name(ar_status s);
std::string text(const uint8_t* p, size_t cap);

int render_cmd(int argc, char** argv, bool dump);
int bench_cmd(int argc, char** argv);

}  // namespace cli

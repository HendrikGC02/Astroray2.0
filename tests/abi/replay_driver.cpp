// SPDX-License-Identifier: Apache-2.0
// Test-only: replays an .arrec through the generated replayer and the public ABI.
// Usage: replay_driver <file.arrec> <context log> [record path]
// Prints "status <n>", "calls <n>" and the message; exits with the status.
#include <stdio.h>

#include "replayer.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: replay_driver <file.arrec> <context log> [record path]\n");
    return 2;
  }
  ArReplayOptions opt;
  opt.log_path = argv[2];
  opt.record_path = argc > 3 ? argv[3] : nullptr;
  ArReplayResult res;
  ar_status st = ar_replay_file(argv[1], opt, &res);
  printf("status %u\ncalls %llu\nmatched %d\n%s\n", static_cast<unsigned>(st), static_cast<unsigned long long>(res.calls),
         res.matched ? 1 : 0, res.message.c_str());
  return static_cast<int>(st);
}

// SPDX-License-Identifier: Apache-2.0
// Self-test case rows for tools/kernelgate.py, same X-macro shape as src/kernel/launch_config.h.
#define AR_LAUNCH_ROWS(X) \
  X(queue_build_count, 256, 0, 0, OTHER)

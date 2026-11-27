// SPDX-License-Identifier: Apache-2.0
// Self-test case rows for tools/kernelgate.py, same X-macro shape as src/kernel/launch_config.h.
#define AR_LAUNCH_ROWS(X) \
  X(shade_surface, 256, 2, 1, VM)

// SPDX-License-Identifier: Apache-2.0
void f() {
  try {
    g();
  } catch (...) {
    // best-effort cleanup: nothing can be done here
  }
  try {
    g();
  } catch (...) {
    report();
  }
}

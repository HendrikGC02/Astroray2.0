// SPDX-License-Identifier: Apache-2.0
void f() {
  try {
    g();
  } catch (...) {
  }
  try {
    g();
  } catch (...) {  }
  try {
    g();
  } catch ( ... ) {

  }
  try {
    g();
  } catch (...) {
    // reason
  }
  try {
    g();
  } catch (const E&) {
  }
}

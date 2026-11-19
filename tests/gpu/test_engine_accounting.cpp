// SPDX-License-Identifier: Apache-2.0
// 0002 D13's ledger check on CUDA (task 126): after a scene is loaded and rendered, the drop in
// cuMemGetInfo free memory matches ar_progress's ledger total (session ledger plus the scene's device
// ledger, DEVICE_CONTEXT included) within 64 MiB, and ar_progress's device free and total bytes are
// filled. The render runs in a child (tests/abi/replay_driver.cpp --ledger), so the drop is measured
// against a primary context nothing else in this process has used. Gate: b1m; full: b5m (1.7 s of
// process alone, over the gate's 1 s for the new tests).
// Ref: docs/decisions/0002-kernel-architecture.md D13 (check row: within 64 MiB); 0007 d14 (ledger,
// device free/total bytes in ar_progress).
#include <doctest.h>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr long long kMiB = 1048576;
constexpr long long kTolerance = 64 * kMiB;  // 0002 D13
// ar_progress.ledger_bytes indices, the host ledger's category order (host/ledger.h).
constexpr size_t kGeometry = 12, kAcceleration = 13, kDeviceContext = 16, kCategories = 17;

struct LedgerLine {
  long long drop = 0, total = 0, progress_free = 0, progress_total = 0, after = 0;
  std::vector<long long> categories;
};

LedgerLine run_ledger_check(const char* scene) {
  char exe[MAX_PATH] = {};
  REQUIRE(GetModuleFileNameA(nullptr, exe, MAX_PATH) != 0);
  const std::filesystem::path bin = std::filesystem::path(exe).parent_path();
  const std::filesystem::path driver = bin / "replay_driver.exe";
  const std::filesystem::path rec = bin.parent_path() / "scenes" / (std::string(scene) + ".arrec");
  const std::filesystem::path log = std::filesystem::temp_directory_path() / "astroray_126_ledger.log";
  REQUIRE_MESSAGE(std::filesystem::exists(driver), driver.string() << " missing: run tools/build.ps1");
  REQUIRE_MESSAGE(std::filesystem::exists(rec), rec.string() << " missing: run tools/check.ps1 (scenes)");
  const std::string cmd = "\"\"" + driver.string() + "\" --ledger \"" + rec.string() + "\" \"" + log.string() +
                          "\" 1920 1080 2>&1\"";
  FILE* p = _popen(cmd.c_str(), "r");
  REQUIRE(p != nullptr);
  std::string out;
  char buf[1024];
  while (std::fgets(buf, sizeof buf, p)) out += buf;
  const int rc = _pclose(p);
  REQUIRE_MESSAGE(rc == 0, out);
  const size_t at = out.find("ledger drop=");
  REQUIRE_MESSAGE(at != std::string::npos, out);
  std::string line = out.substr(at, out.find('\n', at) - at);
  std::printf("%s: %s\n", scene, line.c_str());
  LedgerLine r;
  const auto field = [&line](const char* key) {
    const size_t k = line.find(key);
    REQUIRE_MESSAGE(k != std::string::npos, "no " << key << " in " << line);
    return std::stoll(line.substr(k + std::strlen(key)));
  };
  r.drop = field(" drop=");
  r.total = field(" total=");
  r.progress_free = field(" progress_free=");
  r.progress_total = field(" progress_total=");
  r.after = field(" after=");
  std::istringstream cats(line.substr(line.find("categories") + 10));
  long long v = 0;
  while (cats >> v) r.categories.push_back(v);
  REQUIRE(r.categories.size() == kCategories);
  return r;
}

void check_ledger(const char* scene) {
  const LedgerLine r = run_ledger_check(scene);
  std::printf("%s: free dropped %.1f MiB, ledger %.1f MiB (device context %.1f MiB), gap %.1f MiB\n", scene,
              double(r.drop) / kMiB, double(r.total) / kMiB, double(r.categories[kDeviceContext]) / kMiB,
              double(r.drop - r.total) / kMiB);
  CHECK_MESSAGE(std::llabs(r.drop - r.total) <= kTolerance, "0002 D13: ledger total and free-memory drop differ");
  // The scene's device memory and OptiX's own are in the ledger the session reports.
  CHECK(r.categories[kGeometry] > 0);
  CHECK(r.categories[kAcceleration] > 0);
  CHECK(r.categories[kDeviceContext] > 0);
  // ar_progress's device fields: cuMemGetInfo's, read moments apart from the child's own reading.
  CHECK(r.progress_total > 0);
  CHECK(r.progress_free > 0);
  CHECK(r.progress_free <= r.progress_total);
  CHECK(std::llabs(r.progress_free - r.after) <= kTolerance);
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("engine_accounting/b1m ledger total matches the free-memory drop") { check_ledger("b1m"); }

}  // TEST_SUITE

TEST_SUITE("full") {

TEST_CASE("engine_accounting/b5m ledger total matches the free-memory drop") { check_ledger("b5m"); }

}  // TEST_SUITE

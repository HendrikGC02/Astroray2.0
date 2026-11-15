// SPDX-License-Identifier: Apache-2.0
// 0001 V14, the CPU fault part: an access violation on an engine thread writes a
// minidump, poisons the context and leaves the process alive.
#include <doctest.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "astroray/astroray.h"
#include "host/fault.h"
#include "host/thread.h"

namespace fs = std::filesystem;
using namespace astroray;

#ifdef AR_GATE_HOOKS
TEST_SUITE("gate") {

TEST_CASE("fault/access violation poisons context") {
  // A per-process directory (worktrees run in parallel), emptied first, so one run leaves
  // exactly one dump to inspect.
  const fs::path dir = fs::temp_directory_path() / ("astroray_fault_test_" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(dir);
  fs::create_directories(dir);

  ContextState ctx;
  ctx.log_path = (dir / "ctx.log").string();
  std::string err;
  REQUIRE(ctx.log.open(ctx.log_path, &err));
  ctx.log.line("INFO", "before the fault");
  CHECK_FALSE(ctx.poisoned.load());

  fault_test_access_violation(ctx);  // returning at all means the process is alive

  REQUIRE(ctx.poisoned.load());
  std::vector<fs::path> dumps;
  for (const auto& e : fs::directory_iterator(dir))
    if (e.path().extension() == ".dmp") dumps.push_back(e.path());
  REQUIRE(dumps.size() == 1);
  CHECK(fs::file_size(dumps[0]) > 0);
  // <log_path>.<pid>.<n>.dmp with this process's id and the first dump's n = 0.
  CHECK(dumps[0].string() == ctx.log_path + "." + std::to_string(GetCurrentProcessId()) + ".0.dmp");

  const std::string expect =
      "Engine fault, dump at " + dumps[0].string() + ". Save to a new file and restart Blender.";
  CHECK(ctx.poison_message == expect);

  std::ifstream in(ctx.log_path);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string log = ss.str();
  CHECK(log.find("before the fault") != std::string::npos);
  CHECK(log.find("engine fault, exception code 0xC0000005") != std::string::npos);
  CHECK(log.find(dumps[0].string()) != std::string::npos);
}

// Only last_error, destroy and create take a context today; last_error is the one call
// that can show the poisoned state. Add other ctx-taking calls here as they appear.
TEST_CASE("fault/C API last_error returns the fault message on a poisoned context") {
  const fs::path dir = fs::temp_directory_path() / ("astroray_fault_capi_test_" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string log_path = (dir / "ctx.log").string();

  ar_context_desc desc{};
  desc.device_kind = AR_DEVICE_CPU;
  desc.log_path = log_path.c_str();
  ar_context ctx = nullptr;
  REQUIRE(ar_context_create(&desc, &ctx) == AR_OK);

  ContextState& state = fault_test_context_state(ctx);
  fault_test_access_violation(state);
  REQUIRE(state.poisoned.load());

  uint8_t text[512] = {};
  CHECK(ar_context_last_error(ctx, sizeof text, text) == AR_OK);
  const std::string expect = "Engine fault, dump at " + log_path + "." + std::to_string(GetCurrentProcessId()) +
                             ".0.dmp. Save to a new file and restart Blender.";
  CHECK(std::string(reinterpret_cast<char*>(text)) == expect);
  CHECK(ar_context_destroy(ctx) == AR_OK);
}

TEST_CASE("fault/thread without a fault leaves the context clean") {
  ContextState ctx;
  bool ran = false;
  std::thread t = start_engine_thread(ctx, [&ran] { ran = true; });
  t.join();
  CHECK(ran);
  CHECK_FALSE(ctx.poisoned.load());
  CHECK(ctx.poison_message.empty());
}

}
#endif

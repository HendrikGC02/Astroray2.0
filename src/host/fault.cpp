// SPDX-License-Identifier: Apache-2.0
#include "host/fault.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <stdio.h>

#include "host/thread.h"

namespace astroray {

namespace {

std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

// Ref: https://learn.microsoft.com/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump
// MiniDumpNormal plus MiniDumpWithThreadInfo; exception pointers are in this process.
bool write_dump(const std::string& path, EXCEPTION_POINTERS* info) {
  HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  MINIDUMP_EXCEPTION_INFORMATION mei;
  mei.ThreadId = GetCurrentThreadId();
  mei.ExceptionPointers = info;
  mei.ClientPointers = FALSE;
  BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), &mei, nullptr, nullptr);
  CloseHandle(f);
  return ok != FALSE;
}

// Ref: https://learn.microsoft.com/cpp/cpp/try-except-statement (filter expression
// with GetExceptionInformation); returns EXCEPTION_EXECUTE_HANDLER for a fault.
LONG fault_filter(ContextState& ctx, EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  // Every exception that reaches the top of an engine thread is a fault, an uncaught
  // C++ exception included; a debugger still sees breakpoints first.
  char n[32];
  snprintf(n, sizeof n, ".%lu.%u.dmp", GetCurrentProcessId(), ctx.dump_count.fetch_add(1));
  const std::string path = ctx.log_path + n;
  const bool dumped = write_dump(path, info);

  char codebuf[16];
  snprintf(codebuf, sizeof codebuf, "0x%08lX", code);
  ctx.log.line("ERROR", std::string("engine fault, exception code ") + codebuf + ", " +
                            (dumped ? "dump at " + path : "dump could not be written at " + path));
  // The first fault names the dump; later faults on other threads keep that message.
  if (!ctx.fault_claimed.exchange(true)) {
    ctx.poison_message = "Engine fault, dump at " + path + ". Save to a new file and restart Blender.";
    ctx.poisoned.store(true, std::memory_order_release);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

// No C++ objects with destructors live in this frame: MSVC rejects __try there.
bool run_engine_thread_raw(ContextState& ctx, void (*fn)(void*), void* arg) {
  __try {
    fn(arg);
    return true;
  } __except (fault_filter(ctx, GetExceptionInformation())) {
    return false;
  }
}

#ifdef AR_GATE_HOOKS
void fault_test_access_violation(ContextState& ctx) {
  std::thread t = start_engine_thread(ctx, [] {
    volatile int* p = nullptr;
    *p = 1;
  });
  t.join();
}
#endif

}  // namespace astroray

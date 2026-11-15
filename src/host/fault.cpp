// SPDX-License-Identifier: Apache-2.0
#include "host/fault.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <stdio.h>
#include <wchar.h>

#include "host/thread.h"

namespace astroray {

namespace {

// Ref: https://learn.microsoft.com/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump
// (remarks: call it from a separate process). MiniDumpWriteDump on this process suspends
// the other threads and takes loader and heap locks; under load that hung, so a helper
// process beside the module writes the dump. Everything here uses stack buffers: no heap
// and no CRT lock before the helper runs. Returns "" on success, else the reason.
constexpr DWORD kHelperTimeoutMs = 30000;  // PROVISIONAL

const char* write_dump(const std::string& path, EXCEPTION_POINTERS* info, char* reason, size_t reason_size) {
  wchar_t helper[MAX_PATH + 32];
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&write_dump), &self))
    return "helper missing";
  const DWORD n = GetModuleFileNameW(self, helper, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return "helper missing";
  wchar_t* slash = wcsrchr(helper, L'\\');
  if (!slash) return "helper missing";
  wcscpy_s(slash + 1, 32, L"astroray_dump.exe");

  wchar_t wpath[2 * MAX_PATH];
  if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath, 2 * MAX_PATH) <= 0) {
    snprintf(reason, reason_size, "path too long");
    return reason;
  }
  wchar_t cmd[4 * MAX_PATH];
  swprintf(cmd, 4 * MAX_PATH, L"\"%ls\" %lu %lu %llu \"%ls\"", helper, GetCurrentProcessId(), GetCurrentThreadId(),
           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info)), wpath);

  STARTUPINFOW si = {};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(helper, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return "helper missing";
  const DWORD w = WaitForSingleObject(pi.hProcess, kHelperTimeoutMs);
  const char* result = "";
  if (w != WAIT_OBJECT_0) {
    TerminateProcess(pi.hProcess, 1);
    result = "helper timeout";
  } else {
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (code != 0) {
      snprintf(reason, reason_size, "helper exit %lu", code);
      result = reason;
    }
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return result;
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
  char reason_buf[48];
  const std::string reason = write_dump(path, info, reason_buf, sizeof reason_buf);
  const bool dumped = reason.empty();

  char codebuf[16];
  snprintf(codebuf, sizeof codebuf, "0x%08lX", code);
  ctx.log.line("ERROR", std::string("engine fault, exception code ") + codebuf + ", " +
                            (dumped ? "dump at " + path : "dump could not be written at " + path + " (" + reason + ")"));
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

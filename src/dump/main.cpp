// SPDX-License-Identifier: Apache-2.0
// Out-of-process minidump writer: astroray_dump <pid> <thread id> <EXCEPTION_POINTERS address> <output path>
// MiniDumpWriteDump on the faulting process itself can deadlock on the loader and heap
// locks it takes after suspending threads, so the engine's fault filter runs this instead.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t** argv) {
  if (argc != 5) {
    fputs("usage: astroray_dump <pid> <thread id> <exception pointers address> <output path>\n", stderr);
    return 2;
  }
  const DWORD pid = static_cast<DWORD>(wcstoul(argv[1], nullptr, 10));
  const DWORD tid = static_cast<DWORD>(wcstoul(argv[2], nullptr, 10));
  const ULONGLONG addr = _wcstoui64(argv[3], nullptr, 10);

  HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!proc) {
    fprintf(stderr, "astroray_dump: OpenProcess(%lu) failed, error %lu\n", pid, GetLastError());
    return 3;
  }
  HANDLE f = CreateFileW(argv[4], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "astroray_dump: cannot create the dump file, error %lu\n", GetLastError());
    return 4;
  }
  MINIDUMP_EXCEPTION_INFORMATION mei;
  mei.ThreadId = tid;
  mei.ExceptionPointers = reinterpret_cast<PEXCEPTION_POINTERS>(static_cast<ULONG_PTR>(addr));
  mei.ClientPointers = TRUE;  // the pointers live in the target process
  // Ref: https://learn.microsoft.com/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump
  const BOOL ok = MiniDumpWriteDump(proc, pid, f, static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),
                                    &mei, nullptr, nullptr);
  const DWORD err = GetLastError();
  CloseHandle(f);
  CloseHandle(proc);
  if (!ok) {
    fprintf(stderr, "astroray_dump: MiniDumpWriteDump failed, HRESULT 0x%08lX\n", err);
    return 5;
  }
  return 0;
}

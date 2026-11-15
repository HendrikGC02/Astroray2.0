// SPDX-License-Identifier: Apache-2.0
#include "host/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>

namespace astroray {

namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

}  // namespace

ContextLog::~ContextLog() {
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

bool ContextLog::open(const std::string& path, std::string* error) {
  std::wstring w = widen(path);
  if (w.empty()) {
    *error = "log path is empty or not valid UTF-8";
    return false;
  }
  // FILE_APPEND_DATA without FILE_WRITE_DATA: the OS appends atomically at end of file.
  HANDLE h = CreateFileW(w.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *error = "cannot open log file " + path + " (Windows error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  handle_ = h;
  return true;
}

bool ContextLog::line(const char* level, const std::string& text) {
  if (!handle_) return false;
  std::string t = text;
  for (char& c : t)
    if (c == '\n' || c == '\r') c = ' ';
  std::string l = utc_iso_now() + " " + level + " " + t + "\n";
  DWORD written = 0;
  return WriteFile(static_cast<HANDLE>(handle_), l.data(), static_cast<DWORD>(l.size()), &written, nullptr) &&
         written == l.size();
}

std::string utc_iso_now() {
  SYSTEMTIME st;
  GetSystemTime(&st);
  char buf[32];
  snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay, st.wHour,
           st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

}  // namespace astroray

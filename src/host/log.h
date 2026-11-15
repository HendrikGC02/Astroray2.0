// SPDX-License-Identifier: Apache-2.0
// The per-context log file (0001 D2.5): append-only, one WriteFile per line so
// every line reaches the OS as written and survives a process death.
#pragma once
#include <string>

namespace astroray {

class ContextLog {
 public:
  ContextLog() = default;
  ~ContextLog();
  ContextLog(const ContextLog&) = delete;
  ContextLog& operator=(const ContextLog&) = delete;

  // Opens path (UTF-8) for append, creating it. Returns false and fills error when it cannot.
  bool open(const std::string& path, std::string* error);
  // Writes "<UTC ISO time> <level> <text>\n" with a single WriteFile. Text is one
  // line: embedded newlines are replaced by spaces.
  bool line(const char* level, const std::string& text);

 private:
  void* handle_ = nullptr;  // HANDLE; kept opaque so no header pulls in windows.h
};

// "2026-11-15T12:34:56.789Z" for the current UTC time.
std::string utc_iso_now();

}  // namespace astroray

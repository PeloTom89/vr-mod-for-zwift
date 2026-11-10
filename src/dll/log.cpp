#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

static std::mutex g_logMutex;
static FILE* g_log = nullptr;

void LogInit() {
  wchar_t dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
  if (!n || n >= MAX_PATH) return;
  wcscat_s(dir, L"\\zwift-vr");
  CreateDirectoryW(dir, nullptr);
  wcscat_s(dir, L"\\zwift-vr.log");
  std::lock_guard lock(g_logMutex);
  // _wfopen_s would lock the file; share it so the log can be read while Zwift runs.
  g_log = _wfsopen(dir, L"w", _SH_DENYNO);
}

void Log(const char* fmt, ...) {
  std::lock_guard lock(g_logMutex);
  if (!g_log) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
          GetCurrentThreadId());
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

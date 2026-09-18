#include "common/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace kkvr {
namespace {

FILE* g_file = nullptr;
FILE* g_tee = nullptr;
std::mutex g_mutex;
LARGE_INTEGER g_freq{};
LARGE_INTEGER g_start{};

}  // namespace

// Beside the DLL (i.e. the game directory), not the CWD, which the game is
// free to change.
std::string ModuleDir() {
  char path[MAX_PATH] = {};
  HMODULE self = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCSTR>(&ModuleDir), &self);
  GetModuleFileNameA(self, path, MAX_PATH);
  std::string s(path);
  const size_t slash = s.find_last_of('\\');
  if (slash != std::string::npos) s.resize(slash + 1);
  return s;
}

void LogInit(const char* file_name, const char* title) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) return;
  QueryPerformanceFrequency(&g_freq);
  QueryPerformanceCounter(&g_start);
  g_file = fopen((ModuleDir() + file_name).c_str(), "w");
  if (!g_file) return;
  fprintf(g_file, "=== King Kong VR :: %s ===\n", title);
#ifndef KKVR_VERSION
#define KKVR_VERSION "dev"
#endif
  fprintf(g_file, "version " KKVR_VERSION ", built " __DATE__ " " __TIME__ "\n\n");
  fflush(g_file);
}

void LogShutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_file) return;
  fprintf(g_file, "\n=== shutdown ===\n");
  fclose(g_file);
  g_file = nullptr;
}

void Logf(const char* fmt, ...) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_file) return;

  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const double t = g_freq.QuadPart
                       ? double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart)
                       : 0.0;
  fprintf(g_file, "[%8.3f] ", t);

  va_list args;
  va_start(args, fmt);
  if (g_tee) {
    va_list copy;
    va_copy(copy, args);
    fprintf(g_tee, "[%8.3f] ", t);
    vfprintf(g_tee, fmt, copy);
    fputc('\n', g_tee);
    va_end(copy);
  }
  vfprintf(g_file, fmt, args);
  va_end(args);

  fputc('\n', g_file);
  fflush(g_file);  // we expect to crash sometimes; never buffer
}

void LogTeeBegin(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_tee) fclose(g_tee);
  g_tee = fopen(path.c_str(), "w");
}

void LogTeeEnd() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_tee) fclose(g_tee);
  g_tee = nullptr;
}

}  // namespace kkvr

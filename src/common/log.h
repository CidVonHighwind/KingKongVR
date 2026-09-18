// Minimal file logger, writing beside the DLL (the game directory).
#pragma once

#include <string>

namespace kkvr {

// Directory of this DLL, with a trailing backslash.
std::string ModuleDir();

// Opens the log file (beside the DLL) and writes its header.
void LogInit(const char* file_name, const char* title);
void LogShutdown();
void Logf(const char* fmt, ...);

// While a tee is open, every log line is also written to `path` (a frame
// capture's text file). Opening a new tee closes the previous one.
void LogTeeBegin(const std::string& path);
void LogTeeEnd();

}  // namespace kkvr

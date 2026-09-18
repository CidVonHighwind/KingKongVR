// Crash diagnostics: a vectored exception handler that writes the faulting
// address and a stack trace (module + offset) to kkvr.log. Offsets inside
// d3d9.dll (ours) can be looked up in build/Release/d3d9.map.
//
// It only observes: the exception continues to the game's own handlers.
// A vectored handler runs first, so an exception the game or a driver would
// handle itself is reported too (none has been seen in the logs so far). An
// unhandled-exception filter would see only real crashes, but the game
// imports SetUnhandledExceptionFilter and could replace it, losing the trace.

#include "common/crash.h"

#include "common/log.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstring>

namespace kkvr {
namespace {

LONG g_logged = 0;

bool IsFatal(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      return true;
    default:
      return false;
  }
}

// "name.dll+0x1234" for an address, or the raw address if not in a module.
void Describe(DWORD64 address, char* out, size_t size) {
  HMODULE module = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(address)),
                         &module) &&
      GetModuleFileNameA(module, path, MAX_PATH)) {
    const char* name = strrchr(path, '\\');
    wsprintfA(out, "%s+0x%X", name ? name + 1 : path,
              static_cast<DWORD>(address - reinterpret_cast<ULONG_PTR>(module)));
  } else {
    wsprintfA(out, "0x%08X", static_cast<DWORD>(address));
  }
  (void)size;
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  if (!IsFatal(record->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
  // A few reports are plenty; a crash loop must not flood the log.
  if (InterlockedIncrement(&g_logged) > 3) return EXCEPTION_CONTINUE_SEARCH;

  char where[MAX_PATH + 32];
  Describe(reinterpret_cast<ULONG_PTR>(record->ExceptionAddress), where,
           sizeof(where));
  Logf("!!! exception 0x%08X at %s, thread %u", record->ExceptionCode, where,
       GetCurrentThreadId());
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2) {
    Logf("    %s address 0x%08X",
         record->ExceptionInformation[0] == 0   ? "reading"
         : record->ExceptionInformation[0] == 1 ? "writing"
                                                : "executing",
         static_cast<DWORD>(record->ExceptionInformation[1]));
  }

  const HANDLE process = GetCurrentProcess();
  const HANDLE thread = GetCurrentThread();
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
  SymInitialize(process, nullptr, TRUE);
  CONTEXT context = *info->ContextRecord;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = context.Eip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Ebp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Esp;
  frame.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 24; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_I386, process, thread, &frame, &context,
                     nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                     nullptr) ||
        frame.AddrPC.Offset == 0) {
      break;
    }
    Describe(frame.AddrPC.Offset, where, sizeof(where));
    Logf("    #%-2d %s", i, where);
  }
  SymCleanup(process);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashHandler() {
  AddVectoredExceptionHandler(1, OnException);
}

}  // namespace kkvr

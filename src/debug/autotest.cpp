#include "debug/autotest.h"

#include "common/config.h"
#include "common/log.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace kkvr {
namespace {

struct Event {
  double time = 0.0;      // seconds
  enum Kind { kPress, kStick, kShot, kHotkey, kLog, kCapture, kQuit, kWait } kind = kLog;
  // kWait: tap `control` (or nothing if -1) every `duration` s until
  // `condition` held for 2 s, at most `timeout` s.
  enum Condition { kGameplay, kMenu } condition = kGameplay;
  double timeout = 60.0;
  int control = -1;
  double duration = 0.15;
  bool right = false;     // stick
  float x = 0.0f, y = 0.0f;
  std::string text;
  int vk = 0;
  bool shift = false;
};

struct Held {
  int control = -1;       // or -2 left stick, -3 right stick
  double until = 0.0;     // real time (Now()): a wait's pause must not extend a press
  float x = 0.0f, y = 0.0f;
};

std::mutex g_mutex;
bool g_loaded = false;
bool g_active = false;
std::vector<Event> g_events;
size_t g_next = 0;
// Blocking waits: the script clock excludes time spent waiting.
double g_paused_total = 0.0;
bool g_waiting = false;
double g_wait_start = 0.0;
double g_condition_since = -1.0;
double g_next_tap = 0.0;
std::vector<Held> g_held;
LARGE_INTEGER g_start{};

double Now() {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  if (g_start.QuadPart == 0) g_start = now;
  return static_cast<double>(now.QuadPart - g_start.QuadPart) /
         static_cast<double>(freq.QuadPart);
}

int ControlIndex(const char* name) {
  for (int i = 0; i < kPadControlCount; ++i) {
    if (_stricmp(name, kPadControlNames[i]) == 0) return i;
  }
  return -1;
}

int HotkeyCode(const char* name) {
  if ((name[0] == 'F' || name[0] == 'f') && name[1]) {
    const int n = atoi(name + 1);
    if (n >= 1 && n <= 24) return VK_F1 + n - 1;
  }
  return 0;
}

void Parse(FILE* file) {
  char line[512];
  int number = 0;
  while (fgets(line, sizeof(line), file)) {
    ++number;
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '@') continue;  // comments and blank lines
    Event e;
    char command[32] = {};
    int consumed = 0;
    if (sscanf(p + 1, "%lf %31s %n", &e.time, command, &consumed) < 2) {
      Logf("autotest: line %d not understood", number);
      continue;
    }
    char* args = p + 1 + consumed;
    args[strcspn(args, "\r\n")] = '\0';
    char a1[64] = {}, a2[64] = {};
    double n1 = 0, n2 = 0, n3 = 0;
    if (!_stricmp(command, "press")) {
      e.kind = Event::kPress;
      int got = sscanf(args, "%63s %lf", a1, &n1);
      e.control = ControlIndex(a1);
      if (got >= 2) e.duration = n1 / 1000.0;
      if (e.control < 0) {
        Logf("autotest: line %d unknown control '%s'", number, a1);
        continue;
      }
    } else if (!_stricmp(command, "stick")) {
      e.kind = Event::kStick;
      if (sscanf(args, "%63s %lf %lf %lf", a1, &n1, &n2, &n3) < 4) {
        Logf("autotest: line %d needs: stick left|right x y ms", number);
        continue;
      }
      e.right = !_stricmp(a1, "right");
      e.x = static_cast<float>(n1);
      e.y = static_cast<float>(n2);
      e.duration = n3 / 1000.0;
    } else if (!_stricmp(command, "shot")) {
      e.kind = Event::kShot;
      sscanf(args, "%63s", a1);
      e.text = a1[0] ? a1 : "shot";
    } else if (!_stricmp(command, "hotkey")) {
      e.kind = Event::kHotkey;
      sscanf(args, "%63s %63s", a1, a2);
      e.vk = HotkeyCode(a1);
      e.shift = !_stricmp(a2, "shift");
      if (!e.vk) {
        Logf("autotest: line %d unknown hotkey '%s'", number, a1);
        continue;
      }
    } else if (!_stricmp(command, "log")) {
      e.kind = Event::kLog;
      e.text = args;
    } else if (!_stricmp(command, "press_until") || !_stricmp(command, "wait")) {
      e.kind = Event::kWait;
      const bool press = !_stricmp(command, "press_until");
      char cond[32] = {};
      int got = 0;
      if (press) {
        got = sscanf(args, "%63s %lf %31s %lf", a1, &n1, cond, &n2);
        e.control = ControlIndex(a1);
        e.duration = n1;
        if (got < 3 || e.control < 0 || n1 <= 0.0) {
          Logf("autotest: line %d needs: press_until <control> <interval s> <condition> [timeout s]", number);
          continue;
        }
        if (got >= 4) e.timeout = n2;
      } else {
        got = sscanf(args, "%31s %lf", cond, &n2);
        e.control = -1;
        if (got < 1) {
          Logf("autotest: line %d needs: wait <condition> [timeout s]", number);
          continue;
        }
        if (got >= 2) e.timeout = n2;
      }
      if (!_stricmp(cond, "gameplay")) {
        e.condition = Event::kGameplay;
      } else if (!_stricmp(cond, "menu")) {
        e.condition = Event::kMenu;
      } else {
        Logf("autotest: line %d unknown condition '%s' (gameplay, menu)", number, cond);
        continue;
      }
    } else if (!_stricmp(command, "capture")) {
      e.kind = Event::kCapture;
    } else if (!_stricmp(command, "quit")) {
      e.kind = Event::kQuit;
    } else {
      Logf("autotest: line %d unknown command '%s'", number, command);
      continue;
    }
    g_events.push_back(e);
  }
}

}  // namespace

void AutotestLoad() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_loaded) return;
  g_loaded = true;
  const std::string& script = GetConfig().test_script;
  if (script.empty()) return;
  const std::string path = ModuleDir() + script;
  FILE* file = fopen(path.c_str(), "r");
  if (!file) {
    Logf("autotest: script %s not found", path.c_str());
    return;
  }
  Parse(file);
  fclose(file);
  // Keep the events in time order.
  for (size_t i = 1; i < g_events.size(); ++i) {
    for (size_t j = i; j > 0 && g_events[j].time < g_events[j - 1].time; --j) {
      std::swap(g_events[j], g_events[j - 1]);
    }
  }
  g_active = !g_events.empty();
  Logf("autotest: %zu events from %s", g_events.size(), path.c_str());
}

namespace {
WNDPROC g_game_wndproc = nullptr;

LRESULT CALLBACK ActiveWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ACTIVATEAPP:
      wp = TRUE;
      break;
    case WM_ACTIVATE:
      wp = MAKEWPARAM(WA_ACTIVE, HIWORD(wp));
      break;
    case WM_KILLFOCUS:
    case WM_NCACTIVATE:
      if (msg == WM_KILLFOCUS) return 0;
      wp = TRUE;
      break;
  }
  return CallWindowProcA(g_game_wndproc, hwnd, msg, wp, lp);
}
}  // namespace

void AutotestKeepWindowActive(HWND window) {
  if (!g_active || !window || g_game_wndproc) return;
  // [test] keep_active=0: leave the window's focus handling untouched, to run
  // exactly like a normal session (input then only reaches the game while
  // its window really is in the foreground).
  if (!GetPrivateProfileIntA("test", "keep_active", 1, (ModuleDir() + "kkvr.ini").c_str())) {
    Logf("autotest: keep_active=0, window focus left to Windows");
    return;
  }
  g_game_wndproc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ActiveWndProc)));
  Logf("autotest: game window %p kept active for scripted input", static_cast<void*>(window));
}

bool AutotestActive() {
  return g_active;
}

bool ProcessOwnsForeground() {
  const HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

std::vector<AutotestAction> AutotestTick(const AutotestFrame& frame) {
  std::vector<AutotestAction> actions;
  if (!g_active) return actions;
  std::lock_guard<std::mutex> lock(g_mutex);
  const double real = Now();
  if (g_waiting) {
    const Event& e = g_events[g_next - 1];
    const bool met = e.condition == Event::kGameplay
                         ? frame.render_target_switches >= 8
                         : frame.draws >= 300 && frame.render_target_switches < 8;
    if (!met) {
      g_condition_since = -1.0;
    } else if (g_condition_since < 0.0) {
      g_condition_since = real;
    }
    const bool done = g_condition_since >= 0.0 && real - g_condition_since >= 2.0;
    const bool timed_out = real - g_wait_start >= e.timeout;
    if (!done && !timed_out) {
      if (e.control >= 0 && real >= g_next_tap) {
        g_held.push_back({e.control, real + 0.12});
        g_next_tap = real + e.duration;
      }
    } else {
      g_paused_total += real - g_wait_start;
      g_waiting = false;
      Logf("autotest @%.2f: wait %s after %.1f s", real,
           done ? "done" : "TIMED OUT", real - g_wait_start);
    }
  }
  const double now = real - g_paused_total;
  while (!g_waiting && g_next < g_events.size() && g_events[g_next].time <= now) {
    const Event& e = g_events[g_next++];
    switch (e.kind) {
      case Event::kPress:
        g_held.push_back({e.control, real + e.duration});
        // Games often ignore input while their window is in the background.
        Logf("autotest @%.2f: press %s (game window %s)", now, kPadControlNames[e.control],
             ProcessOwnsForeground() ? "in foreground" : "NOT in foreground");
        break;
      case Event::kStick:
        g_held.push_back({e.right ? -3 : -2, real + e.duration, e.x, e.y});
        Logf("autotest @%.2f: %s stick %.2f %.2f", now, e.right ? "right" : "left",
             e.x, e.y);
        break;
      case Event::kShot:
        actions.push_back({AutotestAction::kScreenshot, e.text});
        break;
      case Event::kHotkey:
        actions.push_back({AutotestAction::kHotkey, "", e.vk, e.shift});
        break;
      case Event::kLog:
        Logf("autotest @%.2f: %s", now, e.text.c_str());
        break;
      case Event::kCapture:
        actions.push_back({AutotestAction::kCapture});
        break;
      case Event::kWait:
        g_waiting = true;
        g_wait_start = real;
        g_condition_since = -1.0;
        g_next_tap = real;
        Logf("autotest @%.2f: waiting for %s%s", real,
             e.condition == Event::kGameplay ? "gameplay" : "menu",
             e.control >= 0 ? " (tapping)" : "");
        break;
      case Event::kQuit:
        actions.push_back({AutotestAction::kQuit});
        break;
    }
  }
  for (size_t i = 0; i < g_held.size();) {
    if (g_held[i].until <= real) {
      g_held.erase(g_held.begin() + i);
    } else {
      ++i;
    }
  }
  return actions;
}

AutotestInput AutotestCurrentInput() {
  AutotestInput input;
  if (!g_active) return input;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const Held& h : g_held) {
    if (h.control >= 0) {
      input.controls |= 1u << h.control;
    } else if (h.control == -2) {
      input.left_stick = true;
      input.lx = h.x;
      input.ly = h.y;
    } else {
      input.right_stick = true;
      input.rx = h.x;
      input.ry = h.y;
    }
  }
  return input;
}

}  // namespace kkvr

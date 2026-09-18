#include "common/config.h"

#include "common/log.h"

#include <windows.h>

#include <cfloat>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace kkvr {
namespace {

constexpr char kDefaultIni[] =
    "; King Kong VR settings. Delete this file to restore the defaults.\r\n"
    "; In the headset: F8 or the Xbox button recentres the window,\r\n"
    "; F3 / Shift+F3 makes it bigger / smaller, F4 / Shift+F4 closer / farther\r\n"
    "; (saved here). More settings: README.md.\r\n"
    "\r\n"
    "[display]\r\n"
    "; frames per second: your headset's refresh rate (Quest 3: 120), or half\r\n"
    "; of it if your PC cannot keep up\r\n"
    "fps_limit=120\r\n"
    "\r\n"
    "[vr]\r\n"
    "; the window into the game world, in metres\r\n"
    "screen_width=1.2\r\n"
    "screen_distance=0.6\r\n"
    "; how much of the window cutscenes fill (0.1 to 1)\r\n"
    "cinematic_scale=0.6\r\n"
    "\r\n"
    "[render]\r\n"
    "; sharpness: 1.0 = one rendered pixel per headset pixel. Lower it if the\r\n"
    "; game stutters.\r\n"
    "render_scale=1.0\r\n";

std::string IniPath() { return ModuleDir() + "kkvr.ini"; }

// Readers for one key of kkvr.ini. `fallback` is the Config default (config.h
// holds every default); numbers are clamped to [lo, hi].
class Ini {
 public:
  explicit Ini(const std::string& path) : path_(path) {}

  std::string String(const char* section, const char* key, const char* fallback) const {
    char buf[MAX_PATH] = {};
    GetPrivateProfileStringA(section, key, fallback, buf, sizeof(buf), path_.c_str());
    return buf;
  }
  bool Bool(const char* section, const char* key, bool fallback) const {
    return GetPrivateProfileIntA(section, key, fallback, path_.c_str()) != 0;
  }
  int Int(const char* section, const char* key, int fallback, int lo = INT_MIN,
          int hi = INT_MAX) const {
    const int v = static_cast<int>(GetPrivateProfileIntA(section, key, fallback, path_.c_str()));
    return v < lo ? lo : v > hi ? hi : v;
  }
  float Float(const char* section, const char* key, float fallback, float lo = -FLT_MAX,
              float hi = FLT_MAX) const {
    const std::string text = String(section, key, "");
    const float v = text.empty() ? fallback : static_cast<float>(atof(text.c_str()));
    return v < lo ? lo : v > hi ? hi : v;
  }

 private:
  std::string path_;
};

Config Load() {
  const std::string path = IniPath();
  if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(f, kDefaultIni, sizeof(kDefaultIni) - 1, &written, nullptr);
      CloseHandle(f);
    }
  }

  Config c;
  const Ini ini(path);
  c.windowed = ini.Bool("display", "windowed", c.windowed);
  c.borderless = ini.Bool("display", "borderless", c.borderless);
  c.window_width = ini.Int("display", "window_width", c.window_width, 320);
  c.window_height = ini.Int("display", "window_height", c.window_height, 200);
  c.fps_limit = ini.Int("display", "fps_limit", c.fps_limit, 0);
  c.fix_frame_step = ini.Bool("display", "fix_frame_step", c.fix_frame_step);
  const std::string aspect = ini.String("display", "aspect_fix", "crop");
  c.aspect_fix = _stricmp(aspect.c_str(), "off") == 0      ? kAspectFixOff
                 : _stricmp(aspect.c_str(), "expand") == 0 ? kAspectFixExpand
                                                           : kAspectFixCrop;
  c.frame_times = ini.Bool("display", "frame_times", c.frame_times);
  c.preview_zoom = ini.Float("display", "preview_zoom", c.preview_zoom, 1.0f, 16.0f);
  c.preview_zoom_x = ini.Float("display", "preview_zoom_x", c.preview_zoom_x);
  c.preview_zoom_y = ini.Float("display", "preview_zoom_y", c.preview_zoom_y);

  c.vr_enabled = ini.Bool("vr", "enabled", c.vr_enabled);
  c.screen_distance = ini.Float("vr", "screen_distance", c.screen_distance);
  c.screen_width = ini.Float("vr", "screen_width", c.screen_width, 0.1f);
  c.screen_height_offset = ini.Float("vr", "screen_height_offset", c.screen_height_offset);
  c.world_scale = ini.Float("vr", "world_scale", c.world_scale, 0.001f);
  c.world_scale_width = ini.Float("vr", "world_scale_width", c.world_scale_width, 0.05f);
  c.cinematic_scale = ini.Float("vr", "cinematic_scale", c.cinematic_scale, 0.1f, 1.0f);
  c.cinematic_fov = ini.Float("vr", "cinematic_fov", c.cinematic_fov, 0.0f);
  c.refresh_rate = ini.Float("vr", "refresh_rate", c.refresh_rate);
  c.simulate_headset = ini.Bool("vr", "simulate_headset", c.simulate_headset);

  const std::string replays = ini.String("render", "replays_per_frame", "1");
  c.replays_per_frame = _stricmp(replays.c_str(), "auto") == 0 ? 0 : atoi(replays.c_str());
  if (c.replays_per_frame < 0) c.replays_per_frame = 1;
  if (c.replays_per_frame > 8) c.replays_per_frame = 8;
  c.game_fps = ini.Int("render", "game_fps", c.game_fps, 10);
  c.verify = ini.Bool("render", "verify", c.verify);
  c.glitch_catch = ini.Bool("render", "glitch_catch", c.glitch_catch);
  c.hide_letterbox = ini.Bool("render", "hide_letterbox", c.hide_letterbox);
  c.execute_live = ini.Bool("render", "execute_live", c.execute_live);
  c.render_scale = ini.Float("render", "render_scale", c.render_scale, 0.5f, 3.0f);

  c.guide_recenter = ini.Bool("controller", "guide_recenter", c.guide_recenter);
  for (int i = 0; i < kPadControlCount; ++i) {
    c.pad_buttons[i] = ini.Int("controller", kPadControlNames[i], c.pad_buttons[i]);
  }

  c.sim_head_amplitude = ini.Float("test", "sim_head_amplitude", c.sim_head_amplitude);
  c.sim_head_frequency = ini.Float("test", "sim_head_frequency", c.sim_head_frequency);
  c.sim_head_rotation = ini.Float("test", "sim_head_rotation", c.sim_head_rotation);
  c.sim_head_step = ini.Float("test", "sim_head_step", c.sim_head_step);
  c.sim_headset_width = ini.Int("test", "sim_headset_width", c.sim_headset_width);
  c.sim_headset_height = ini.Int("test", "sim_headset_height", c.sim_headset_height);
  c.sim_headset_fov = ini.Float("test", "sim_headset_fov", c.sim_headset_fov);
  c.output_grid = ini.Bool("test", "output_grid", c.output_grid);
  c.debug_keys = ini.Bool("test", "debug_keys", c.debug_keys);
  c.dump_passes = ini.Bool("test", "dump_passes", c.dump_passes);
  c.share_eye_targets = ini.Bool("test", "share_eye_targets", c.share_eye_targets);
  c.flat = ini.Bool("test", "flat", c.flat);
  c.test_script = ini.String("test", "script", "");

  Logf("config: windowed=%d borderless=%d window=%dx%d fps_limit=%d",
       c.windowed, c.borderless, c.window_width, c.window_height, c.fps_limit);
  Logf("config: vr=%d screen_distance=%.2f screen_width=%.2f "
       "screen_height_offset=%.2f",
       c.vr_enabled, c.screen_distance, c.screen_width, c.screen_height_offset);
  return c;
}

}  // namespace

const Config& GetConfig() {
  static const Config config = Load();
  return config;
}

void SaveScreenSettings(float width, float distance) {
  const std::string path = IniPath();
  char buf[32];
  snprintf(buf, sizeof(buf), "%.2f", width);
  WritePrivateProfileStringA("vr", "screen_width", buf, path.c_str());
  snprintf(buf, sizeof(buf), "%.2f", distance);
  WritePrivateProfileStringA("vr", "screen_distance", buf, path.c_str());
}

}  // namespace kkvr

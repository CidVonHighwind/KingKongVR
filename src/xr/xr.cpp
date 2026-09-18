#include "xr/xr.h"

#include "common/config.h"
#include "render/eye_projection.h"
#include "common/log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <cstring>

namespace kkvr {
namespace {

// Swapchain formats in order of preference. The game's output is already
// sRGB-encoded, so an _SRGB format shows it unchanged. The B8G8R8A8 variants
// match the D3D9 X8R8G8B8 byte order and need no channel swap.
constexpr int64_t kFormats[] = {
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM,
};

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
  // v' = v + 2w(q x v) + 2 q x (q x v)
  const XrVector3f u{q.x, q.y, q.z};
  const XrVector3f c1{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                      u.x * v.y - u.y * v.x};
  const XrVector3f c2{u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z,
                      u.x * c1.y - u.y * c1.x};
  return {v.x + 2.0f * (q.w * c1.x + c2.x), v.y + 2.0f * (q.w * c1.y + c2.y),
          v.z + 2.0f * (q.w * c1.z + c2.z)};
}

class XrRunner {
 public:
  explicit XrRunner(XrPresenter* owner) : owner_(owner) {}
  ~XrRunner() { DestroyAll(); }

  void Run(const std::atomic<bool>& stop, std::atomic<bool>& wants_frames,
           std::atomic<bool>& recenter);

 private:
  struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    bool ready = false;  // at least one image released
  };

  bool Check(XrResult r, const char* what);
  bool CreateInstance();
  bool GetSystem();
  bool CreateDevice();
  bool CreateSession();
  void ApplyRefreshRate();
  bool EnsureSwapchains(int width, int height);
  void Upload(int eye, const XrPresenter::Frame& frame);
  // Shared texture of a zero-copy frame, opened on device_ (cached).
  ID3D11Texture2D* OpenShared(void* handle);
  void ReleaseShared();
  std::map<void*, ID3D11Texture2D*> shared_textures_;
  uint32_t shared_generation_seen_ = 0;
  // Simulated headset: stand-in swapchain images for zero-copy frames.
  ID3D11Texture2D* sim_images_[2] = {};
  int sim_width_ = 0, sim_height_ = 0;
  void SimCopyShared(const XrPresenter::Frame& frame);
  uint32_t sim_copies_ = 0, sim_probe_ok_ = 0, sim_probe_bad_ = 0;
  bool sim_alpha_logged_ = false;
  double sim_copy_ms_ = 0.0;
  // The runtime's recommended eye image size (CreateSession), and whether the
  // resolution line (LogResolution) has been written.
  XrViewConfigurationView config_view_{XR_TYPE_VIEW_CONFIGURATION_VIEW};
  bool resolution_logged_ = false;
  void LogResolution(const XrView& view);
  void PollEvents();
  void RunFrame(std::atomic<bool>& recenter);
  bool Recenter(XrTime time);
  void PlaceScreen();
  bool LocateWindowEyes(XrTime time, XrPresenter::EyeView views[2]);
  void PublishPredictedEyes(XrTime display_time);
  void MeasureShownPose(XrTime display_time, const XrPresenter::EyeView true_views[2],
                        bool valid, bool new_image);
  void LogPoseStats();
  void RunSimulated(const std::atomic<bool>& stop, std::atomic<bool>& wants_frames);
  void SimulatedWindowEyes(XrTime time, XrPresenter::EyeView views[2]) const;
  void DestroySwapchains();
  void DestroySession();
  void DestroyAll();

  XrPresenter* owner_;
  const Config& config_ = GetConfig();

  // Pose timing. Frames are rendered for a pose predicted `prediction_ns_`
  // beyond the display time of the XR frame that published it. When a frame
  // is first shown, (display time - its pose time) is how late it is; the
  // prediction integrates that towards zero. Repeats of an old image (the
  // game is slower than the headset, or stalls while loading) are counted in
  // the stats but never steer the prediction: no prediction can fix them.
  double prediction_ns_ = 30e6;  // start near the usual pipeline delay
  int64_t shown_pose_time_ = 0;
  double late_sum_ms_ = 0.0;
  double error_sum_mm_ = 0.0;
  double error_max_mm_ = 0.0;
  uint32_t pose_samples_ = 0;
  double first_late_sum_ms_ = 0.0;  // new images only
  uint32_t first_samples_ = 0;
  uint32_t error_samples_ = 0;

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId system_ = XR_NULL_SYSTEM_ID;
  XrEnvironmentBlendMode blend_mode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  bool refresh_rate_ext_ = false;  // XR_FB_display_refresh_rate enabled
  bool logged_no_system_ = false;

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;

  XrSession session_ = XR_NULL_HANDLE;
  XrSpace local_space_ = XR_NULL_HANDLE;
  XrSpace view_space_ = XR_NULL_HANDLE;
  bool session_running_ = false;
  bool session_ended_ = false;   // EXITING / LOSS_PENDING: rebuild the session
  bool instance_lost_ = false;

  EyeSwapchain swapchains_[2];
  int64_t format_ = 0;
  int swap_width_ = 0;
  int swap_height_ = 0;

  // What the swapchains currently hold, for the layer and the pose stats.
  struct Shown {
    int width = 0;
    int height = 0;
    float quad_width = 0.0f;
    XrPresenter::EyeView views[2];
  } shown_;
  // Orientation from Recenter; PlaceScreen sets the position every frame.
  XrPosef screen_pose_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
  // Head position and flattened forward direction at the last recentre.
  XrVector3f anchor_{0.0f, 0.0f, 0.0f};
  XrVector3f forward_{0.0f, 0.0f, -1.0f};

  // Stats, logged every few seconds.
  uint32_t xr_frames_ = 0;
  uint32_t uploads_ = 0;
  double upload_ms_ = 0.0;
  double wait_ms_ = 0.0;     // time blocked in xrWaitFrame
  double submit_ms_ = 0.0;   // xrBeginFrame through xrEndFrame
  double begin_ms_ = 0.0;    // xrBeginFrame alone
  double locate_ms_ = 0.0;   // recentre + xrLocateViews
  double end_ms_ = 0.0;      // xrEndFrame alone
  double period_ms_ = 0.0;   // runtime's predictedDisplayPeriod
  uint32_t not_rendered_ = 0;  // frames with shouldRender == false
  ULONGLONG stats_since_ = 0;
};

bool XrRunner::Check(XrResult r, const char* what) {
  if (XR_SUCCEEDED(r)) return true;
  char name[XR_MAX_RESULT_STRING_SIZE] = {};
  if (instance_ == XR_NULL_HANDLE ||
      XR_FAILED(xrResultToString(instance_, r, name))) {
    snprintf(name, sizeof(name), "%d", static_cast<int>(r));
  }
  Logf("xr: %s failed: %s", what, name);
  if (r == XR_ERROR_INSTANCE_LOST) instance_lost_ = true;
  if (r == XR_ERROR_SESSION_LOST) session_ended_ = true;
  return false;
}

bool XrRunner::CreateInstance() {
  // Optional: choosing the headset refresh rate.
  uint32_t available = 0;
  xrEnumerateInstanceExtensionProperties(nullptr, 0, &available, nullptr);
  std::vector<XrExtensionProperties> props_list(
      available, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, available, &available,
                                         props_list.data());
  refresh_rate_ext_ = false;
  for (const XrExtensionProperties& p : props_list) {
    if (strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) {
      refresh_rate_ext_ = true;
    }
  }

  const char* extensions[2] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
  uint32_t extension_count = 1;
  if (refresh_rate_ext_) {
    extensions[extension_count++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
  }
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  strcpy(ci.applicationInfo.applicationName, "King Kong VR");
  ci.applicationInfo.applicationVersion = 1;
  strcpy(ci.applicationInfo.engineName, "kkvr");
  ci.applicationInfo.engineVersion = 1;
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = extension_count;
  ci.enabledExtensionNames = extensions;
  if (!Check(xrCreateInstance(&ci, &instance_), "xrCreateInstance")) {
    instance_ = XR_NULL_HANDLE;
    return false;
  }
  XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &props))) {
    Logf("xr: runtime %s %u.%u.%u", props.runtimeName,
         XR_VERSION_MAJOR(props.runtimeVersion),
         XR_VERSION_MINOR(props.runtimeVersion),
         XR_VERSION_PATCH(props.runtimeVersion));
  }
  Logf("xr: %u runtime extensions, refresh rate control %s", available,
       refresh_rate_ext_ ? "available" : "NOT available");
  return true;
}

bool XrRunner::GetSystem() {
  XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
  gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  const XrResult r = xrGetSystem(instance_, &gi, &system_);
  if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
    if (!logged_no_system_) Logf("xr: no headset available yet, waiting");
    logged_no_system_ = true;
    system_ = XR_NULL_SYSTEM_ID;
    return false;
  }
  if (!Check(r, "xrGetSystem")) {
    system_ = XR_NULL_SYSTEM_ID;
    return false;
  }

  XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(xrGetSystemProperties(instance_, system_, &props))) {
    Logf("xr: system \"%s\", max layers %u", props.systemName,
         props.graphicsProperties.maxLayerCount);
  }

  XrEnvironmentBlendMode modes[8];
  uint32_t count = 0;
  if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(
          instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8,
          &count, modes)) &&
      count > 0) {
    blend_mode_ = modes[0];
  }
  return true;
}

bool XrRunner::CreateDevice() {
  // The loader only exports core functions; extension entry points come from
  // the runtime through xrGetInstanceProcAddr.
  PFN_xrVoidFunction fn = nullptr;
  if (!Check(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
                                   &fn),
             "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)") ||
      !fn) {
    return false;
  }
  const auto get_requirements =
      reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(fn);
  XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  if (!Check(get_requirements(instance_, system_, &req),
             "xrGetD3D11GraphicsRequirementsKHR")) {
    return false;
  }

  // The runtime dictates the adapter by LUID.
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                reinterpret_cast<void**>(&factory)))) {
    Logf("xr: CreateDXGIFactory1 failed");
    return false;
  }
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc;
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        desc.AdapterLuid.LowPart == req.adapterLuid.LowPart &&
        desc.AdapterLuid.HighPart == req.adapterLuid.HighPart) {
      char name[128] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name),
                          nullptr, nullptr);
      Logf("xr: using adapter %s", name);
      owner_->SetAdapterLuid(static_cast<int64_t>(
          (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
          desc.AdapterLuid.LowPart));
      break;
    }
    adapter->Release();
    adapter = nullptr;
  }
  factory->Release();
  if (!adapter) {
    Logf("xr: adapter required by the runtime not found");
    return false;
  }

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  const HRESULT hr = D3D11CreateDevice(
      adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
      sizeof(levels) / sizeof(levels[0]), D3D11_SDK_VERSION, &device_, nullptr,
      &context_);
  adapter->Release();
  if (FAILED(hr)) {
    Logf("xr: D3D11CreateDevice failed hr=0x%08X", hr);
    return false;
  }
  return true;
}

bool XrRunner::CreateSession() {
  XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
  binding.device = device_;
  XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
  ci.next = &binding;
  ci.systemId = system_;
  if (!Check(xrCreateSession(instance_, &ci, &session_), "xrCreateSession")) {
    session_ = XR_NULL_HANDLE;
    return false;
  }

  {
    XrViewConfigurationView config_views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                               {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t n = 0;
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(
            instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n,
            config_views)) && n >= 1) {
      config_view_ = config_views[0];
      owner_->SetRecommendedSize(static_cast<int>(config_view_.recommendedImageRectWidth),
                                 static_cast<int>(config_view_.recommendedImageRectHeight));
    }
  }

  XrReferenceSpaceCreateInfo si{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  si.poseInReferenceSpace.orientation.w = 1.0f;
  si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  if (!Check(xrCreateReferenceSpace(session_, &si, &local_space_),
             "xrCreateReferenceSpace(LOCAL)")) {
    DestroySession();
    return false;
  }
  si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  if (!Check(xrCreateReferenceSpace(session_, &si, &view_space_),
             "xrCreateReferenceSpace(VIEW)")) {
    DestroySession();
    return false;
  }
  session_ended_ = false;
  Logf("xr: session created");
  ApplyRefreshRate();
  return true;
}

// Ask for [vr] refresh_rate (the closest rate the headset offers). The
// compositor shows the latest game frame at that rate.
void XrRunner::ApplyRefreshRate() {
  if (!refresh_rate_ext_ || config_.refresh_rate <= 0.0f) return;
  PFN_xrVoidFunction fn = nullptr;
  xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRefreshRatesFB", &fn);
  const auto enumerate = reinterpret_cast<PFN_xrEnumerateDisplayRefreshRatesFB>(fn);
  fn = nullptr;
  xrGetInstanceProcAddr(instance_, "xrRequestDisplayRefreshRateFB", &fn);
  const auto request = reinterpret_cast<PFN_xrRequestDisplayRefreshRateFB>(fn);
  fn = nullptr;
  xrGetInstanceProcAddr(instance_, "xrGetDisplayRefreshRateFB", &fn);
  const auto current = reinterpret_cast<PFN_xrGetDisplayRefreshRateFB>(fn);
  if (!enumerate || !request) {
    Logf("xr: refresh rate functions missing");
    return;
  }

  uint32_t count = 0;
  if (!Check(enumerate(session_, 0, &count, nullptr),
             "xrEnumerateDisplayRefreshRatesFB") || count == 0) {
    return;
  }
  std::vector<float> rates(count);
  enumerate(session_, count, &count, rates.data());
  char list[256] = {};
  float best = rates[0];
  for (float r : rates) {
    const size_t used = strlen(list);
    snprintf(list + used, sizeof(list) - used, "%s%.0f", used ? " " : "", r);
    if (std::fabs(r - config_.refresh_rate) < std::fabs(best - config_.refresh_rate)) {
      best = r;
    }
  }
  float before = 0.0f;
  if (current) current(session_, &before);
  const bool ok = Check(request(session_, best), "xrRequestDisplayRefreshRateFB");
  Logf("xr: refresh rates offered: %s Hz; was %.0f Hz, requested %.0f Hz%s", list,
       before, best, ok ? "" : " (refused)");
}

bool XrRunner::EnsureSwapchains(int width, int height) {
  if (swapchains_[0].handle != XR_NULL_HANDLE && width == swap_width_ &&
      height == swap_height_) {
    return true;
  }
  DestroySwapchains();

  uint32_t count = 0;
  if (!Check(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr),
             "xrEnumerateSwapchainFormats")) {
    return false;
  }
  std::vector<int64_t> formats(count);
  xrEnumerateSwapchainFormats(session_, count, &count, formats.data());
  format_ = 0;
  for (int64_t wanted : kFormats) {
    for (int64_t f : formats) {
      if (f == wanted) {
        format_ = f;
        break;
      }
    }
    if (format_) break;
  }
  if (!format_) {
    Logf("xr: runtime offers no 8-bit RGBA swapchain format");
    return false;
  }

  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags =
      XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  ci.format = format_;
  ci.sampleCount = 1;
  ci.width = static_cast<uint32_t>(width);
  ci.height = static_cast<uint32_t>(height);
  ci.faceCount = 1;
  ci.arraySize = 1;
  ci.mipCount = 1;
  for (EyeSwapchain& sc : swapchains_) {
    if (!Check(xrCreateSwapchain(session_, &ci, &sc.handle), "xrCreateSwapchain")) {
      sc.handle = XR_NULL_HANDLE;
      DestroySwapchains();
      return false;
    }
    xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
    sc.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!Check(xrEnumerateSwapchainImages(
                   sc.handle, count, &count,
                   reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())),
               "xrEnumerateSwapchainImages")) {
      DestroySwapchains();
      return false;
    }
  }
  swap_width_ = width;
  swap_height_ = height;
  Logf("xr: 2 swapchains %dx%d format=%lld images=%u", width, height, format_,
       count);
  return true;
}

// Copy one eye from the game's shared texture into the swapchain (GPU copy).
void XrRunner::Upload(int eye, const XrPresenter::Frame& frame) {
  EyeSwapchain& sc = swapchains_[eye];
  if (sc.handle == XR_NULL_HANDLE) return;

  uint32_t index = 0;
  XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (!Check(xrAcquireSwapchainImage(sc.handle, &ai, &index),
             "xrAcquireSwapchainImage")) {
    return;
  }
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = XR_INFINITE_DURATION;
  if (!Check(xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage")) {
    // The image stays acquired and cannot be released without a successful
    // wait, so every later acquire would fail and the headset image freeze.
    // New swapchains are made with the next frame.
    DestroySwapchains();
    return;
  }
  // Same format family (B8G8R8A8), and the swapchain was made at the
  // texture's size.
  if (ID3D11Texture2D* source = frame.shared[eye] ? OpenShared(frame.shared[eye]) : nullptr) {
    context_->CopyResource(sc.images[index].texture, source);
  }
  XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  if (Check(xrReleaseSwapchainImage(sc.handle, &ri), "xrReleaseSwapchainImage")) {
    sc.ready = true;
  }
}

ID3D11Texture2D* XrRunner::OpenShared(void* handle) {
  const uint32_t generation = owner_->SharedGeneration();
  if (generation != shared_generation_seen_) {
    ReleaseShared();
    shared_generation_seen_ = generation;
  }
  auto it = shared_textures_.find(handle);
  if (it != shared_textures_.end()) return it->second;
  ID3D11Texture2D* texture = nullptr;
  const HRESULT hr = device_ ? device_->OpenSharedResource(static_cast<HANDLE>(handle),
                                                           __uuidof(ID3D11Texture2D),
                                                           reinterpret_cast<void**>(&texture))
                             : E_FAIL;
  if (FAILED(hr)) {
    Logf("xr: OpenSharedResource(%p) failed hr=0x%08X", handle, static_cast<unsigned>(hr));
    texture = nullptr;
  }
  shared_textures_[handle] = texture;
  return texture;
}

void XrRunner::ReleaseShared() {
  for (auto& kv : shared_textures_) {
    if (kv.second) kv.second->Release();
  }
  shared_textures_.clear();
}

// Simulated headset, zero-copy frame: the same GPU copy the real path does,
// into stand-in swapchain textures, plus a content check of one pixel.
void XrRunner::SimCopyShared(const XrPresenter::Frame& frame) {
  if (!device_) {
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device_, nullptr, &context_))) {
      Logf("xr (simulated): D3D11CreateDevice failed");
      return;
    }
    IDXGIDevice* dxgi = nullptr;
    if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice),
                                          reinterpret_cast<void**>(&dxgi)))) {
      IDXGIAdapter* adapter = nullptr;
      DXGI_ADAPTER_DESC desc{};
      if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
        owner_->SetAdapterLuid(static_cast<int64_t>(
            (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
            desc.AdapterLuid.LowPart));
      }
      if (adapter) adapter->Release();
      dxgi->Release();
    }
  }
  if (frame.width != sim_width_ || frame.height != sim_height_) {
    for (ID3D11Texture2D*& t : sim_images_) {
      if (t) t->Release();
      t = nullptr;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(frame.width);
    d.Height = static_cast<UINT>(frame.height);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;  // what the real swapchain uses
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (ID3D11Texture2D*& t : sim_images_) device_->CreateTexture2D(&d, nullptr, &t);
    sim_width_ = frame.width;
    sim_height_ = frame.height;
  }
  LARGE_INTEGER t0, t1, freq;
  QueryPerformanceCounter(&t0);
  for (int eye = 0; eye < 2; ++eye) {
    ID3D11Texture2D* source = frame.shared[eye] ? OpenShared(frame.shared[eye]) : nullptr;
    if (source && sim_images_[eye]) context_->CopyResource(sim_images_[eye], source);
  }
  context_->Flush();
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&freq);
  sim_copy_ms_ += 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
  ++sim_copies_;

  // Every 120th frame: read the centre pixel of the left image back.
  if (sim_copies_ % 120 == 1 && sim_images_[0]) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 1;
    d.Height = 1;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(device_->CreateTexture2D(&d, nullptr, &staging))) {
      const UINT x = static_cast<UINT>(frame.width / 2), y = static_cast<UINT>(frame.height / 2);
      D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
      context_->CopySubresourceRegion(staging, 0, 0, 0, 0, sim_images_[0], 0, &box);
      D3D11_MAPPED_SUBRESOURCE map{};
      if (SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
        const auto* p = static_cast<const uint8_t*>(map.pData);
        const bool same = p[0] == frame.probe[0] && p[1] == frame.probe[1] &&
                          p[2] == frame.probe[2];
        (same ? sim_probe_ok_ : sim_probe_bad_)++;
        if (!same) {
          Logf("xr (simulated): shared pixel mismatch: got %02X %02X %02X, game had %02X %02X %02X",
               p[0], p[1], p[2], frame.probe[0], frame.probe[1], frame.probe[2]);
        }
        context_->Unmap(staging, 0);
      }
      if (!sim_alpha_logged_) {
        // The projection layer needs alpha 0 outside the window: check a
        // corner, once.
        sim_alpha_logged_ = true;
        D3D11_BOX corner{2, 2, 0, 3, 3, 1};
        context_->CopySubresourceRegion(staging, 0, 0, 0, 0, sim_images_[0], 0, &corner);
        if (SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
          const auto* p = static_cast<const uint8_t*>(map.pData);
          Logf("xr (simulated): projection image alpha: corner %u (colour %02X %02X %02X), "
               "centre pixel alpha %u", p[3], p[2], p[1], p[0], frame.probe[3]);
          context_->Unmap(staging, 0);
        }
      }
      staging->Release();
    }
  }
}

void XrRunner::PollEvents() {
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
    if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      Logf("xr: instance loss pending");
      instance_lost_ = true;
    } else if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto* e =
          reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
      Logf("xr: session state %d", static_cast<int>(e->state));
      switch (e->state) {
        case XR_SESSION_STATE_READY: {
          XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
          bi.primaryViewConfigurationType =
              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          if (Check(xrBeginSession(session_, &bi), "xrBeginSession")) {
            session_running_ = true;
          }
          break;
        }
        case XR_SESSION_STATE_STOPPING:
          Check(xrEndSession(session_), "xrEndSession");
          session_running_ = false;
          break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
          session_running_ = false;
          session_ended_ = true;
          break;
        default:
          break;
      }
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

bool XrRunner::Recenter(XrTime time) {
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(view_space_, local_space_, time, &loc))) return false;
  const XrSpaceLocationFlags needed =
      XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if ((loc.locationFlags & needed) != needed) return false;

  // Yaw only: the screen stays upright wherever the head was pitched.
  XrVector3f fwd = Rotate(loc.pose.orientation, {0.0f, 0.0f, -1.0f});
  const float len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
  if (len < 1e-3f) {
    fwd = {0.0f, 0.0f, -1.0f};
  } else {
    fwd.x /= len;
    fwd.z /= len;
  }
  // A rotation of theta about +Y maps -Z to (-sin theta, 0, -cos theta).
  const float theta = std::atan2(-fwd.x, -fwd.z);
  screen_pose_.orientation = {0.0f, std::sin(theta * 0.5f), 0.0f,
                              std::cos(theta * 0.5f)};
  anchor_ = loc.pose.position;
  forward_ = fwd;
  PlaceScreen();
  Logf("xr: screen recentred at (%.2f, %.2f, %.2f), yaw %.1f deg",
       screen_pose_.position.x, screen_pose_.position.y,
       screen_pose_.position.z, theta * 57.29578f);
  return true;
}

// The screen hangs the current distance in front of the recentre point.
void XrRunner::PlaceScreen() {
  const float d = owner_->ScreenDistance();
  screen_pose_.position = {anchor_.x + forward_.x * d,
                           anchor_.y + config_.screen_height_offset,
                           anchor_.z + forward_.z * d};
}

// Where are the eyes relative to the window at `time`? Window frame: origin at
// the quad centre, +x right, +y up, +z out of the quad towards the viewer.
bool XrRunner::LocateWindowEyes(XrTime time, XrPresenter::EyeView out_views[2]) {
  XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
  info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  info.displayTime = time;
  info.space = local_space_;
  XrViewState state{XR_TYPE_VIEW_STATE};
  XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  uint32_t count = 0;
  if (XR_FAILED(xrLocateViews(session_, &info, &state, 2, &count, views)) ||
      count < 2 || !(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
    return false;
  }
  const XrQuaternionf& q = screen_pose_.orientation;
  const XrQuaternionf inverse{-q.x, -q.y, -q.z, q.w};
  for (int eye = 0; eye < 2; ++eye) {
    const XrVector3f& e = views[eye].pose.position;
    const XrVector3f local = Rotate(
        inverse, {e.x - screen_pose_.position.x, e.y - screen_pose_.position.y,
                  e.z - screen_pose_.position.z});
    XrPresenter::EyeView& v = out_views[eye];
    v.position[0] = local.x;
    v.position[1] = local.y;
    v.position[2] = local.z;
    // Orientation in the window frame: inverse(screen) * eye.
    const XrQuaternionf& o = views[eye].pose.orientation;
    const XrQuaternionf& a = inverse;
    v.orientation[0] = a.w * o.x + a.x * o.w + a.y * o.z - a.z * o.y;
    v.orientation[1] = a.w * o.y - a.x * o.z + a.y * o.w + a.z * o.x;
    v.orientation[2] = a.w * o.z + a.x * o.y - a.y * o.x + a.z * o.w;
    v.orientation[3] = a.w * o.w - a.x * o.x - a.y * o.y - a.z * o.z;
    const XrFovf& f = views[eye].fov;
    v.fov[0] = f.angleLeft;
    v.fov[1] = f.angleRight;
    v.fov[2] = f.angleUp;
    v.fov[3] = f.angleDown;
    v.tangents[0] = std::tan(f.angleLeft);
    v.tangents[1] = std::tan(f.angleRight);
    v.tangents[2] = std::tan(f.angleUp);
    v.tangents[3] = std::tan(f.angleDown);
    v.local_position[0] = e.x;
    v.local_position[1] = e.y;
    v.local_position[2] = e.z;
    v.local_orientation[0] = o.x;
    v.local_orientation[1] = o.y;
    v.local_orientation[2] = o.z;
    v.local_orientation[3] = o.w;
  }
  if (!resolution_logged_ && shown_.quad_width > 0.0f &&
      config_view_.recommendedImageRectWidth > 0) {
    resolution_logged_ = true;
    LogResolution(views[0]);
  }
  return true;
}

// Once: how many headset pixels does the window cover? The runtime's
// recommended eye image width over the eye's horizontal field of view gives
// pixels per degree at the centre.
void XrRunner::LogResolution(const XrView& view) {
  const double fov_deg = (view.fov.angleRight - view.fov.angleLeft) * 57.29578;
  const double ppd = config_view_.recommendedImageRectWidth / fov_deg;
  const XrQuaternionf& q = screen_pose_.orientation;
  const XrVector3f& e = view.pose.position;
  const XrVector3f local =
      Rotate({-q.x, -q.y, -q.z, q.w}, {e.x - screen_pose_.position.x,
                                        e.y - screen_pose_.position.y,
                                        e.z - screen_pose_.position.z});
  const double distance = local.z > 0.05f ? local.z : 0.05;
  const double window_deg = 2.0 * std::atan(0.5 * shown_.quad_width / distance) * 57.29578;
  Logf("xr: headset eye image %ux%u over %.0f deg (%.1f px/deg); the %.2f m window at "
       "%.2f m spans %.0f deg, so ~%.0f px wide is native (eye images are %d px)",
       config_view_.recommendedImageRectWidth, config_view_.recommendedImageRectHeight,
       fov_deg, ppd, shown_.quad_width, distance, window_deg, ppd * window_deg, shown_.width);
}

// Publish the eyes for where the head will be when a frame rendered now is
// actually shown: the XR frame's display time plus the pipeline delay.
void XrRunner::PublishPredictedEyes(XrTime display_time) {
  const XrTime time = display_time + static_cast<XrTime>(prediction_ns_);
  XrPresenter::EyeView views[2];
  const bool ok = config_.simulate_headset ? (SimulatedWindowEyes(time, views), true)
                                           : LocateWindowEyes(time, views);
  owner_->PublishWindowViews(views, ok, time);
}

// Called for every XR frame with the eyes' true views at its display time.
void XrRunner::MeasureShownPose(XrTime display_time, const XrPresenter::EyeView true_views[2],
                                bool valid, bool new_image) {
  if (shown_pose_time_ == 0) return;
  const double late_ms = static_cast<double>(display_time - shown_pose_time_) / 1e6;
  late_sum_ms_ += late_ms;
  ++pose_samples_;
  if (valid) {
    const float* shown = shown_.views[0].position;
    const float* truth = true_views[0].position;
    const double dx = truth[0] - shown[0];
    const double dy = truth[1] - shown[1];
    const double dz = truth[2] - shown[2];
    const double mm = 1000.0 * std::sqrt(dx * dx + dy * dy + dz * dz);
    error_sum_mm_ += mm;
    if (mm > error_max_mm_) error_max_mm_ = mm;
    ++error_samples_;
  }
  if (!new_image) return;
  first_late_sum_ms_ += late_ms;
  ++first_samples_;
  // Nudge the prediction so new images are neither late nor early. An image
  // more than 50 ms off comes from a hitch (the game stalled between taking
  // the pose and finishing the frame), not from the pipeline.
  constexpr double kHitchMs = 50.0;
  constexpr double kMaxPredictionNs = 60e6;
  if (std::fabs(late_ms) < kHitchMs) {
    prediction_ns_ += 0.05 * late_ms * 1e6;
    if (prediction_ns_ < 0.0) prediction_ns_ = 0.0;
    if (prediction_ns_ > kMaxPredictionNs) prediction_ns_ = kMaxPredictionNs;
  }
}

void XrRunner::LogPoseStats() {
  if (pose_samples_ == 0) return;
  Logf("xr pose: new images late by %.1f ms, all shown images %.1f ms (0 = on time), "
       "prediction %.1f ms, eye position error avg %.1f mm, max %.1f mm",
       first_samples_ ? first_late_sum_ms_ / first_samples_ : 0.0,
       late_sum_ms_ / pose_samples_, prediction_ns_ / 1e6,
       error_samples_ ? error_sum_mm_ / error_samples_ : 0.0, error_max_mm_);
  late_sum_ms_ = first_late_sum_ms_ = error_sum_mm_ = error_max_mm_ = 0.0;
  pose_samples_ = error_samples_ = first_samples_ = 0;
}

// Synthetic head for [vr] simulate_headset: a brisk side-to-side, up-down and
// forward-back motion in front of the window, like leaning and looking around.
void XrRunner::SimulatedWindowEyes(XrTime time, XrPresenter::EyeView views[2]) const {
  const double s = static_cast<double>(time) / 1e9;
  const double w = 6.283185307 * config_.sim_head_frequency;
  const double a = config_.sim_head_amplitude;
  const float x = static_cast<float>(a * std::sin(w * s));
  const float y = static_cast<float>(0.3 * a * std::sin(0.7 * w * s + 1.0));
  const float z = owner_->ScreenDistance() +
                  static_cast<float>(0.5 * a * std::sin(0.5 * w * s + 2.0));
  // Head rotation (tests rotated eye views): yaw, pitch and roll sway,
  // combined as q = yaw * pitch * roll (x, y, z, w).
  // Manual angles (hotkeys) are added: with sim_head_rotation 0 and
  // sim_head_amplitude 0 the head stands still and only they move it.
  float manual[3];
  owner_->ManualHead(&manual[0], &manual[1], &manual[2]);
  constexpr double kDeg = 3.14159265 / 180.0;
  const double r = config_.sim_head_rotation * kDeg;
  const double yaw = r * std::sin(0.37 * w * s + 0.5) + manual[0] * kDeg;
  const double pitch = 0.6 * r * std::sin(0.53 * w * s + 1.3) + manual[1] * kDeg;
  const double roll = 0.3 * r * std::sin(0.29 * w * s + 2.1) + manual[2] * kDeg;
  const double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2);
  const double cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
  const double cr = std::cos(roll / 2), sr = std::sin(roll / 2);
  // yaw about +y, pitch about +x, roll about +z
  const float q[4] = {static_cast<float>(cy * sp * cr + sy * cp * sr),
                      static_cast<float>(sy * cp * cr - cy * sp * sr),
                      static_cast<float>(cy * cp * sr - sy * sp * cr),
                      static_cast<float>(cy * cp * cr + sy * sp * sr)};
  const float half_ipd[3] = {0.032f, 0.0f, 0.0f};
  float right[3];
  QuatRotate(q, half_ipd, right);
  // Looking roughly at the window: half the configured field of view
  // horizontally, vertically in the proportion of the eye image ([test]
  // sim_headset_*).
  const float half = 0.5f * config_.sim_headset_fov * 3.14159265f / 180.0f;
  const float half_y = half * static_cast<float>(config_.sim_headset_height) /
                       static_cast<float>(config_.sim_headset_width);
  const float angles[4] = {-half, half, half_y, -half_y};
  for (int eye = 0; eye < 2; ++eye) {
    const float side = eye ? 1.0f : -1.0f;
    XrPresenter::EyeView& v = views[eye];
    v.position[0] = x + side * right[0];
    v.position[1] = y + side * right[1];
    v.position[2] = z + side * right[2];
    for (int k = 0; k < 4; ++k) v.orientation[k] = q[k];
    for (int k = 0; k < 4; ++k) {
      v.fov[k] = angles[k];
      v.tangents[k] = std::tan(angles[k]);
    }
    for (int k = 0; k < 3; ++k) v.local_position[k] = v.position[k];
  }
}

// Stand-in for the OpenXR frame loop when no headset is available: same
// timing (120 Hz, frame uploads, pose publishing and measurement), no
// rendering. Lets latency and prediction be tested on a desktop.
void XrRunner::RunSimulated(const std::atomic<bool>& stop,
                            std::atomic<bool>& wants_frames) {
  LARGE_INTEGER freq, now;
  QueryPerformanceFrequency(&freq);
  const auto now_ns = [&]() -> XrTime {
    QueryPerformanceCounter(&now);
    return static_cast<XrTime>(now.QuadPart / freq.QuadPart) * 1000000000LL +
           static_cast<XrTime>(now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart;
  };
  constexpr XrTime kPeriod = 1000000000LL / 120;
  owner_->SetRecommendedSize(config_.sim_headset_width, config_.sim_headset_height);
  Logf("xr: simulated headset (no OpenXR), 120 Hz, eye image %dx%d over %.0f deg, head amplitude "
       "%.2f m at %.2f Hz", config_.sim_headset_width, config_.sim_headset_height,
       config_.sim_headset_fov, config_.sim_head_amplitude, config_.sim_head_frequency);
  wants_frames.store(true);
  XrTime next = now_ns() + kPeriod;
  ULONGLONG stats_since = GetTickCount64();
  uint32_t frames = 0;
  uint32_t uploads = 0;
  while (!stop.load()) {
    // xrWaitFrame: return at the start of the next frame.
    for (XrTime t = now_ns(); t < next; t = now_ns()) {
      if (next - t > 2000000) Sleep(1); else YieldProcessor();
    }
    const XrTime display_time = next + kPeriod;  // shown at the end of the frame
    next += kPeriod;
    if (now_ns() > next) next = now_ns() + kPeriod;  // fell behind: resync

    const bool uploaded = owner_->UploadNewFrame([this](const XrPresenter::Frame& f) {
      if (f.shared[0]) SimCopyShared(f);
      shown_.views[0] = f.views[0];
      shown_.views[1] = f.views[1];
      shown_pose_time_ = f.pose_time;
    });
    if (uploaded) ++uploads;
    PublishPredictedEyes(display_time);
    XrPresenter::EyeView true_views[2];
    SimulatedWindowEyes(display_time, true_views);
    MeasureShownPose(display_time, true_views, true, uploaded);
    ++frames;

    const ULONGLONG tick = GetTickCount64();
    if (tick - stats_since >= 5000) {
      const float secs = static_cast<float>(tick - stats_since) / 1000.0f;
      Logf("xr (simulated): %.1f fps, %.1f game frames taken", frames / secs, uploads / secs);
      if (sim_copies_) {
        Logf("xr (simulated): zero-copy: %u GPU copies, %.3f ms each (incl. flush), pixel "
             "checks %u ok / %u wrong", sim_copies_, sim_copy_ms_ / sim_copies_, sim_probe_ok_,
             sim_probe_bad_);
        sim_copies_ = sim_probe_ok_ = sim_probe_bad_ = 0;
        sim_copy_ms_ = 0.0;
      }
      LogPoseStats();
      stats_since = tick;
      frames = uploads = 0;
    }
  }
  wants_frames.store(false);
}

void XrRunner::RunFrame(std::atomic<bool>& recenter) {
  LARGE_INTEGER freq, before_wait, after_wait, after_end;
  QueryPerformanceFrequency(&freq);
  const auto ms = [&freq](const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    return 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) /
           static_cast<double>(freq.QuadPart);
  };
  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState state{XR_TYPE_FRAME_STATE};
  QueryPerformanceCounter(&before_wait);
  if (!Check(xrWaitFrame(session_, &wait_info, &state), "xrWaitFrame")) return;
  QueryPerformanceCounter(&after_wait);
  wait_ms_ += ms(before_wait, after_wait);
  period_ms_ += static_cast<double>(state.predictedDisplayPeriod) / 1e6;
  if (!state.shouldRender) ++not_rendered_;
  // Upload a new game frame right away, so this XR frame already shows it
  // (uploading after xrEndFrame delayed every image by one headset frame).
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  const bool uploaded = owner_->UploadNewFrame([this](const XrPresenter::Frame& f) {
    if (!EnsureSwapchains(f.width, f.height)) return;
    Upload(0, f);
    Upload(1, f);
    shown_.width = f.width;
    shown_.height = f.height;
    shown_.quad_width = f.quad_width;
    shown_.views[0] = f.views[0];
    shown_.views[1] = f.views[1];
    shown_pose_time_ = f.pose_time;
  });
  if (uploaded) {
    QueryPerformanceCounter(&t1);
    upload_ms_ += ms(t0, t1);
    ++uploads_;
  }

  XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
  LARGE_INTEGER before_begin, after_begin, after_locate, before_end;
  QueryPerformanceCounter(&before_begin);
  if (!Check(xrBeginFrame(session_, &begin_info), "xrBeginFrame")) return;
  QueryPerformanceCounter(&after_begin);
  begin_ms_ += ms(before_begin, after_begin);

  if (recenter.load() && Recenter(state.predictedDisplayTime)) {
    recenter.store(false);
  }
  PlaceScreen();
  PublishPredictedEyes(state.predictedDisplayTime);
  {
    XrPresenter::EyeView true_views[2];
    const bool valid = LocateWindowEyes(state.predictedDisplayTime, true_views);
    MeasureShownPose(state.predictedDisplayTime, true_views, valid, uploaded);
  }
  QueryPerformanceCounter(&after_locate);
  locate_ms_ += ms(after_begin, after_locate);

  const XrCompositionLayerBaseHeader* layers[1];
  uint32_t layer_count = 0;
  // Each eye image is that eye's own view, rendered for exactly these poses
  // and fields of view; the compositor reprojects from them. Alpha is 0
  // outside the window.
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView projection_views[2] = {
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  if (state.shouldRender && swapchains_[0].ready && swapchains_[1].ready) {
    for (int eye = 0; eye < 2; ++eye) {
      const XrPresenter::EyeView& v = shown_.views[eye];
      XrCompositionLayerProjectionView& pv = projection_views[eye];
      pv.pose.position = {v.local_position[0], v.local_position[1], v.local_position[2]};
      pv.pose.orientation = {v.local_orientation[0], v.local_orientation[1],
                             v.local_orientation[2], v.local_orientation[3]};
      pv.fov = {v.fov[0], v.fov[1], v.fov[2], v.fov[3]};
      pv.subImage.swapchain = swapchains_[eye].handle;
      pv.subImage.imageRect = {{0, 0}, {swap_width_, swap_height_}};
      pv.subImage.imageArrayIndex = 0;
    }
    projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    projection.space = local_space_;
    projection.viewCount = 2;
    projection.views = projection_views;
    layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
  }

  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = state.predictedDisplayTime;
  end_info.environmentBlendMode = blend_mode_;
  end_info.layerCount = layer_count;
  end_info.layers = layers;
  QueryPerformanceCounter(&before_end);
  Check(xrEndFrame(session_, &end_info), "xrEndFrame");
  QueryPerformanceCounter(&after_end);
  end_ms_ += ms(before_end, after_end);
  submit_ms_ += ms(before_begin, after_end);

  ++xr_frames_;
  const ULONGLONG now = GetTickCount64();
  if (now - stats_since_ >= 5000) {
    if (stats_since_ != 0) {
      const float secs = static_cast<float>(now - stats_since_) / 1000.0f;
      Logf("xr: %.1f fps compositor, %.1f fps game frames uploaded, avg upload "
           "%.2f ms (eye images %dx%d, window %.2f m wide)", xr_frames_ / secs,
           uploads_ / secs, uploads_ ? upload_ms_ / uploads_ : 0.0, shown_.width,
           shown_.height, shown_.quad_width);
      const double n = xr_frames_ ? xr_frames_ : 1;
      Logf("xr loop: runtime display period %.2f ms (%.1f Hz), avg wait %.2f ms, "
           "begin..end %.2f ms (begin %.2f, locate %.2f, end %.2f), "
           "shouldRender false %u of %u",
           period_ms_ / n, period_ms_ > 0 ? 1000.0 * n / period_ms_ : 0.0,
           wait_ms_ / n, submit_ms_ / n, begin_ms_ / n, locate_ms_ / n, end_ms_ / n,
           not_rendered_, xr_frames_);
      LogPoseStats();
    }
    stats_since_ = now;
    xr_frames_ = 0;
    uploads_ = 0;
    upload_ms_ = 0.0;
    wait_ms_ = submit_ms_ = period_ms_ = begin_ms_ = locate_ms_ = end_ms_ = 0.0;
    not_rendered_ = 0;
  }
}

void XrRunner::DestroySwapchains() {
  for (EyeSwapchain& sc : swapchains_) {
    if (sc.handle != XR_NULL_HANDLE) xrDestroySwapchain(sc.handle);
    sc = EyeSwapchain();
  }
  swap_width_ = swap_height_ = 0;
}

void XrRunner::DestroySession() {
  DestroySwapchains();
  if (view_space_ != XR_NULL_HANDLE) xrDestroySpace(view_space_);
  if (local_space_ != XR_NULL_HANDLE) xrDestroySpace(local_space_);
  if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
  view_space_ = local_space_ = XR_NULL_HANDLE;
  session_ = XR_NULL_HANDLE;
  session_running_ = false;
  owner_->PublishWindowViews(nullptr, false, 0);
}

void XrRunner::DestroyAll() {
  DestroySession();
  ReleaseShared();
  for (ID3D11Texture2D*& t : sim_images_) {
    if (t) t->Release();
    t = nullptr;
  }
  if (context_) context_->Release();
  if (device_) device_->Release();
  context_ = nullptr;
  device_ = nullptr;
  if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
  instance_ = XR_NULL_HANDLE;
  system_ = XR_NULL_SYSTEM_ID;
  instance_lost_ = false;
}

void XrRunner::Run(const std::atomic<bool>& stop, std::atomic<bool>& wants_frames,
                   std::atomic<bool>& recenter) {
  if (config_.simulate_headset) {
    RunSimulated(stop, wants_frames);
    DestroyAll();
    return;
  }
  // Wait in small steps so shutdown never blocks for long.
  auto pause = [&stop](DWORD ms) {
    for (DWORD waited = 0; waited < ms && !stop.load(); waited += 50) Sleep(50);
  };

  while (!stop.load()) {
    if (instance_ == XR_NULL_HANDLE && !CreateInstance()) {
      pause(5000);
      continue;
    }
    if (system_ == XR_NULL_SYSTEM_ID && !GetSystem()) {
      pause(2000);
      continue;
    }
    if (!device_ && !CreateDevice()) {
      Logf("xr: giving up, no graphics device");
      break;
    }
    if (session_ == XR_NULL_HANDLE && !CreateSession()) {
      pause(2000);
      continue;
    }

    PollEvents();
    if (instance_lost_) {
      wants_frames.store(false);
      DestroyAll();
      pause(2000);
      continue;
    }
    if (session_ended_) {
      wants_frames.store(false);
      DestroySession();
      pause(1000);
      continue;
    }
    if (!session_running_) {
      wants_frames.store(false);
      pause(100);
      continue;
    }

    wants_frames.store(true);
    RunFrame(recenter);
  }
  wants_frames.store(false);
}

}  // namespace

// ---------------------------------------------------------------------------
// XrPresenter
// ---------------------------------------------------------------------------

XrPresenter::XrPresenter() : screen_distance_(GetConfig().screen_distance) {
  thread_ = std::thread([this] { ThreadMain(); });
}

XrPresenter::~XrPresenter() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
}

void XrPresenter::ThreadMain() {
  Logf("xr: thread started");
  {
    XrRunner runner(this);
    runner.Run(stop_, wants_frames_, recenter_);
  }
  Logf("xr: thread stopped");
}

void XrPresenter::SubmitFrame(const Frame& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (has_pending_ && pending_.slot >= 0) released_ |= 1u << pending_.slot;
  pending_ = frame;
  has_pending_ = true;
}

uint32_t XrPresenter::TakeReleasedSlots() {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint32_t released = released_;
  released_ = 0;
  return released;
}

void XrPresenter::InvalidateFrames() {
  std::lock_guard<std::mutex> upload_lock(upload_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  if (has_pending_ && pending_.slot >= 0) released_ |= 1u << pending_.slot;
  if (current_.slot >= 0) released_ |= 1u << current_.slot;
  has_pending_ = false;
  pending_ = Frame();
  current_ = Frame();
  // Shared textures may be freed next: the XR thread drops its handles.
  shared_generation_.fetch_add(1);
}

void XrPresenter::PublishWindowViews(const EyeView* views, bool valid, int64_t pose_time) {
  {
    std::lock_guard<std::mutex> lock(eyes_mutex_);
    window_views_valid_ = valid && views;
    if (views) {
      window_views_[0] = views[0];
      window_views_[1] = views[1];
    }
    window_views_time_ = pose_time;
    ++tick_;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (last_tick_qpc_) {
      const double ms = 1000.0 * static_cast<double>(now.QuadPart - last_tick_qpc_) / freq.QuadPart;
      if (ms > 0.0 && ms < 100.0) {
        const double old = tick_period_ms_.load();
        tick_period_ms_.store(old > 0.0 ? old * 0.95 + ms * 0.05 : ms);
      }
    }
    last_tick_qpc_ = now.QuadPart;
  }
  tick_cv_.notify_all();
}

int XrPresenter::WaitForFrameTick(uint64_t* last_tick, int timeout_ms) {
  std::unique_lock<std::mutex> lock(eyes_mutex_);
  const bool ticked = tick_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                        [&] { return tick_ != *last_tick; });
  const uint64_t passed = ticked ? tick_ - *last_tick : 0;
  *last_tick = tick_;
  return static_cast<int>(passed > 1000 ? 1000 : passed);
}

bool XrPresenter::GetWindowViews(EyeView views[2], int64_t* pose_time) const {
  std::lock_guard<std::mutex> lock(eyes_mutex_);
  views[0] = window_views_[0];
  views[1] = window_views_[1];
  if (pose_time) *pose_time = window_views_time_;
  return window_views_valid_;
}

}  // namespace kkvr

#include "xr_thread.h"

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

// clang-format off
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// clang-format on

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "camera.h"
#include "config.h"
#include "log.h"
#include "shared.h"
#include "timing.h"

SharedState g_shared;
XrViewsShared g_xrViews;

namespace {

XrInstance g_instance = XR_NULL_HANDLE;

bool Ok(XrResult r, const char* what) {
  if (XR_SUCCEEDED(r)) return true;
  char name[XR_MAX_RESULT_STRING_SIZE] = {};
  if (g_instance) xrResultToString(g_instance, r, name);
  Log("OpenXR %s failed: %s (%d)", what, name[0] ? name : "?", r);
  return false;
}

// Creates the D3D11 device on the GPU the runtime asked for. Only ever done once.
ID3D11Device* CreateDevice(LUID luid, D3D_FEATURE_LEVEL minLevel) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { Log("CreateDXGIFactory1 failed"); return nullptr; }
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d;
    adapter->GetDesc1(&d);
    if (memcmp(&d.AdapterLuid, &luid, sizeof(LUID)) == 0) {
      Log("OpenXR adapter: %ls", d.Description);
      break;
    }
    adapter->Release();
    adapter = nullptr;
  }
  factory->Release();
  if (!adapter) { Log("adapter with the runtime's LUID not found"); return nullptr; }

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
  adapter->Release();
  if (FAILED(hr)) { Log("D3D11CreateDevice failed: 0x%08lx", hr); return nullptr; }
  if (dev->GetFeatureLevel() < minLevel) Log("warning: feature level below runtime minimum");

  // Zwift's render thread creates textures + runs NV_DX_interop on this device while we
  // use its immediate context here.
  ID3D11Multithread* mt = nullptr;
  if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&mt)))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
  ctx->Release();
  return dev;
}

DXGI_FORMAT StripSrgb(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return f;
  }
}

// ---------------- phase 2: stereo from depth ----------------
// The virtual screen acts as a window. A scene point at view depth z gets a per-eye horizontal
// shift d(z) = maxShift * (1 - conv/z), clamped to +-maxShift, where maxShift is half the IPD in
// screen pixels: at infinity the eyes are an IPD apart (reads as infinitely far), at the
// convergence distance d = 0 (on the screen), nearer pops out. Each output pixel solves
// xs + sign*d(xs) = x for its source column by fixed-point iteration (shifts are small: ~31 px).
// Output is side by side: left eye in [0, W), right eye in [W, 2W).
const char kStereoHlsl[] = R"(
cbuffer P : register(b0) { uint W; uint H; float maxShift; float convCm; float skyCm; float eyeSign; float2 pad; };
Texture2D<float4> Color : register(t0);   // top-down (D3D order)
Texture2D<float>  Depth : register(t1);   // linear view depth in cm, bottom-up (GL order)
RWTexture2D<float4> Out : register(u0);

float Disp(float x, uint y) {
  int xi = clamp((int)round(x), 0, (int)W - 1);
  float z = Depth.Load(int3(xi, H - 1 - y, 0));
  float invz = (z <= 0.0 || z >= skyCm) ? 0.0 : 1.0 / z;
  return clamp(maxShift * (1.0 - convCm * invz), -maxShift, maxShift);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= 2 * W || id.y >= H) return;
  uint eye = id.x >= W ? 1 : 0;
  float ox = (float)(id.x - eye * W);
  float s = (eye == 0 ? -1.0 : 1.0) * eyeSign;  // left eye: content shifted left by d (eyeSign -1 swaps)
  float xs = ox;
  [unroll] for (int i = 0; i < 4; ++i) xs = ox - s * Disp(xs, id.y);
  xs = clamp(xs, 0.0, (float)(W - 1));
  int x0 = (int)floor(xs);
  int x1 = min(x0 + 1, (int)W - 1);
  float4 c = lerp(Color.Load(int3(x0, id.y, 0)), Color.Load(int3(x1, id.y, 0)), xs - x0);
  Out[id.xy] = float4(c.rgb, 1.0);
}
)";

struct StereoParams {
  uint32_t W, H;
  float maxShift, convCm, skyCm, eyeSign, pad[2];
};

// Live tuning from the keyboard (polled on the XR thread; works whichever window has focus).
struct LiveTuning {
  float strength = 1.0f;
  bool swapEyes = false;
  bool f9 = false, up = false, down = false;  // previous key states for edge detection

  void Poll() {
    const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    const bool k9 = mods && (GetAsyncKeyState(VK_F9) & 0x8000);
    const bool ku = mods && (GetAsyncKeyState(VK_UP) & 0x8000);
    const bool kd = mods && (GetAsyncKeyState(VK_DOWN) & 0x8000);
    if (k9 && !f9) { swapEyes = !swapEyes; Log("Ctrl+Shift+F9: eyes %s", swapEyes ? "SWAPPED" : "normal"); }
    if (ku && !up) { strength = std::min(2.0f, strength + 0.1f); Log("Ctrl+Shift+Up: strength %.1f", strength); }
    if (kd && !down) { strength = std::max(0.0f, strength - 0.1f); Log("Ctrl+Shift+Down: strength %.1f", strength); }
    f9 = k9;
    up = ku;
    down = kd;
  }
};
LiveTuning g_live;
Stat g_xrLockWait, g_xrLockHold;
static_assert(sizeof(StereoParams) == 32);

template <class T>
void SafeRelease(T*& p) {
  if (p) { p->Release(); p = nullptr; }
}

struct Stereo {
  ID3D11ComputeShader* cs = nullptr;  // compiled once per device, kept forever
  ID3D11Buffer* cb = nullptr;
  // per capture generation
  uint64_t generation = ~0ull;
  ID3D11Texture2D* out = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  ID3D11ShaderResourceView *colorSrv = nullptr, *depthSrv = nullptr;
  unsigned framesStereo = 0, framesFlat = 0;

  void ReleasePerGen() {
    SafeRelease(out);
    SafeRelease(uav);
    SafeRelease(colorSrv);
    SafeRelease(depthSrv);
    generation = ~0ull;
  }
};
Stereo g_stereo;  // lives as long as the D3D11 device (the whole process)

bool EnsureStereoShader(ID3D11Device* dev) {
  if (g_stereo.cs) return true;
  ID3DBlob *code = nullptr, *errors = nullptr;
  HRESULT hr = D3DCompile(kStereoHlsl, sizeof(kStereoHlsl) - 1, "stereo.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(hr)) {
    Log("stereo shader compile failed: 0x%08lx %s", hr, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    SafeRelease(errors);
    return false;
  }
  SafeRelease(errors);
  hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_stereo.cs);
  code->Release();
  if (FAILED(hr)) { Log("CreateComputeShader failed: 0x%08lx", hr); return false; }
  D3D11_BUFFER_DESC bd{sizeof(StereoParams), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_stereo.cb))) { Log("stereo constant buffer failed"); return false; }
  return true;
}

// Called with g_shared.m held (reads g_shared.tex / depthTex).
bool EnsureStereoResources(ID3D11Device* dev, uint64_t gen, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
  if (g_stereo.generation == gen) return g_stereo.out != nullptr;
  g_stereo.ReleasePerGen();
  g_stereo.generation = gen;
  if (!g_shared.tex || !g_shared.depthTex) return false;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w * 2;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_stereo.out)) &&
            SUCCEEDED(dev->CreateUnorderedAccessView(g_stereo.out, nullptr, &g_stereo.uav)) &&
            SUCCEEDED(dev->CreateShaderResourceView(g_shared.tex, nullptr, &g_stereo.colorSrv)) &&
            SUCCEEDED(dev->CreateShaderResourceView(g_shared.depthTex, nullptr, &g_stereo.depthSrv));
  if (!ok) {
    Log("stereo resources %ux%u failed - flat screen", w * 2, h);
    g_stereo.ReleasePerGen();
    g_stereo.generation = gen;  // don't retry every frame
  }
  return ok;
}

// ---------------- phase 3: immersive (alternate-eye rendering) ----------------
// Latest captured image per eye + the pose/FOV the camera override rendered it with.
struct EyeImages {
  uint64_t generation = ~0ull;
  ID3D11Texture2D* tex[2] = {};
  XrPosef pose[2]{};
  XrFovf fov[2]{};
  FrameInfo info[2];
  bool have[2] = {};
  uint64_t lastFrame = ~0ull;
  unsigned updates[2] = {}, coherentUpdates = 0;
  int lastEye = -1;                 // eye of the previous new image (alternation check)
  unsigned nonAlternating = 0;      // the same eye updated twice in a row (a frame was missed)
  uint64_t maxFrameGap = 0;         // max |frameNo L - frameNo R| of the displayed pair
};
EyeImages g_eyes;

// Crop one eye's projection view to the part of the rendered image the headset can actually show (the
// runtime's own eye FOV), so the runtime never gets a layer wider than its display. The FOV is rebuilt
// from the integer pixel edges, so pixels and angles stay exactly consistent.
void FillView(XrCompositionLayerProjectionView& v, int i, int32_t W, int32_t H, XrSwapchain sc, const XrFovf& r,
              const XrFovf* eye) {
  v.fov = r;
  v.subImage.swapchain = sc;
  v.subImage.imageArrayIndex = static_cast<uint32_t>(i);  // one array slice per eye
  v.subImage.imageRect = {{0, 0}, {W, H}};
  if (!eye) return;
  const double tL = std::tan(r.angleLeft), tR = std::tan(r.angleRight), tU = std::tan(r.angleUp),
               tD = std::tan(r.angleDown);
  if (tR - tL < 1e-6 || tU - tD < 1e-6) return;
  const double eL = std::tan(std::max(eye->angleLeft, r.angleLeft)), eR = std::tan(std::min(eye->angleRight, r.angleRight));
  const double eU = std::tan(std::min(eye->angleUp, r.angleUp)), eD = std::tan(std::max(eye->angleDown, r.angleDown));
  const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(W * (eL - tL) / (tR - tL))), 0, W);
  const int32_t x1 = std::clamp(static_cast<int32_t>(std::ceil(W * (eR - tL) / (tR - tL))), 0, W);
  const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(H * (tU - eU) / (tU - tD))), 0, H);  // row 0 = up
  const int32_t y1 = std::clamp(static_cast<int32_t>(std::ceil(H * (tU - eD) / (tU - tD))), 0, H);
  if (x1 - x0 < 16 || y1 - y0 < 16) return;
  v.fov.angleLeft = static_cast<float>(std::atan(tL + (tR - tL) * x0 / W));
  v.fov.angleRight = static_cast<float>(std::atan(tL + (tR - tL) * x1 / W));
  v.fov.angleUp = static_cast<float>(std::atan(tU - (tU - tD) * y0 / H));
  v.fov.angleDown = static_cast<float>(std::atan(tU - (tU - tD) * y1 / H));
  v.subImage.imageRect = {{x0, y0}, {x1 - x0, y1 - y0}};
}

// [hud] spread: Ctrl+Shift+] pushes every card further from straight ahead, Ctrl+Shift+[ pulls them in.
float g_hudSpread = 1.0f;
bool g_spreadInWas = false, g_spreadOutWas = false;
void PollSpreadKeys() {
  const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
  const bool in = mods && (GetAsyncKeyState(VK_OEM_4) & 0x8000), out = mods && (GetAsyncKeyState(VK_OEM_6) & 0x8000);
  if ((in && !g_spreadInWas) || (out && !g_spreadOutWas)) {
    g_hudSpread = std::clamp(std::round((g_hudSpread + (out ? 0.1f : -0.1f)) * 10.0f) / 10.0f, 0.5f, 2.5f);
    Log("Ctrl+Shift+%s: HUD cards spread %.1f (to keep it: [hud] spread = %.1f)", out ? "]" : "[", g_hudSpread,
        g_hudSpread);
  }
  g_spreadInWas = in;
  g_spreadOutWas = out;
}

bool g_cropKeyWasDown = false;
void PollCropKey() {
  const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                    (GetAsyncKeyState(VK_INSERT) & 0x8000);
  if (down && !g_cropKeyWasDown) {
    const bool on = !g_xrViews.crop.load();
    g_xrViews.crop.store(on);
    Log("Ctrl+Shift+Insert: crop to the headset's eye FOV %s", on ? "ON" : "OFF (full rendered image submitted)");
  }
  g_cropKeyWasDown = down;
}

// Ctrl+Shift+F7: save both eye images (exactly what goes to the headset) + their poses/FOVs to
// %LOCALAPPDATA%\zwift-vr\eyes-HHMMSS-{L,R}-WxH.rgba (raw RGBA8 rows) and a .txt; convert with
// tools/eyes_to_png.py. Diagnoses "the eyes don't merge" by looking at the actual pixels.
bool g_dumpKeyWasDown = false;
bool g_dumpRequested = false;

void PollDumpKey() {
  const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                    (GetAsyncKeyState(VK_F7) & 0x8000);
  if (down && !g_dumpKeyWasDown) g_dumpRequested = true;
  g_dumpKeyWasDown = down;
}

// Called with g_shared.m held, after both eye textures are valid. `views` = what is submitted this frame
// (after the crop), `rt` = the runtime's eye FOVs (null if unknown).
void DumpEyes(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
              const XrCompositionLayerProjectionView* views, const XrFovf* rt) {
  wchar_t dir[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t base[MAX_PATH];
  swprintf_s(base, L"%s\\zwift-vr\\eyes-%02d%02d%02d", dir, t.wHour, t.wMinute, t.wSecond);

  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_STAGING;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (int e = 0; e < 2; ++e) {
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) { Log("eye dump: staging failed"); return; }
    ctx->CopyResource(staging, g_eyes.tex[e]);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
      wchar_t path[MAX_PATH];
      swprintf_s(path, L"%s-%c-%ux%u.rgba", base, e == 0 ? L'L' : L'R', w, h);
      FILE* f = nullptr;
      if (!_wfopen_s(&f, path, L"wb") && f) {
        for (uint32_t y = 0; y < h; ++y) fwrite(static_cast<const uint8_t*>(map.pData) + y * map.RowPitch, 4, w, f);
        fclose(f);
      }
      ctx->Unmap(staging, 0);
    }
    staging->Release();
  }
  wchar_t txt[MAX_PATH];
  swprintf_s(txt, L"%s.txt", base);
  FILE* f = nullptr;
  if (!_wfopen_s(&f, txt, L"w") && f) {
    fprintf(f, "format DXGI %d (%s)\n", fmt, fmt == DXGI_FORMAT_B8G8R8A8_UNORM ? "BGRA" : "RGBA");
    for (int e = 0; e < 2; ++e) {
      const XrPosef& p = g_eyes.pose[e];
      const XrFovf& v = g_eyes.fov[e];
      const double kDeg = 180.0 / 3.14159265358979;
      fprintf(f, "%s pos (%.4f %.4f %.4f) m  quat (%.4f %.4f %.4f %.4f)  fov L %.2f R %.2f U %.2f D %.2f deg\n",
              e == 0 ? "LEFT " : "RIGHT", p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y,
              p.orientation.z, p.orientation.w, v.angleLeft * kDeg, v.angleRight * kDeg, v.angleUp * kDeg,
              v.angleDown * kDeg);
    }
    static const char* kModes[] = {"STEREO", "MONO", "COHERENT"};
    for (int e = 0; e < 2; ++e) {
      const FrameInfo& in = g_eyes.info[e];
      const XrCompositionLayerProjectionView& sv = views[e];
      const double kDeg = 180.0 / 3.14159265358979;
      fprintf(f, "%s frame %llu mode %s parallax %.2f | submitted rect x %d y %d %dx%d fov L %.2f R %.2f U %.2f D %.2f",
              e == 0 ? "LEFT " : "RIGHT", static_cast<unsigned long long>(in.frameNo),
              kModes[std::clamp(in.mode, 0, 2)], in.parallax, sv.subImage.imageRect.offset.x,
              sv.subImage.imageRect.offset.y, sv.subImage.imageRect.extent.width, sv.subImage.imageRect.extent.height,
              sv.fov.angleLeft * kDeg, sv.fov.angleRight * kDeg, sv.fov.angleUp * kDeg, sv.fov.angleDown * kDeg);
      if (rt)
        fprintf(f, " | runtime eye fov L %.2f R %.2f U %.2f D %.2f", rt[e].angleLeft * kDeg, rt[e].angleRight * kDeg,
                rt[e].angleUp * kDeg, rt[e].angleDown * kDeg);
      if (in.haveCam)
        fprintf(f, " | Zwift camera (%.1f %.1f %.1f) cm fwd (%.4f %.4f %.4f)", in.zwiftCam[0], in.zwiftCam[1],
                in.zwiftCam[2], in.zwiftFwd[0], in.zwiftFwd[1], in.zwiftFwd[2]);
      fprintf(f, "\n");
    }
    const FrameInfo &a = g_eyes.info[0], &b = g_eyes.info[1];
    if (a.haveCam && b.haveCam) {
      const float dx = a.zwiftCam[0] - b.zwiftCam[0], dy = a.zwiftCam[1] - b.zwiftCam[1], dz = a.zwiftCam[2] - b.zwiftCam[2];
      const float dot = std::clamp(a.zwiftFwd[0] * b.zwiftFwd[0] + a.zwiftFwd[1] * b.zwiftFwd[1] + a.zwiftFwd[2] * b.zwiftFwd[2], -1.0f, 1.0f);
      fprintf(f, "pair: Zwift camera %.2f cm apart, forward %.3f deg apart, %lld frames apart\n",
              std::sqrt(dx * dx + dy * dy + dz * dz), std::acos(dot) * 57.29578,
              static_cast<long long>(a.frameNo) - static_cast<long long>(b.frameNo));
    }
    fclose(f);
  }
  Log("eye dump: wrote %ls-{L,R}-%ux%u.rgba + .txt", base, w, h);
}

// Called with g_shared.m held.
bool EnsureEyeTextures(ID3D11Device* dev, uint64_t gen, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
  if (g_eyes.generation == gen) return g_eyes.tex[0] && g_eyes.tex[1];
  for (auto*& t : g_eyes.tex) SafeRelease(t);
  g_eyes.have[0] = g_eyes.have[1] = false;
  g_eyes.generation = gen;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  for (auto*& t : g_eyes.tex) {
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) {
      Log("immersive: eye texture %ux%u failed", w, h);
      for (auto*& u : g_eyes.tex) SafeRelease(u);
      return false;
    }
  }
  Log("immersive: eye images %ux%u", w, h);
  return true;
}

struct Swapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageD3D11KHR> images;
  uint32_t w = 0, h = 0;
  uint64_t generation = ~0ull;
  uint32_t srcW = 0, srcH = 0;  // the capture (eye image) size it was made for; w/h smaller = filtered down
  bool released = false;  // an image was released at least once (a layer may reference it)

  void Destroy() {
    if (handle) xrDestroySwapchain(handle);
    *this = Swapchain{};
  }
};

// ---------------- [hud]: Zwift's HUD as flat, world-locked cards ----------------
// The render thread hands over two images of the same immersive Zwift frame: the world (snapshotted just before
// Zwift's HUD drew into the window) and the finished frame. The HUD is exactly what differs between them: keep
// those pixels (3x3 max, so thin text and panel edges stay solid) and make the rest transparent. Translucent HUD
// panels keep a dimmed copy of the world behind them, which reads as the panel's own background.
const char kHudHlsl[] = R"(
cbuffer P : register(b0) { uint W; uint H; float lo; float hi; uint useAlpha; uint3 pad; };
Texture2D<float4> Final : register(t0);
Texture2D<float4> World : register(t1);
RWTexture2D<float4> Out : register(u0);

float Diff(int2 p) {
  p = clamp(p, int2(0, 0), int2((int)W - 1, (int)H - 1));
  float3 d = abs(Final.Load(int3(p, 0)).rgb - World.Load(int3(p, 0)).rgb);
  return max(d.r, max(d.g, d.b));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= W || id.y >= H) return;
  float m = 0.0;
  [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) m = max(m, Diff(int2(id.xy) + int2(x, y)));
  float4 f = Final.Load(int3(id.xy, 0));
  float d = saturate((m - lo) / (hi - lo));
  if (useAlpha != 0 && f.a > 0.004) {
    // The window's alpha was zeroed before the HUD, so f.a = HUD coverage and f.rgb = HUD over world:
    // take the world back out -> the panel's own colour, no scenery moving inside it.
    float3 w = World.Load(int3(id.xy, 0)).rgb;
    Out[id.xy] = float4(saturate((f.rgb - (1.0 - f.a) * w) / f.a), f.a);
  } else {
    Out[id.xy] = float4(f.rgb, d);  // no coverage written here: fall back to the difference mask
  }
}
)";

struct HudParams {
  uint32_t W, H;
  float lo, hi;  // colour difference (0..1) below lo = transparent, above hi = opaque
  uint32_t useAlpha, pad[3];
};
static_assert(sizeof(HudParams) == 32);

struct Hud {
  ID3D11ComputeShader* cs = nullptr;  // once per device
  ID3D11Buffer* cb = nullptr;
  // per capture generation
  uint64_t generation = ~0ull;
  uint32_t w = 0, h = 0;
  ID3D11Texture2D* out = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  ID3D11ShaderResourceView *finalSrv = nullptr, *worldSrv = nullptr;
  bool have = false;                  // `out` holds a HUD image
  uint64_t updates = 0, copied = 0;   // HUD images made / last one copied into the HUD swapchain
  unsigned updatesInWindow = 0;

  void ReleasePerGen() {
    SafeRelease(out);
    SafeRelease(uav);
    SafeRelease(finalSrv);
    SafeRelease(worldSrv);
    generation = ~0ull;
    have = false;
  }
};
Hud g_hud;

bool EnsureHudShader(ID3D11Device* dev) {
  if (g_hud.cs) return true;
  ID3DBlob *code = nullptr, *errors = nullptr;
  HRESULT hr = D3DCompile(kHudHlsl, sizeof(kHudHlsl) - 1, "hud.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(hr)) {
    Log("hud shader compile failed: 0x%08lx %s", hr, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    SafeRelease(errors);
    return false;
  }
  SafeRelease(errors);
  hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_hud.cs);
  code->Release();
  if (FAILED(hr)) { Log("hud CreateComputeShader failed: 0x%08lx", hr); return false; }
  D3D11_BUFFER_DESC bd{sizeof(HudParams), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_hud.cb))) { Log("hud constant buffer failed"); return false; }
  return true;
}

// Called with g_shared.m held (reads g_shared.tex / hudTex).
bool EnsureHudResources(ID3D11Device* dev, uint64_t gen, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
  if (g_hud.generation == gen) return g_hud.out != nullptr;
  g_hud.ReleasePerGen();
  g_hud.generation = gen;
  if (!g_shared.tex || !g_shared.hudTex) return false;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_hud.out)) &&
            SUCCEEDED(dev->CreateUnorderedAccessView(g_hud.out, nullptr, &g_hud.uav)) &&
            SUCCEEDED(dev->CreateShaderResourceView(g_shared.hudTex, nullptr, &g_hud.finalSrv)) &&
            SUCCEEDED(dev->CreateShaderResourceView(g_shared.tex, nullptr, &g_hud.worldSrv));
  if (!ok) {
    Log("hud resources %ux%u failed - no HUD cards", w, h);
    g_hud.ReleasePerGen();
    g_hud.generation = gen;  // don't retry every frame
    return false;
  }
  g_hud.w = w;
  g_hud.h = h;
  return true;
}

// Called with g_shared.m held, right after a new immersive frame with hudValid arrived.
void MakeHud(ID3D11DeviceContext* ctx) {
  const HudParams p{g_hud.w, g_hud.h, 0.02f, 0.08f, g_shared.hudAlpha.load() ? 1u : 0u, {}};
  ctx->UpdateSubresource(g_hud.cb, 0, nullptr, &p, 0, 0);
  ID3D11ShaderResourceView* srvs[2] = {g_hud.finalSrv, g_hud.worldSrv};
  ctx->CSSetShader(g_hud.cs, nullptr, 0);
  ctx->CSSetConstantBuffers(0, 1, &g_hud.cb);
  ctx->CSSetShaderResources(0, 2, srvs);
  ctx->CSSetUnorderedAccessViews(0, 1, &g_hud.uav, nullptr);
  ctx->Dispatch((g_hud.w + 7) / 8, (g_hud.h + 7) / 8, 1);
  ID3D11ShaderResourceView* noSrv[2] = {};
  ID3D11UnorderedAccessView* noUav = nullptr;
  ctx->CSSetShaderResources(0, 2, noSrv);
  ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
  ctx->CSSetShader(nullptr, nullptr, 0);
  g_hud.have = true;
  g_hud.updates++;
  g_hud.updatesInWindow++;
}

// With the Ctrl+Shift+F7 eye dump: the cut-out HUD (RGBA8 rows, alpha = card transparency) as
// %LOCALAPPDATA%\zwift-vr\hud-HHMMSS-WxH.rgba - measure card rects ([hud] cardN) on it, as fractions of W and H.
void DumpHud(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  if (!g_hud.have || !g_hud.out) return;
  wchar_t dir[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
  D3D11_TEXTURE2D_DESC d{};
  g_hud.out->GetDesc(&d);
  d.BindFlags = 0;
  d.Usage = D3D11_USAGE_STAGING;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* staging = nullptr;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) { Log("hud dump: staging failed"); return; }
  ctx->CopyResource(staging, g_hud.out);
  D3D11_MAPPED_SUBRESOURCE map{};
  if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\zwift-vr\\hud-%02d%02d%02d-%ux%u.rgba", dir, t.wHour, t.wMinute, t.wSecond, d.Width, d.Height);
    FILE* f = nullptr;
    if (!_wfopen_s(&f, path, L"wb") && f) {
      for (uint32_t y = 0; y < d.Height; ++y) fwrite(static_cast<const uint8_t*>(map.pData) + y * map.RowPitch, 4, d.Width, f);
      fclose(f);
      Log("hud dump: %ls", path);
    }
    ctx->Unmap(staging, 0);
  }
  staging->Release();
}

// ---------------- [vr] eye_resolution: eye images filtered down to the headset's own resolution ----------------
// A big Zwift window gives eye images far denser than the headset (3840 px over ~108 deg vs a Quest 3's ~2000 over
// its eye FOV). The runtime shrinks a projection layer with plain bilinear sampling, which aliases: distant detail
// shimmers. So the XR thread shrinks each new eye image itself, with a 4x4-tap box filter (each tap bilinear, so ~8x8
// texels), to the runtime's recommended size x eye_resolution, before it goes into the swapchain.
const char kScaleHlsl[] = R"(
cbuffer P : register(b0) { uint dw; uint dh; float2 spread; };  // spread 0.25 = shrunk axis, 0 = 1:1 axis
Texture2D<float4> Src : register(t0);
SamplerState Lin : register(s0);
RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= dw || id.y >= dh) return;
  float2 px = 1.0 / float2(dw, dh);
  float2 c = (float2(id.xy) + 0.5) * px;
  float4 acc = 0;
  [unroll] for (int j = 0; j < 4; ++j)
    [unroll] for (int i = 0; i < 4; ++i) acc += Src.SampleLevel(Lin, c + (float2(i, j) - 1.5) * spread * px, 0);
  Out[id.xy] = acc * (1.0 / 16.0);
}
)";

struct ScaleParams {
  uint32_t dw, dh;
  float spread[2];  // per axis: taps 1/4 output pixel apart where the image shrinks, all at the centre where it's 1:1
};
static_assert(sizeof(ScaleParams) == 16);

struct EyeScale {
  ID3D11ComputeShader* cs = nullptr;  // once per device
  ID3D11Buffer* cb = nullptr;
  ID3D11SamplerState* smp = nullptr;
  uint64_t generation = ~0ull;
  uint32_t w = 0, h = 0;  // scaled size
  ID3D11Texture2D* out[2] = {};
  ID3D11UnorderedAccessView* uav[2] = {};
  ID3D11Texture2D* srcTex[2] = {};  // the eye texture each srv was made for
  ID3D11ShaderResourceView* srv[2] = {};
  bool fresh[2] = {};  // out[e] holds the current eye image (false after (re)creation)
  unsigned scaledInWindow = 0;

  void ReleasePerSize() {
    for (int e = 0; e < 2; ++e) {
      SafeRelease(out[e]);
      SafeRelease(uav[e]);
      SafeRelease(srv[e]);
      srcTex[e] = nullptr;
      fresh[e] = false;
    }
    generation = ~0ull;
    w = h = 0;
  }
};
EyeScale g_scale;

bool EnsureScaleShader(ID3D11Device* dev) {
  if (g_scale.cs) return true;
  ID3DBlob *code = nullptr, *errors = nullptr;
  HRESULT hr = D3DCompile(kScaleHlsl, sizeof(kScaleHlsl) - 1, "scale.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(hr)) {
    Log("scale shader compile failed: 0x%08lx %s", hr, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    SafeRelease(errors);
    return false;
  }
  SafeRelease(errors);
  hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_scale.cs);
  code->Release();
  if (FAILED(hr)) { Log("scale CreateComputeShader failed: 0x%08lx", hr); return false; }
  D3D11_BUFFER_DESC bd{sizeof(ScaleParams), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_scale.cb)) || FAILED(dev->CreateSamplerState(&sd, &g_scale.smp))) {
    Log("scale constant buffer / sampler failed");
    return false;
  }
  return true;
}

bool EnsureScaleResources(ID3D11Device* dev, uint64_t gen, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
  if (g_scale.generation == gen && g_scale.w == w && g_scale.h == h) return g_scale.out[0] != nullptr;
  g_scale.ReleasePerSize();
  g_scale.generation = gen;
  g_scale.w = w;
  g_scale.h = h;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  for (int e = 0; e < 2; ++e) {
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_scale.out[e])) ||
        FAILED(dev->CreateUnorderedAccessView(g_scale.out[e], nullptr, &g_scale.uav[e]))) {
      Log("scaled eye textures %ux%u failed", w, h);
      g_scale.ReleasePerSize();
      g_scale.generation = gen;
      g_scale.w = w;
      g_scale.h = h;
      return false;
    }
  }
  return true;
}

// Filters eye image `src` (Zwift's size) into g_scale.out[e] (the swapchain's size).
void DownscaleEye(ID3D11Device* dev, ID3D11DeviceContext* ctx, int e, ID3D11Texture2D* src) {
  if (g_scale.srcTex[e] != src) {
    SafeRelease(g_scale.srv[e]);
    g_scale.srcTex[e] = src;
    if (FAILED(dev->CreateShaderResourceView(src, nullptr, &g_scale.srv[e]))) return;
  }
  if (!g_scale.srv[e] || !g_scale.uav[e]) return;
  D3D11_TEXTURE2D_DESC sd{};
  src->GetDesc(&sd);
  const ScaleParams p{g_scale.w, g_scale.h, {sd.Width > g_scale.w ? 0.25f : 0.0f, sd.Height > g_scale.h ? 0.25f : 0.0f}};
  ctx->UpdateSubresource(g_scale.cb, 0, nullptr, &p, 0, 0);
  ctx->CSSetShader(g_scale.cs, nullptr, 0);
  ctx->CSSetConstantBuffers(0, 1, &g_scale.cb);
  ctx->CSSetShaderResources(0, 1, &g_scale.srv[e]);
  ctx->CSSetSamplers(0, 1, &g_scale.smp);
  ctx->CSSetUnorderedAccessViews(0, 1, &g_scale.uav[e], nullptr);
  ctx->Dispatch((g_scale.w + 7) / 8, (g_scale.h + 7) / 8, 1);
  ID3D11ShaderResourceView* noSrv = nullptr;
  ID3D11UnorderedAccessView* noUav = nullptr;
  ctx->CSSetShaderResources(0, 1, &noSrv);
  ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
  ctx->CSSetShader(nullptr, nullptr, 0);
  g_scale.fresh[e] = true;
  g_scale.scaledInWindow++;
}

// Eye image size the headset can use: Zwift's pixels per tangent unit over the rendered frustum vs the runtime's
// recommended pixels over its own eye FOV, per axis, never upscaled, in 1/64 steps (so FOV jitter can't churn the
// swapchain). mult = [vr] eye_resolution (0 = off).
void ScaledEyeSize(uint32_t w, uint32_t h, const XrFovf& rendered, const XrFovf& eye, uint32_t recW, uint32_t recH,
                   float mult, uint32_t* ow, uint32_t* oh) {
  *ow = w;
  *oh = h;
  const double srcX = std::tan(rendered.angleRight) - std::tan(rendered.angleLeft);
  const double srcY = std::tan(rendered.angleUp) - std::tan(rendered.angleDown);
  const double eyeX = std::tan(eye.angleRight) - std::tan(eye.angleLeft);
  const double eyeY = std::tan(eye.angleUp) - std::tan(eye.angleDown);
  if (mult <= 0.0f || !recW || !recH || srcX < 1e-3 || srcY < 1e-3 || eyeX < 1e-3 || eyeY < 1e-3) return;
  auto factor = [](double want, double have) { return std::ceil(std::clamp(want / have, 0.25, 1.0) * 64.0) / 64.0; };
  const double sx = factor(recW * mult / eyeX, w / srcX), sy = factor(recH * mult / eyeY, h / srcY);
  *ow = std::max<uint32_t>(16, static_cast<uint32_t>(std::ceil(w * sx)));
  *oh = std::max<uint32_t>(16, static_cast<uint32_t>(std::ceil(h * sy)));
}

// Card pose: `home` + (right, up, -ahead), turned to face the home point (yaw about +Y, then pitch about +X).
// spread scales the card's angles from straight ahead (right/left, then up/down) at the same distance from you.
XrPosef CardPose(const float home[3], const Config::HudCard& k, float spread) {
  float px = k.px, py = k.py, pz = -k.dist;
  if (spread != 1.0f) {
    const float r = std::sqrt(px * px + py * py + pz * pz);
    const float az = std::atan2(px, -pz) * spread;
    const float el = std::clamp(std::atan2(py, std::sqrt(px * px + pz * pz)) * spread, -1.4f, 1.4f);
    px = r * std::cos(el) * std::sin(az);
    py = r * std::sin(el);
    pz = -r * std::cos(el) * std::cos(az);
  }
  const float yaw = std::atan2(-px, -pz);                          // right of you -> turned left, toward you
  const float pitch = std::atan2(py, std::sqrt(px * px + pz * pz));  // below eye level -> tilted up, toward you
  const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f), cp = std::cos(pitch * 0.5f),
              sp = std::sin(pitch * 0.5f);
  XrPosef pose{};
  pose.orientation = {cy * sp, sy * cp, -sy * sp, cy * cp};  // q_yaw * q_pitch
  pose.position = {home[0] + px, home[1] + py, home[2] + pz};
  return pose;
}

// One instance+session lifetime. Returns when the session/instance is lost or can't start.
void RunInstance(const Config& cfg) {
  // --- instance ---
  uint32_t extCount = 0;
  if (!Ok(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr), "enumerate extensions")) return;
  std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
  bool hasD3D11 = false;
  for (auto& e : exts) hasD3D11 |= strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
  if (!hasD3D11) { Log("active OpenXR runtime has no %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME); return; }

  const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
  XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
  strcpy_s(ici.applicationInfo.applicationName, "zwift-vr");
  strcpy_s(ici.applicationInfo.engineName, "zwift-vr");
  ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  ici.enabledExtensionCount = 1;
  ici.enabledExtensionNames = enabled;
  if (!Ok(xrCreateInstance(&ici, &g_instance), "xrCreateInstance")) return;

  XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
  xrGetInstanceProperties(g_instance, &ip);
  Log("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
      XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

  XrSession session = XR_NULL_HANDLE;
  XrSpace space = XR_NULL_HANDLE;
  Swapchain sc;
  Swapchain hudSc;  // [hud]: the cut-out HUD; every card is a quad layer over a part of it
  auto cleanup = [&] {
    sc.Destroy();
    hudSc.Destroy();
    if (space) xrDestroySpace(space);
    if (session) xrDestroySession(session);
    xrDestroyInstance(g_instance);
    g_instance = XR_NULL_HANDLE;
  };

  // --- system (headset); wait for it to be connected ---
  XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
  sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  for (bool logged = false;;) {
    XrResult r = xrGetSystem(g_instance, &sgi, &system);
    if (XR_SUCCEEDED(r)) break;
    if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE) { Ok(r, "xrGetSystem"); cleanup(); return; }
    if (!logged) { Log("headset not available yet - waiting"); logged = true; }
    Sleep(2000);
  }
  XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
  xrGetSystemProperties(g_instance, system, &sp);
  Log("headset: %s (max swapchain image %ux%u)", sp.systemName, sp.graphicsProperties.maxSwapchainImageWidth,
      sp.graphicsProperties.maxSwapchainImageHeight);
  uint32_t recW = 0, recH = 0;  // the runtime's recommended eye image size (Link: its render-resolution setting)
  {
    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t nv = 0;
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                       2, &nv, vcv)) && nv >= 1) {
      recW = vcv[0].recommendedImageRectWidth;
      recH = vcv[0].recommendedImageRectHeight;
      Log("headset recommends %ux%u per eye; [vr] eye_resolution %.2f%s", recW, recH, cfg.eyeResolution,
          cfg.eyeResolution > 0.0f ? " - immersive eye images are filtered down to that" : " (off: Zwift's pixels as-is)");
    }
  }

  // --- D3D11 device on the runtime's adapter ---
  PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
  xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                        reinterpret_cast<PFN_xrVoidFunction*>(&getReqs));
  XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  if (!getReqs || !Ok(getReqs(g_instance, system, &reqs), "xrGetD3D11GraphicsRequirementsKHR")) { cleanup(); return; }

  static LUID deviceLuid{};
  ID3D11Device* dev = g_shared.device.load();
  if (!dev) {
    dev = CreateDevice(reqs.adapterLuid, reqs.minFeatureLevel);
    if (!dev) { cleanup(); return; }
    deviceLuid = reqs.adapterLuid;
    g_shared.device.store(dev);
  } else if (memcmp(&deviceLuid, &reqs.adapterLuid, sizeof(LUID)) != 0) {
    Log("runtime now wants a different GPU than our D3D11 device - restart Zwift");
  }
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);

  // --- session ---
  XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
  binding.device = dev;
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &binding;
  sci.systemId = system;
  if (!Ok(xrCreateSession(g_instance, &sci, &session), "xrCreateSession")) { ctx->Release(); cleanup(); return; }

  XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  rsci.poseInReferenceSpace.orientation.w = 1.0f;
  if (!Ok(xrCreateReferenceSpace(session, &rsci, &space), "xrCreateReferenceSpace")) { ctx->Release(); cleanup(); return; }

  // --- swapchain format: an 8-bit sRGB format, so Zwift's gamma-encoded pixels display as-is ---
  uint32_t nf = 0;
  xrEnumerateSwapchainFormats(session, 0, &nf, nullptr);
  std::vector<int64_t> formats(nf);
  xrEnumerateSwapchainFormats(session, nf, &nf, formats.data());
  int64_t scFormat = 0;
  for (DXGI_FORMAT want : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                           DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}) {
    for (int64_t f : formats) if (f == want) { scFormat = f; break; }
    if (scFormat) break;
  }
  if (!scFormat) { Log("runtime offers no 8-bit RGBA/BGRA swapchain format"); ctx->Release(); cleanup(); return; }
  const DXGI_FORMAT sharedFmt = StripSrgb(static_cast<DXGI_FORMAT>(scFormat));
  // [hud] needs a typed RGBA8 UAV for the cut-out shader. Decided before the render thread can start capturing
  // (it waits for sharedFormat), so it never strips the HUD from the eyes when no cards can be shown.
  bool hudMode = cfg.hud;
  if (hudMode && sharedFmt != DXGI_FORMAT_R8G8B8A8_UNORM) {
    Log("hud: swapchain format %lld has no guaranteed UAV support - HUD stays in the eye images", scFormat);
    hudMode = false;
  }
  if (hudMode && !EnsureHudShader(dev)) hudMode = false;
  if (cfg.hud && !hudMode) {
    g_shared.hudOn.store(false);
    g_shared.hudConfigured.store(false);
  }
  if (hudMode) Log("hud: %d card(s); Ctrl+Shift+F9 toggles", cfg.hudCardCount);
  const bool scaleMode = cfg.eyeResolution > 0.0f && recW && sharedFmt == DXGI_FORMAT_R8G8B8A8_UNORM &&
                         EnsureScaleShader(dev);
  g_shared.sharedFormat.store(sharedFmt);
  Log("swapchain format: DXGI %lld", scFormat);

  // Stereo needs a typed UAV of the shared format, which D3D11 guarantees for RGBA8 but not BGRA8.
  bool stereoMode = cfg.stereo;
  if (stereoMode && sharedFmt != DXGI_FORMAT_R8G8B8A8_UNORM) {
    Log("stereo: swapchain format %lld has no guaranteed UAV support - flat screen", scFormat);
    stereoMode = false;
  }
  if (stereoMode && !EnsureStereoShader(dev)) stereoMode = false;
  g_live.strength = cfg.strength;
  g_hudSpread = cfg.hudSpread;
  Log("mode: %s", stereoMode ? "stereo (side-by-side swapchain, per-eye quad layers)" : "flat");

  uint32_t nb = 0;
  xrEnumerateEnvironmentBlendModes(g_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nb, nullptr);
  std::vector<XrEnvironmentBlendMode> blends(nb);
  xrEnumerateEnvironmentBlendModes(g_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nb, &nb, blends.data());
  const XrEnvironmentBlendMode blend = nb ? blends[0] : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

  // --- frame loop ---
  bool running = false, quit = false;
  unsigned submitted = 0, submittedImmersive = 0;
  ULONGLONG statsSince = GetTickCount64();
  g_xrViews.crop.store(cfg.cropToEyeFov);
  XrFovf rtFov[2]{};  // the runtime's own eye FOVs (latest xrLocateViews)
  bool haveRtFov = false;
  XrRect2Di loggedRect[2]{};
  while (!quit) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
      if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
        // The user recentred (Quest: hold the Meta button). LOCAL moves at changeTime; re-home the eye origin
        // shortly after, once the new space is in effect.
        auto* e = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
        Log("reference space %d change pending (runtime recentre), pose valid %d, previous-space pose (%.2f %.2f %.2f) m",
            e->referenceSpaceType, e->poseValid, e->poseInPreviousSpace.position.x, e->poseInPreviousSpace.position.y,
            e->poseInPreviousSpace.position.z);
        g_xrViews.recentreAtTick.store(GetTickCount64() + 500);
      } else if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
        Log("session state -> %d", e->state);
        // Headset put on (proximity sensor) or dash closed: re-home the eye origin once the user has settled.
        if (e->state == XR_SESSION_STATE_FOCUSED) g_xrViews.recentreAtTick.store(GetTickCount64() + 1500);
        switch (e->state) {
          case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
            bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            running = Ok(xrBeginSession(session, &bi), "xrBeginSession");
            break;
          }
          case XR_SESSION_STATE_STOPPING:
            xrEndSession(session);
            running = false;
            break;
          case XR_SESSION_STATE_EXITING:
          case XR_SESSION_STATE_LOSS_PENDING:
            quit = true;
            break;
          default: break;
        }
      } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
        Log("instance loss pending");
        quit = true;
      }
      ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (quit) break;
    if (!running) { Sleep(50); continue; }

    if (stereoMode) g_live.Poll();
    PollDumpKey();
    PollCropKey();
    PollSpreadKeys();

    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!Ok(xrWaitFrame(session, nullptr, &fs), "xrWaitFrame")) break;
    if (!Ok(xrBeginFrame(session, nullptr), "xrBeginFrame")) break;

    // Publish the eye views for the render thread's camera override (immersive mode).
    {
      XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
      vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      vli.displayTime = fs.predictedDisplayTime;
      vli.space = space;
      XrViewState vs{XR_TYPE_VIEW_STATE};
      XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
      uint32_t nv = 0;
      const bool ok = XR_SUCCEEDED(xrLocateViews(session, &vli, &vs, 2, &nv, views)) && nv == 2 &&
                      (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
      std::lock_guard lock(g_xrViews.m);
      g_xrViews.valid = ok;
      if (ok) {
        for (int i = 0; i < 2; ++i) { g_xrViews.pose[i] = views[i].pose; g_xrViews.fov[i] = views[i].fov; }
        rtFov[0] = views[0].fov;
        rtFov[1] = views[1].fov;
        haveRtFov = true;
      }
    }

    // Match the swapchain to the current capture texture.
    uint64_t gen;
    uint32_t w, h;
    bool haveFrame;
    int frameEye;
    XrFovf frameFov{};
    {
      std::lock_guard lock(g_shared.m);
      gen = g_shared.generation;
      w = g_shared.width;
      h = g_shared.height;
      haveFrame = g_shared.tex && g_shared.framesInGen > 0;
      frameEye = g_shared.frameEye;
      frameFov = g_shared.frameFov;
    }
    // Swapchain size: immersive eye images filtered down to what the headset uses ([vr] eye_resolution);
    // the virtual screen keeps Zwift's pixels.
    uint32_t sw = w, sh = h;
    if (scaleMode && frameEye >= 0 && haveRtFov) ScaledEyeSize(w, h, frameFov, rtFov[0], recW, recH, cfg.eyeResolution, &sw, &sh);
    const bool resize = gen != sc.generation || sw != sc.w || sh != sc.h;
    if (haveFrame && resize && sp.graphicsProperties.maxSwapchainImageWidth &&
        (sw > sp.graphicsProperties.maxSwapchainImageWidth || sh > sp.graphicsProperties.maxSwapchainImageHeight)) {
      // One eye's image doesn't fit the runtime's limit (e.g. a very big Zwift window): show nothing and say
      // why once, instead of failing xrCreateSwapchain every frame. (Each eye has its own array slice, so the
      // limit applies per eye - side by side, 3840 wide failed on a Quest 3, whose limit is 4096.)
      static uint64_t loggedGen = ~0ull;
      if (loggedGen != gen) {
        loggedGen = gen;
        Log("swapchain: eye image %ux%u is larger than the headset's limit %ux%u - make Zwift's window smaller "
            "([vr] big_window_width)", sw, sh, sp.graphicsProperties.maxSwapchainImageWidth,
            sp.graphicsProperties.maxSwapchainImageHeight);
      }
      haveFrame = false;
    }
    if (haveFrame && resize) {
      sc.Destroy();
      XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
      ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
      ci.format = scFormat;
      ci.sampleCount = 1;
      ci.width = sw;  // array slice 0 = left eye, 1 = right eye (screen mode without stereo uses slice 0 only)
      ci.height = sh;
      ci.faceCount = 1;
      ci.arraySize = 2;
      ci.mipCount = 1;
      if (Ok(xrCreateSwapchain(session, &ci, &sc.handle), "xrCreateSwapchain")) {
        uint32_t n = 0;
        xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
        sc.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data()));
        sc.w = sw;
        sc.h = sh;
        sc.srcW = w;
        sc.srcH = h;
        sc.generation = gen;
        if (sw != w || sh != h)
          Log("swapchain %ux%u x2 eyes, %u images (eye images %ux%u filtered down to the headset's resolution)", sw,
              sh, n, w, h);
        else
          Log("swapchain %ux%u x2 eyes, %u images", sw, sh, n);
      } else {
        sc = Swapchain{};
      }
    }

    XrCompositionLayerQuad quads[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    XrCompositionLayerProjectionView pviews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad cardQuads[Config::kMaxHudCards];
    bool immersiveFrame = false;
    const XrCompositionLayerBaseHeader* layers[2 + Config::kMaxHudCards];
    uint32_t layerCount = 0;
    if (fs.shouldRender && sc.handle && haveFrame) {
      uint32_t idx = 0;
      XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      wi.timeout = XR_INFINITE_DURATION;
      if (Ok(xrAcquireSwapchainImage(sc.handle, nullptr, &idx), "xrAcquireSwapchainImage")) {
        if (Ok(xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage")) {
          const double tReq = NowMs();
          std::lock_guard lock(g_shared.m);
          const double tGot = NowMs();
          g_xrLockWait.Add(tGot - tReq);
          struct HoldTimer { double t0; ~HoldTimer() { g_xrLockHold.Add(NowMs() - t0); } } hold{tGot};
          if (g_shared.tex && g_shared.generation == sc.generation) {
            ID3D11Texture2D* img = sc.images[idx].texture;
            D3D11_BOX box{0, 0, 0, sc.w, sc.h, 1};
            // Immersive: keep the newest image per eye (Zwift renders them alternately) with the
            // pose/FOV it was rendered for; submit both once we have both.
            // Eye images filtered down to the headset's resolution (swapchain smaller than the capture)?
            const bool scaledSc = (sc.w != sc.srcW || sc.h != sc.srcH) &&
                                  EnsureScaleResources(dev, sc.generation, sc.w, sc.h, sharedFmt);
            if (g_shared.frameEye >= 0 && EnsureEyeTextures(dev, sc.generation, sc.srcW, sc.srcH, sharedFmt)) {
              if (g_shared.framesInGen != g_eyes.lastFrame) {
                const int fe = g_shared.frameEye;
                // COHERENT (2): this one image, rendered from the mid-eye, goes to BOTH eyes.
                for (int e = fe == 2 ? 0 : fe; e <= (fe == 2 ? 1 : fe); ++e) {
                  ctx->CopyResource(g_eyes.tex[e], g_shared.tex);
                  if (scaledSc) DownscaleEye(dev, ctx, e, g_eyes.tex[e]);
                  g_eyes.pose[e] = g_shared.framePose;
                  g_eyes.fov[e] = g_shared.frameFov;
                  g_eyes.info[e] = g_shared.frameInfo;
                  g_eyes.have[e] = true;
                }
                if (fe == 2) {
                  g_eyes.coherentUpdates++;
                  g_eyes.lastEye = -1;
                } else {
                  g_eyes.updates[fe]++;
                  if (fe == g_eyes.lastEye) g_eyes.nonAlternating++;
                  g_eyes.lastEye = fe;
                  if (g_eyes.have[0] && g_eyes.have[1]) {
                    const uint64_t a = g_eyes.info[0].frameNo, b = g_eyes.info[1].frameNo;
                    g_eyes.maxFrameGap = std::max(g_eyes.maxFrameGap, a > b ? a - b : b - a);
                  }
                }
                // [hud]: this frame's world went to the eye above; cut its HUD out for the cards. A frame without
                // a split (one missed snapshot) keeps the previous HUD image rather than blinking the cards.
                if (!g_shared.hudOn.load()) g_hud.have = false;
                else if (hudMode && g_shared.hudValid &&
                         EnsureHudResources(dev, sc.generation, sc.srcW, sc.srcH, sharedFmt))
                  MakeHud(ctx);
                g_eyes.lastFrame = g_shared.framesInGen;
              }
            } else if (g_shared.frameEye < 0) {
              g_eyes.have[0] = g_eyes.have[1] = false;
            }
            if (g_eyes.have[0] && g_eyes.have[1]) {
              // Destination subresource = array slice (1 mip): 0 = left eye, 1 = right eye.
              for (int e = 0; e < 2; ++e) {
                if (scaledSc && !g_scale.fresh[e]) DownscaleEye(dev, ctx, e, g_eyes.tex[e]);  // swapchain just resized
                ctx->CopySubresourceRegion(img, static_cast<UINT>(e), 0, 0, 0,
                                           scaledSc ? g_scale.out[e] : g_eyes.tex[e], 0, &box);
              }
              immersiveFrame = true;
              const bool crop = g_xrViews.crop.load() && haveRtFov;
              for (int i = 0; i < 2; ++i) {
                pviews[i].pose = g_eyes.pose[i];
                FillView(pviews[i], i, static_cast<int32_t>(sc.w), static_cast<int32_t>(sc.h), sc.handle, g_eyes.fov[i],
                         crop ? &rtFov[i] : nullptr);
                // Log only real changes: Zwift's own frustum (the no-scale-field fallback) jitters the crop by a
                // pixel or two every frame, which logged ~25 lines/s.
                const XrRect2Di& r = pviews[i].subImage.imageRect;
                const XrRect2Di& o = loggedRect[i];
                if (std::abs(r.offset.x - o.offset.x) > 8 || std::abs(r.offset.y - o.offset.y) > 8 ||
                    std::abs(r.extent.width - o.extent.width) > 8 || std::abs(r.extent.height - o.extent.height) > 8) {
                  loggedRect[i] = r;
                  Log("immersive: %s eye submits x %d y %d %dx%d, fov L %.2f R %.2f U %.2f D %.2f deg%s",
                      i == 0 ? "left" : "right", r.offset.x, r.offset.y,
                      r.extent.width, r.extent.height, pviews[i].fov.angleLeft * 57.29578f,
                      pviews[i].fov.angleRight * 57.29578f, pviews[i].fov.angleUp * 57.29578f,
                      pviews[i].fov.angleDown * 57.29578f, crop ? " (cropped to the headset's eye FOV)" : "");
                }
              }
              if (g_dumpRequested) {
                g_dumpRequested = false;
                DumpEyes(dev, ctx, sc.srcW, sc.srcH, sharedFmt, pviews, haveRtFov ? rtFov : nullptr);
                DumpHud(dev, ctx);
              }
            } else if (!stereoMode) {
              ctx->CopySubresourceRegion(img, 0, 0, 0, 0, g_shared.tex, 0, &box);
            } else if (g_shared.depthValid &&
                       EnsureStereoResources(dev, sc.generation, sc.w, sc.h, sharedFmt)) {
              StereoParams p{};
              p.W = sc.w;
              p.H = sc.h;
              p.maxShift = g_live.strength * (cfg.ipdMm * 0.001f * 0.5f) / (cfg.width / sc.w);
              p.convCm = cfg.convergenceM * 100.0f;
              p.skyCm = 1.0e7f;  // Zwift's sky sentinel is 1.32e8
              p.eyeSign = g_live.swapEyes ? -1.0f : 1.0f;
              ctx->UpdateSubresource(g_stereo.cb, 0, nullptr, &p, 0, 0);
              ID3D11ShaderResourceView* srvs[2] = {g_stereo.colorSrv, g_stereo.depthSrv};
              ctx->CSSetShader(g_stereo.cs, nullptr, 0);
              ctx->CSSetConstantBuffers(0, 1, &g_stereo.cb);
              ctx->CSSetShaderResources(0, 2, srvs);
              ctx->CSSetUnorderedAccessViews(0, 1, &g_stereo.uav, nullptr);
              ctx->Dispatch((sc.w * 2 + 7) / 8, (sc.h + 7) / 8, 1);
              ID3D11ShaderResourceView* noSrv[2] = {};
              ID3D11UnorderedAccessView* noUav = nullptr;
              ctx->CSSetShaderResources(0, 2, noSrv);
              ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
              ctx->CSSetShader(nullptr, nullptr, 0);
              // g_stereo.out is left | right side by side; each half goes to its eye's array slice.
              D3D11_BOX left{0, 0, 0, sc.w, sc.h, 1}, right{sc.w, 0, 0, sc.w * 2, sc.h, 1};
              ctx->CopySubresourceRegion(img, 0, 0, 0, 0, g_stereo.out, 0, &left);
              ctx->CopySubresourceRegion(img, 1, 0, 0, 0, g_stereo.out, 0, &right);
              g_stereo.framesStereo++;
            } else {  // no depth this frame (menus, loading): same flat image to both eyes
              ctx->CopySubresourceRegion(img, 0, 0, 0, 0, g_shared.tex, 0, &box);
              ctx->CopySubresourceRegion(img, 1, 0, 0, 0, g_shared.tex, 0, &box);
              g_stereo.framesFlat++;
            }
          }
        }
        xrReleaseSwapchainImage(sc.handle, nullptr);

        const int32_t W = static_cast<int32_t>(sc.w), H = static_cast<int32_t>(sc.h);
        if (immersiveFrame) {
          // A real VR layer: each eye's image with the exact pose + FOV Zwift rendered it for (pviews were
          // filled above, cropped to the headset's eye FOV unless Ctrl+Shift+Insert turned that off).
          proj.space = space;
          proj.viewCount = 2;
          proj.views = pviews;
          layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
          layerCount = 1;
          submittedImmersive++;

          // [hud]: refresh the HUD swapchain when there's a new cut-out, then one quad per card on top.
          if (hudMode && g_hud.have && g_hud.out) {
            if (hudSc.generation != sc.generation || hudSc.w != g_hud.w || hudSc.h != g_hud.h) {
              hudSc.Destroy();
              XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
              ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
              ci.format = scFormat;
              ci.sampleCount = 1;
              ci.width = g_hud.w;
              ci.height = g_hud.h;
              ci.faceCount = 1;
              ci.arraySize = 1;
              ci.mipCount = 1;
              if (Ok(xrCreateSwapchain(session, &ci, &hudSc.handle), "xrCreateSwapchain (hud)")) {
                uint32_t n = 0;
                xrEnumerateSwapchainImages(hudSc.handle, 0, &n, nullptr);
                hudSc.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                xrEnumerateSwapchainImages(hudSc.handle, n, &n,
                                           reinterpret_cast<XrSwapchainImageBaseHeader*>(hudSc.images.data()));
                hudSc.w = g_hud.w;
                hudSc.h = g_hud.h;
                Log("hud: swapchain %ux%u, %u images", hudSc.w, hudSc.h, n);
              } else {
                hudSc = Swapchain{};
              }
              hudSc.generation = sc.generation;  // don't retry a failed create every frame
              g_hud.copied = 0;
            }
            if (hudSc.handle && g_hud.copied != g_hud.updates) {
              uint32_t hidx = 0;
              XrSwapchainImageWaitInfo hwi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
              hwi.timeout = XR_INFINITE_DURATION;
              if (Ok(xrAcquireSwapchainImage(hudSc.handle, nullptr, &hidx), "xrAcquireSwapchainImage (hud)")) {
                if (Ok(xrWaitSwapchainImage(hudSc.handle, &hwi), "xrWaitSwapchainImage (hud)"))
                  ctx->CopySubresourceRegion(hudSc.images[hidx].texture, 0, 0, 0, 0, g_hud.out, 0, nullptr);
                xrReleaseSwapchainImage(hudSc.handle, nullptr);
                hudSc.released = true;
                g_hud.copied = g_hud.updates;
              }
            }
            if (hudSc.handle && hudSc.released) {
              float home[3];
              CamHome(home);
              const float hw = static_cast<float>(hudSc.w), hh = static_cast<float>(hudSc.h);
              for (int c = 0; c < cfg.hudCardCount; ++c) {
                const Config::HudCard& k = cfg.hudCards[c];
                const int32_t x0 = static_cast<int32_t>(k.x0 * hw), y0 = static_cast<int32_t>(k.y0 * hh);
                const int32_t rw = std::max(1, static_cast<int32_t>(k.x1 * hw) - x0);
                const int32_t rh = std::max(1, static_cast<int32_t>(k.y1 * hh) - y0);
                XrCompositionLayerQuad& q = cardQuads[c];
                q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.layerFlags =
                    XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                q.space = space;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = hudSc.handle;
                q.subImage.imageRect = {{x0, y0}, {rw, rh}};
                q.pose = CardPose(home, k, g_hudSpread);
                q.size = {k.width, k.width * static_cast<float>(rh) / static_cast<float>(rw)};
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
              }
            }
          }
        }
        if (!immersiveFrame) layerCount = stereoMode ? 2 : 1;
        for (uint32_t i = 0; !immersiveFrame && i < layerCount; ++i) {
          XrCompositionLayerQuad& q = quads[i];
          q.space = space;
          q.eyeVisibility = !stereoMode ? XR_EYE_VISIBILITY_BOTH : i == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
          q.subImage.swapchain = sc.handle;
          q.subImage.imageArrayIndex = i;  // stereo window: slice 0 left, 1 right; flat: slice 0
          q.subImage.imageRect = {{0, 0}, {W, H}};
          q.pose.orientation.w = 1.0f;
          q.pose.position = {0.0f, cfg.height, -cfg.distance};
          q.size = {cfg.width, cfg.width * sc.h / sc.w};
          layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
        }
        submitted++;
      }
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = blend;
    ei.layerCount = layerCount;
    ei.layers = layers;
    if (!Ok(xrEndFrame(session, &ei), "xrEndFrame")) break;

    ULONGLONG now = GetTickCount64();
    if (now - statsSince >= 10000) {
      Log("submitted %u frames with the Zwift layer in %.1fs%s", submitted, (now - statsSince) / 1000.0,
          stereoMode ? (" (stereo " + std::to_string(g_stereo.framesStereo) + ", flat " +
                        std::to_string(g_stereo.framesFlat) + ")").c_str() : "");
      Log("  XR ms avg/max: shared-lock wait %.3f/%.3f, hold %.3f/%.3f", g_xrLockWait.Avg(), g_xrLockWait.max,
          g_xrLockHold.Avg(), g_xrLockHold.max);
      if (submittedImmersive || g_eyes.updates[0] || g_eyes.updates[1] || g_eyes.coherentUpdates)
        Log("  immersive: %u projection-layer frames; new eye images L %u, R %u, both (COHERENT) %u | same eye twice "
            "in a row %u, max L/R frame gap %llu",
            submittedImmersive, g_eyes.updates[0], g_eyes.updates[1], g_eyes.coherentUpdates, g_eyes.nonAlternating,
            static_cast<unsigned long long>(g_eyes.maxFrameGap));
      if (hudMode && (g_hud.updatesInWindow || g_hud.have))
        Log("  hud: %u new HUD images, %d card(s) %s", g_hud.updatesInWindow, cfg.hudCardCount,
            g_hud.have && hudSc.released ? "shown" : "not shown yet");
      g_hud.updatesInWindow = 0;
      if (g_scale.scaledInWindow)
        Log("  eye_resolution: %u eye images filtered %ux%u -> %ux%u", g_scale.scaledInWindow, sc.srcW, sc.srcH, sc.w,
            sc.h);
      g_scale.scaledInWindow = 0;
      submittedImmersive = 0;
      g_eyes.updates[0] = g_eyes.updates[1] = g_eyes.coherentUpdates = g_eyes.nonAlternating = 0;
      g_eyes.maxFrameGap = 0;
      g_xrLockWait.Reset();
      g_xrLockHold.Reset();
      g_stereo.framesStereo = g_stereo.framesFlat = 0;
      submitted = 0;
      statsSince = now;
    }
  }

  Log("OpenXR session ended - will retry");
  ctx->Release();
  cleanup();
}

}  // namespace

DWORD WINAPI XrThreadMain(void* p) {
  const Config& cfg = *static_cast<Config*>(p);
  for (;;) {
    RunInstance(cfg);
    Sleep(3000);
  }
}

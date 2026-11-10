#pragma once
// State shared between Zwift's render thread (GL capture) and our OpenXR thread.
//
//   render thread:  wglSwapBuffers hook -> blit GL backbuffer into `tex` (via WGL_NV_DX_interop2)
//   XR thread:      xrWaitFrame loop    -> copy `tex` into the OpenXR swapchain -> quad layer
//
// The XR thread owns the D3D11 device (it must be created on the adapter OpenXR names);
// the render thread owns `tex` and recreates it when Zwift's window size changes.
#include <d3d11.h>
#include <openxr/openxr.h>

#include <atomic>
#include <cstdint>
#include <mutex>

// Per-frame facts the render thread knows about the image it just captured (immersive mode), carried to
// the XR thread so eye dumps can state each image's own Zwift camera / mode (diagnoses L/R skew).
struct FrameInfo {
  uint64_t frameNo = 0;          // render-thread counter of overridden Zwift frames
  bool haveCam = false;          // Zwift's original main camera was seen this frame
  float zwiftCam[3] = {};        // Zwift's ORIGINAL camera position (world, cm), before our override
  float zwiftFwd[3] = {};        // ... and its forward vector
  int mode = 0;                  // 0 STEREO, 2 COHERENT (both eyes from this frame); 1 was MONO CHECK (removed)
  float parallax = 1.0f;         // eye-separation scale this frame was rendered with
};

struct SharedState {
  // Published once by the XR thread; never destroyed. Multithread-protected.
  std::atomic<ID3D11Device*> device{nullptr};
  // Non-sRGB twin of the swapchain format, so the copy is a plain bit copy.
  std::atomic<DXGI_FORMAT> sharedFormat{DXGI_FORMAT_UNKNOWN};
  // [hud]: split Zwift's HUD off the eye images (render thread snapshots, XR thread shows cards).
  // hudConfigured = the feature is set up at all; hudOn = live state (Ctrl+Shift+F9, render thread writes).
  std::atomic<bool> hudConfigured{false};
  std::atomic<bool> hudOn{false};
  // The window has alpha and it was zeroed before the HUD: hudTex's alpha = HUD coverage (clean panels).
  std::atomic<bool> hudAlpha{false};

  std::mutex m;  // guards everything below
  ID3D11Texture2D* tex = nullptr;
  ID3D11Texture2D* depthTex = nullptr;  // R32_FLOAT linear view depth (cm), GL row order (bottom-up)
  uint32_t width = 0, height = 0;
  uint64_t generation = 0;     // bumped whenever tex/depthTex are recreated
  uint64_t framesInGen = 0;    // frames written into the current tex
  bool depthValid = false;     // depthTex holds the same frame as tex
  // HUD split (immersive frames): when hudValid, `tex` holds the world only (snapshotted just before Zwift's
  // HUD drew into the window) and `hudTex` the finished frame (world + HUD) of the same Zwift frame.
  ID3D11Texture2D* hudTex = nullptr;
  bool hudValid = false;

  // Immersive mode: which eye / pose / fov the frame in `tex` was rendered with (camera override).
  // frameEye 0/1 = that eye (alternate-eye rendering), 2 = COHERENT: one image for BOTH eyes.
  int frameEye = -1;  // -1 = not an immersive frame (screen mode)
  XrPosef framePose{{0, 0, 0, 1}, {0, 0, 0}};
  XrFovf frameFov{};
  FrameInfo frameInfo;
};

extern SharedState g_shared;

// Latest headset eye views (LOCAL space), published by the XR thread every frame, read by the
// render thread when it latches the camera for the next Zwift frame.
struct XrViewsShared {
  std::mutex m;
  bool valid = false;
  XrPosef pose[2]{};
  XrFovf fov[2]{};
  // XR thread -> render thread, lock-free:
  std::atomic<bool> crop{true};                    // projection views cropped to the runtime eye FOV (Ctrl+Shift+Insert)
  std::atomic<unsigned long long> recentreAtTick{0};  // GetTickCount64 at which to re-home the eye origin (runtime recentre)
};

extern XrViewsShared g_xrViews;

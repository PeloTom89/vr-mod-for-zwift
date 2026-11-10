#include "gl_capture.h"

#include <GL/gl.h>
#include <d3d11.h>

#include <string>

#include "camera.h"
#include "gl_trace.h"
#include "log.h"
#include "shared.h"
#include "timing.h"

// ---- the handful of GL 3.0 / WGL_NV_DX_interop2 bits we need (no glext.h dependency) ----
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RENDERBUFFER 0x8D41
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA8 0x8058
#ifndef GL_SAMPLES
#define GL_SAMPLES 0x80A9
#endif
#ifndef GL_SAMPLE_BUFFERS
#define GL_SAMPLE_BUFFERS 0x80A8
#endif
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002

using PFN_GenFramebuffers = void(APIENTRY*)(GLsizei, GLuint*);
using PFN_BindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
using PFN_FramebufferTexture2D = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_CheckFramebufferStatus = GLenum(APIENTRY*)(GLenum);
using PFN_BlitFramebuffer = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
using PFN_GenRenderbuffers = void(APIENTRY*)(GLsizei, GLuint*);
using PFN_DeleteRenderbuffers = void(APIENTRY*)(GLsizei, const GLuint*);
using PFN_BindRenderbuffer = void(APIENTRY*)(GLenum, GLuint);
using PFN_RenderbufferStorage = void(APIENTRY*)(GLenum, GLenum, GLsizei, GLsizei);
using PFN_FramebufferRenderbuffer = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint);
using PFN_DXOpenDevice = HANDLE(WINAPI*)(void*);
using PFN_DXRegisterObject = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using PFN_DXUnregisterObject = BOOL(WINAPI*)(HANDLE, HANDLE);
using PFN_DXLockObjects = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);
using PFN_CopyImageSubData = void(APIENTRY*)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint,
                                             GLint, GLint, GLsizei, GLsizei, GLsizei);
using PFN_GetTextureLevelParameteriv = void(APIENTRY*)(GLuint, GLint, GLenum, GLint*);

namespace {

struct GlFns {
  PFN_GenFramebuffers GenFramebuffers;
  PFN_BindFramebuffer BindFramebuffer;
  PFN_FramebufferTexture2D FramebufferTexture2D;
  PFN_CheckFramebufferStatus CheckFramebufferStatus;
  PFN_BlitFramebuffer BlitFramebuffer;
  PFN_GenRenderbuffers GenRenderbuffers;
  PFN_DeleteRenderbuffers DeleteRenderbuffers;
  PFN_BindRenderbuffer BindRenderbuffer;
  PFN_RenderbufferStorage RenderbufferStorage;
  PFN_FramebufferRenderbuffer FramebufferRenderbuffer;
  PFN_DXOpenDevice DXOpenDeviceNV;
  PFN_DXRegisterObject DXRegisterObjectNV;
  PFN_DXUnregisterObject DXUnregisterObjectNV;
  PFN_DXLockObjects DXLockObjectsNV;
  PFN_DXLockObjects DXUnlockObjectsNV;
  // optional (depth); null = depth disabled
  PFN_CopyImageSubData CopyImageSubData;
  PFN_GetTextureLevelParameteriv GetTextureLevelParameteriv;
};

// Per-GL-context capture state. Only touched on Zwift's render thread.
struct Capture {
  HGLRC ctx = nullptr;
  bool failed = false;
  GlFns gl{};
  HANDLE dxDevice = nullptr;
  GLuint dstFbo = 0, glTex = 0, glDepth = 0;
  HANDLE dxObj = nullptr, dxDepthObj = nullptr;
  unsigned framesSinceProbe = 0;
  GLuint loggedBadDepth = 0;
  bool needAttach = false;
  int samples = -1;  // of the default framebuffer; -1 = not queried yet
  GLuint resolveFbo = 0, resolveRb = 0;
  UINT w = 0, h = 0;      // captured (possibly cropped) size = shared texture size
  UINT srcX = 0, srcY = 0;  // bottom-left of the captured region in the window (GL coords)
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  // HUD split: the window snapshotted just before Zwift's HUD draws (full window, GL row order, 1 sample)
  UINT winW = 0, winH = 0;                // window size at the last swap
  GLuint worldFbo = 0, worldRb = 0;
  UINT worldW = 0, worldH = 0;
  GLuint hudGlTex = 0, hudDstFbo = 0;     // GL alias of g_shared.hudTex (+ its blit target)
  HANDLE dxHudObj = nullptr;
  bool hudNeedAttach = false;
  unsigned windowDraws = 0;               // draws into the window this frame
  bool worldValid = false;                // snapshot taken this frame
  int alphaBits = -1;                     // the window's alpha bits (-1 = not queried yet)
  // stats
  unsigned frames = 0, glErrorsLogged = 0;
  unsigned hudFrames = 0, immersiveFrames = 0, windowDrawsSum = 0;
  ULONGLONG statsSince = 0;
};

Capture s;
float g_cropAspect = 0.0f;
bool g_wantDepth = false;
bool g_depthOn = true;  // Ctrl+Shift+F11 toggles the depth path live (A/B for performance)
bool g_toggleKeyWasDown = false;
int g_hudCompositeDraws = 1;
bool g_hudWantAlpha = true;
bool g_hudKeyWasDown = false;
Stat g_tMutex, g_tDxLock, g_tBlit, g_tDepthCopy, g_tDxUnlock;

template <class T>
bool Load(T& fn, const char* name) {
  fn = reinterpret_cast<T>(wglGetProcAddress(name));
  if (!fn) Log("GL: missing %s", name);
  return fn != nullptr;
}

bool LoadFns(GlFns& g) {
  bool ok = true;
  ok &= Load(g.GenFramebuffers, "glGenFramebuffers");
  ok &= Load(g.BindFramebuffer, "glBindFramebuffer");
  ok &= Load(g.FramebufferTexture2D, "glFramebufferTexture2D");
  ok &= Load(g.CheckFramebufferStatus, "glCheckFramebufferStatus");
  ok &= Load(g.BlitFramebuffer, "glBlitFramebuffer");
  ok &= Load(g.GenRenderbuffers, "glGenRenderbuffers");
  ok &= Load(g.DeleteRenderbuffers, "glDeleteRenderbuffers");
  ok &= Load(g.BindRenderbuffer, "glBindRenderbuffer");
  ok &= Load(g.RenderbufferStorage, "glRenderbufferStorage");
  ok &= Load(g.FramebufferRenderbuffer, "glFramebufferRenderbuffer");
  ok &= Load(g.DXOpenDeviceNV, "wglDXOpenDeviceNV");
  ok &= Load(g.DXRegisterObjectNV, "wglDXRegisterObjectNV");
  ok &= Load(g.DXUnregisterObjectNV, "wglDXUnregisterObjectNV");
  ok &= Load(g.DXLockObjectsNV, "wglDXLockObjectsNV");
  ok &= Load(g.DXUnlockObjectsNV, "wglDXUnlockObjectsNV");
  if (g_wantDepth &&
      !(Load(g.CopyImageSubData, "glCopyImageSubData") &&
        Load(g.GetTextureLevelParameteriv, "glGetTextureLevelParameteriv"))) {
    g.CopyImageSubData = nullptr;
    Log("depth copy unavailable - flat screen only");
  }
  return ok;
}

// (Re)creates the shared D3D11 texture + its GL alias at the window's size.
bool Recreate(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt) {
  std::lock_guard lock(g_shared.m);
  if (s.dxObj) { s.gl.DXUnregisterObjectNV(s.dxDevice, s.dxObj); s.dxObj = nullptr; }
  if (s.dxDepthObj) { s.gl.DXUnregisterObjectNV(s.dxDevice, s.dxDepthObj); s.dxDepthObj = nullptr; }
  if (s.glTex) { glDeleteTextures(1, &s.glTex); s.glTex = 0; }
  if (s.glDepth) { glDeleteTextures(1, &s.glDepth); s.glDepth = 0; }
  if (g_shared.tex) { g_shared.tex->Release(); g_shared.tex = nullptr; }
  if (g_shared.depthTex) { g_shared.depthTex->Release(); g_shared.depthTex = nullptr; }
  g_shared.depthValid = false;
  if (s.dxHudObj) { s.gl.DXUnregisterObjectNV(s.dxDevice, s.dxHudObj); s.dxHudObj = nullptr; }
  if (s.hudGlTex) { glDeleteTextures(1, &s.hudGlTex); s.hudGlTex = 0; }
  if (g_shared.hudTex) { g_shared.hudTex->Release(); g_shared.hudTex = nullptr; }
  g_shared.hudValid = false;
  if (s.resolveRb) { s.gl.DeleteRenderbuffers(1, &s.resolveRb); s.resolveRb = 0; }
  s.w = s.h = 0;

  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  HRESULT hr = dev->CreateTexture2D(&d, nullptr, &g_shared.tex);
  if (FAILED(hr)) { Log("D3D11 CreateTexture2D %ux%u fmt %d failed: 0x%08lx", w, h, fmt, hr); return false; }

  glGenTextures(1, &s.glTex);
  s.dxObj = s.gl.DXRegisterObjectNV(s.dxDevice, g_shared.tex, s.glTex, GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
  if (!s.dxObj) { Log("wglDXRegisterObjectNV failed: %lu", GetLastError()); return false; }

  if (s.gl.CopyImageSubData) {  // R32F twin for Zwift's linear depth; failure just means no stereo
    D3D11_TEXTURE2D_DESC dd = d;
    dd.Format = DXGI_FORMAT_R32_FLOAT;
    dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(dev->CreateTexture2D(&dd, nullptr, &g_shared.depthTex))) {
      glGenTextures(1, &s.glDepth);
      s.dxDepthObj = s.gl.DXRegisterObjectNV(s.dxDevice, g_shared.depthTex, s.glDepth, GL_TEXTURE_2D,
                                             WGL_ACCESS_WRITE_DISCARD_NV);
      if (!s.dxDepthObj) {
        Log("depth: wglDXRegisterObjectNV failed: %lu - flat screen only", GetLastError());
        glDeleteTextures(1, &s.glDepth);
        s.glDepth = 0;
        g_shared.depthTex->Release();
        g_shared.depthTex = nullptr;
      }
    }
  }

  if (g_shared.hudConfigured.load()) {  // twin for the finished frame (world + HUD); failure = no HUD cards
    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_shared.hudTex))) {
      glGenTextures(1, &s.hudGlTex);
      s.dxHudObj = s.gl.DXRegisterObjectNV(s.dxDevice, g_shared.hudTex, s.hudGlTex, GL_TEXTURE_2D,
                                           WGL_ACCESS_WRITE_DISCARD_NV);
      if (!s.dxHudObj) {
        Log("hud: wglDXRegisterObjectNV failed: %lu - HUD stays in the eye images", GetLastError());
        glDeleteTextures(1, &s.hudGlTex);
        s.hudGlTex = 0;
        g_shared.hudTex->Release();
        g_shared.hudTex = nullptr;
      }
    }
    s.hudNeedAttach = true;
  }

  s.w = w;
  s.h = h;
  s.fmt = fmt;
  s.needAttach = true;
  s.samples = -1;
  g_shared.width = w;
  g_shared.height = h;
  g_shared.generation++;
  g_shared.framesInGen = 0;
  Log("capture target %ux%u, DXGI format %d (generation %llu)", w, h, fmt, g_shared.generation);
  return true;
}

// Copies the captured region (flipped to D3D's top-down order) from the window's back buffer (srcFbo 0) or
// from one of our single-sample FBOs of the window's size (the HUD split's world snapshot) into a shared texture.
void Blit(GLuint srcFbo, GLuint& dstFbo, GLuint dstTex, bool& needAttach) {
  auto& gl = s.gl;
  GLint readFbo = 0, drawFbo = 0, readBuf = 0, rb = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  while (glGetError() != GL_NO_ERROR) {}  // don't blame Zwift's errors on us

  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glGetIntegerv(GL_READ_BUFFER, &readBuf);
  glReadBuffer(GL_BACK);
  if (s.samples < 0) {
    GLint sb = 0;
    glGetIntegerv(GL_SAMPLE_BUFFERS, &sb);
    if (sb) glGetIntegerv(GL_SAMPLES, &s.samples); else s.samples = 0;
    Log("default framebuffer: %d samples", s.samples);
  }
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  if (srcFbo) {  // our own FBO: its read buffer is ours to set (FBO 0's was saved above)
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
  }

  if (!dstFbo) gl.GenFramebuffers(1, &dstFbo);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
  if (needAttach) {
    gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    GLenum st = gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) Log("capture FBO incomplete: 0x%x", st);
    needAttach = false;
  }

  const GLint W = static_cast<GLint>(s.w), H = static_cast<GLint>(s.h);
  GLint X = static_cast<GLint>(s.srcX), Y = static_cast<GLint>(s.srcY);
  if (!srcFbo && s.samples > 0) {
    // A multisampled resolve can't flip or scale, so resolve the region 1:1 first, then flip-blit.
    if (!s.resolveFbo) gl.GenFramebuffers(1, &s.resolveFbo);
    if (!s.resolveRb) {
      gl.GenRenderbuffers(1, &s.resolveRb);
      gl.BindRenderbuffer(GL_RENDERBUFFER, s.resolveRb);
      gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
      gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, s.resolveFbo);
      gl.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s.resolveRb);
    }
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, s.resolveFbo);
    gl.BlitFramebuffer(X, Y, X + W, Y + H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, s.resolveFbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    X = Y = 0;
  }
  // GL's origin is bottom-left, D3D's is top-left: flip vertically.
  gl.BlitFramebuffer(X, Y, X + W, Y + H, 0, H, W, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  GLenum err = glGetError();
  if (err != GL_NO_ERROR && s.glErrorsLogged < 5) { Log("GL error 0x%x during capture blit", err); s.glErrorsLogged++; }

  // restore Zwift's state
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glReadBuffer(static_cast<GLenum>(readBuf));
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
  gl.BindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(rb));
  if (scissor) glEnable(GL_SCISSOR_TEST);
  if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
}

// Mid-frame, right before Zwift's first HUD draw: copy the whole window 1:1 into worldRb (resolving MSAA if
// any). Restores every binding/enable it touches - Zwift carries on drawing its HUD straight after.
void SnapshotWorld() {
  auto& gl = s.gl;
  if (!s.winW || !s.winH) return;
  GLint readFbo = 0, drawFbo = 0, readBuf = 0, rb = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);

  if (!s.worldFbo) gl.GenFramebuffers(1, &s.worldFbo);
  if (s.worldW != s.winW || s.worldH != s.winH) {
    if (s.worldRb) gl.DeleteRenderbuffers(1, &s.worldRb);
    gl.GenRenderbuffers(1, &s.worldRb);
    gl.BindRenderbuffer(GL_RENDERBUFFER, s.worldRb);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, static_cast<GLsizei>(s.winW), static_cast<GLsizei>(s.winH));
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, s.worldFbo);
    gl.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s.worldRb);
    const GLenum st = gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) Log("hud: world snapshot FBO incomplete: 0x%x", st);
    s.worldW = s.winW;
    s.worldH = s.winH;
    Log("hud: world snapshot %ux%u (before window draw #%d)", s.worldW, s.worldH, g_hudCompositeDraws + 1);
  }
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glGetIntegerv(GL_READ_BUFFER, &readBuf);
  glReadBuffer(GL_BACK);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, s.worldFbo);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  const GLint W = static_cast<GLint>(s.winW), H = static_cast<GLint>(s.winH);
  gl.BlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  // Clean panel backgrounds: zero the window's ALPHA only (the colour - what the monitor shows - is untouched).
  // Zwift's HUD then blends over it, leaving each pixel's HUD coverage in alpha, so the XR thread can take the
  // world back out of translucent panels instead of showing a copy of the scenery that moves with the head.
  if (g_hudWantAlpha) {
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (s.alphaBits < 0) {
      GLint bits = 0;
      glGetIntegerv(GL_ALPHA_BITS, &bits);
      s.alphaBits = bits;
      g_shared.hudAlpha.store(bits > 0);
      Log("hud: window alpha bits %d - %s", bits,
          bits > 0 ? "panel backgrounds from the HUD's own coverage" : "no alpha: panels keep the world behind them");
    }
    if (s.alphaBits > 0) {
      GLboolean mask[4];
      GLfloat clear[4];
      glGetBooleanv(GL_COLOR_WRITEMASK, mask);
      glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
      glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
      glClear(GL_COLOR_BUFFER_BIT);  // scissor is off here
      glColorMask(mask[0], mask[1], mask[2], mask[3]);
      glClearColor(clear[0], clear[1], clear[2], clear[3]);
      GlTraceHudBlend(true);  // until the swap: every HUD blend writes "over" coverage into the window's alpha
    }
  }

  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glReadBuffer(static_cast<GLenum>(readBuf));
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
  gl.BindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(rb));
  if (scissor) glEnable(GL_SCISSOR_TEST);
  if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
  s.worldValid = true;
}

}  // namespace

void GlCaptureConfigure(float cropAspect, bool wantDepth) {
  g_cropAspect = cropAspect;
  g_wantDepth = wantDepth;
}

void GlCaptureConfigureHud(bool on, int compositeDraws, bool useAlpha) {
  g_hudCompositeDraws = compositeDraws;
  g_hudWantAlpha = useAlpha;
  g_shared.hudConfigured.store(on);
  g_shared.hudOn.store(on);
}

void GlCaptureOnWindowDraw() {
  const unsigned n = ++s.windowDraws;
  if (n != static_cast<unsigned>(g_hudCompositeDraws) + 1 || s.worldValid) return;
  if (s.failed || !s.dxHudObj || !g_shared.hudOn.load() || !CamActive()) return;
  if (wglGetCurrentContext() != s.ctx) return;  // only Zwift's main context, the one we capture
  SnapshotWorld();
}

void GlCaptureFrame(HDC hdc) {
  // This frame's HUD-split facts, then reset for the next frame whatever happens below.
  const bool worldSnap = s.worldValid;
  const unsigned windowDraws = s.windowDraws;
  s.worldValid = false;
  s.windowDraws = 0;
  GlTraceHudBlend(false);  // the HUD is drawn: Zwift's own blend factors back before its next frame

  HGLRC ctx = wglGetCurrentContext();
  if (!ctx) return;
  ID3D11Device* dev = g_shared.device.load();
  DXGI_FORMAT fmt = g_shared.sharedFormat.load();
  if (!dev || fmt == DXGI_FORMAT_UNKNOWN) return;  // headset/runtime not up yet

  if (ctx != s.ctx) {
    if (s.ctx) Log("GL context changed (%p -> %p), re-initialising capture", s.ctx, ctx);
    s = Capture{};  // old context's objects are abandoned; they die with that context
    {
      std::lock_guard lock(g_shared.m);
      if (g_shared.tex) { g_shared.tex->Release(); g_shared.tex = nullptr; }
      if (g_shared.hudTex) { g_shared.hudTex->Release(); g_shared.hudTex = nullptr; }
      g_shared.hudValid = false;
      g_shared.generation++;
      g_shared.framesInGen = 0;
    }
    s.ctx = ctx;
    Log("GL: %s | %s | %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    if (!LoadFns(s.gl)) { s.failed = true; Log("GL capture disabled: required functions missing"); return; }
    s.dxDevice = s.gl.DXOpenDeviceNV(dev);
    if (!s.dxDevice) { s.failed = true; Log("wglDXOpenDeviceNV failed: %lu - capture disabled", GetLastError()); return; }
    s.statsSince = GetTickCount64();
  }
  if (s.failed) return;

  RECT rc{};
  HWND wnd = WindowFromDC(hdc);
  if (!wnd || !GetClientRect(wnd, &rc)) return;
  const UINT winW = static_cast<UINT>(rc.right - rc.left), winH = static_cast<UINT>(rc.bottom - rc.top);
  if (!winW || !winH) return;  // minimised
  s.winW = winW;
  s.winH = winH;
  CamSetWindowAspect(static_cast<float>(winW) / static_cast<float>(winH));

  // Centre crop to the configured aspect: trim the sides of a too-wide window, or the
  // top/bottom of a too-tall one.
  // Immersive frames: the whole window is the eye's view frustum (Zwift stretches its scene to fill it).
  XrPosef eyePose{};
  XrFovf eyeFov{};
  const int eye = CamCurrentEye(&eyePose, &eyeFov);

  UINT w = winW, h = winH;
  if (g_cropAspect > 0.0f && eye < 0) {
    const float cur = static_cast<float>(winW) / static_cast<float>(winH);
    if (cur > g_cropAspect * 1.01f) w = static_cast<UINT>(winH * g_cropAspect + 0.5f);
    else if (cur < g_cropAspect / 1.01f) h = static_cast<UINT>(winW / g_cropAspect + 0.5f);
  }
  s.srcX = (winW - w) / 2;
  s.srcY = (winH - h) / 2;

  if (w != s.w || h != s.h || fmt != s.fmt) {
    if (w != winW || h != winH) Log("cropping %ux%u window to centre %ux%u", winW, winH, w, h);
    if (!Recreate(dev, w, h, fmt)) { s.failed = true; Log("GL capture disabled"); return; }
  }

  // Is Zwift's linear depth (T4) known and the same size as what we capture? If not, probe for it
  // every ~2 s (the probe costs one slow frame, and menus/loading screens have no scene pass).
  const bool toggleDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                          (GetAsyncKeyState(VK_F11) & 0x8000);
  if (toggleDown && !g_toggleKeyWasDown) {
    g_depthOn = !g_depthOn;
    Log("Ctrl+Shift+F11: depth path %s", g_depthOn ? "ON" : "OFF (flat)");
  }
  g_toggleKeyWasDown = toggleDown;
  const bool hudKey = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                      (GetAsyncKeyState(VK_F9) & 0x8000);
  if (hudKey && !g_hudKeyWasDown && g_shared.hudConfigured.load()) {
    const bool on = !g_shared.hudOn.load();
    g_shared.hudOn.store(on);
    Log("Ctrl+Shift+F9: HUD cards %s", on ? "ON (HUD out of the eye images)" : "OFF (HUD painted in the eye images)");
  }
  g_hudKeyWasDown = hudKey;

  // HUD split this frame: an immersive frame whose world was snapshotted before the HUD, at today's window size.
  const bool split = worldSnap && eye >= 0 && s.dxHudObj && g_shared.hudOn.load() && s.worldW == winW &&
                     s.worldH == winH && s.srcX == 0 && s.srcY == 0;
  if (eye >= 0) {
    s.immersiveFrames++;
    s.windowDrawsSum += windowDraws;
    if (split) s.hudFrames++;
  }

  GLuint srcDepth = 0;
  if (s.dxDepthObj && g_depthOn && eye < 0) {
    GLuint cand = GlTraceSceneLinearDepth();
    if (cand) {
      GLint dw = 0, dh = 0;
      s.gl.GetTextureLevelParameteriv(cand, 0, GL_TEXTURE_WIDTH, &dw);
      s.gl.GetTextureLevelParameteriv(cand, 0, GL_TEXTURE_HEIGHT, &dh);
      if (static_cast<UINT>(dw) == s.w && static_cast<UINT>(dh) == s.h) {
        srcDepth = cand;
      } else if (s.loggedBadDepth != cand) {
        Log("depth: T%u is %dx%d but capture is %ux%u - flat until they match (check crop_aspect vs Zwift's "
            "resolution)", cand, dw, dh, s.w, s.h);
        s.loggedBadDepth = cand;
      }
    }
    if (!srcDepth && ++s.framesSinceProbe >= 120 && !GlTraceProbeBusy()) {
      s.framesSinceProbe = 0;
      GlTraceRequestSceneProbe();
    }
  }

  {
    double t = NowMs();
    std::lock_guard lock(g_shared.m);
    double t2 = NowMs();
    g_tMutex.Add(t2 - t);
    // Only lock the depth object when we'll write it, so the flat A/B path matches phase 1.
    // Depth (screen-stereo) and HUD (immersive) never happen in the same frame.
    HANDLE objs[2] = {s.dxObj, split ? s.dxHudObj : s.dxDepthObj};
    const GLint nObjs = (srcDepth || split) ? 2 : 1;
    if (!s.gl.DXLockObjectsNV(s.dxDevice, nObjs, objs)) {
      if (s.glErrorsLogged++ < 5) Log("wglDXLockObjectsNV failed: %lu", GetLastError());
      return;
    }
    t = NowMs();
    g_tDxLock.Add(t - t2);
    if (split) {
      Blit(s.worldFbo, s.dstFbo, s.glTex, s.needAttach);   // eyes: the world, HUD-free
      Blit(0, s.hudDstFbo, s.hudGlTex, s.hudNeedAttach);   // the finished frame, to cut the HUD from
    } else {
      Blit(0, s.dstFbo, s.glTex, s.needAttach);
    }
    g_shared.hudValid = split;
    t2 = NowMs();
    g_tBlit.Add(t2 - t);
    if (srcDepth) {
      // Same size, both R32F: a raw copy. Stays in GL row order; the stereo shader flips it.
      s.gl.CopyImageSubData(srcDepth, GL_TEXTURE_2D, 0, 0, 0, 0, s.glDepth, GL_TEXTURE_2D, 0, 0, 0, 0,
                            static_cast<GLsizei>(s.w), static_cast<GLsizei>(s.h), 1);
    }
    t = NowMs();
    g_tDepthCopy.Add(t - t2);
    s.gl.DXUnlockObjectsNV(s.dxDevice, nObjs, objs);
    g_tDxUnlock.Add(NowMs() - t);
    g_shared.depthValid = srcDepth != 0;
    g_shared.frameEye = eye;
    g_shared.framePose = eyePose;
    g_shared.frameFov = eyeFov;
    if (!CamCurrentFrameInfo(&g_shared.frameInfo)) g_shared.frameInfo = FrameInfo{};
    g_shared.framesInGen++;
  }

  s.frames++;
  ULONGLONG now = GetTickCount64();
  if (now - s.statsSince >= 10000) {
    Log("captured %u frames in %.1fs (%.1f fps) at %ux%u, depth %s", s.frames, (now - s.statsSince) / 1000.0,
        s.frames * 1000.0 / (now - s.statsSince), s.w, s.h, g_depthOn ? "on" : "OFF");
    auto f = [](const Stat& st) { return std::to_string(st.Avg()).substr(0, 5) + "/" + std::to_string(st.max).substr(0, 5); };
    Log("  ms avg/max: frame %s | our hook %s = mutex %s + dxlock %s + blit %s + depthcopy %s + dxunlock %s | "
        "Zwift SwapBuffers %s",
        f(g_tFrameInterval).c_str(), f(g_tOurHook).c_str(), f(g_tMutex).c_str(), f(g_tDxLock).c_str(),
        f(g_tBlit).c_str(), f(g_tDepthCopy).c_str(), f(g_tDxUnlock).c_str(), f(g_tZwiftSwap).c_str());
    for (Stat* st : {&g_tFrameInterval, &g_tOurHook, &g_tMutex, &g_tDxLock, &g_tBlit, &g_tDepthCopy, &g_tDxUnlock,
                     &g_tZwiftSwap})
      st->Reset();
    if (s.immersiveFrames)
      Log("  hud: cards %s, split %u of %u immersive frames | window draws per frame avg %.1f (world snapshot "
          "before draw #%d - [hud] composite_draws)",
          !s.dxHudObj ? "unavailable" : g_shared.hudOn.load() ? "ON" : "off (Ctrl+Shift+F9)", s.hudFrames,
          s.immersiveFrames, static_cast<double>(s.windowDrawsSum) / s.immersiveFrames, g_hudCompositeDraws + 1);
    s.hudFrames = s.immersiveFrames = s.windowDrawsSum = 0;
    s.frames = 0;
    s.statsSince = now;
  }
}

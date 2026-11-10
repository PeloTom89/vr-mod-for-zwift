// zwift_vr.dll - injected into ZwiftApp.exe by zvr_loader.exe.
// Hooks the GL present calls to capture each frame; a separate thread shows it in OpenXR.
#include <windows.h>

#include <MinHook.h>

#include "camera.h"
#include "config.h"
#include "gl_capture.h"
#include "gl_trace.h"
#include "log.h"
#include "profiler.h"
#include "timing.h"
#include "vr_window.h"
#include "xr_thread.h"

namespace {

using PFN_SwapBuffers = BOOL(WINAPI*)(HDC);
PFN_SwapBuffers g_origWglSwapBuffers = nullptr;
PFN_SwapBuffers g_origGdiSwapBuffers = nullptr;

thread_local int t_depth = 0;  // gdi32!SwapBuffers and opengl32!wglSwapBuffers may call each other
bool g_captureDisabled = false;

// No C++ objects with destructors in here, so SEH can wrap it.
void SafeCapture(HDC hdc) {
  if (g_captureDisabled) return;
  __try {
    GlTraceOnSwap();  // first: ends a traced frame before our own GL calls run
    GlCaptureFrame(hdc);  // tags the frame with the eye it was rendered for
    CamOnSwap();          // then latch the eye pose for Zwift's next frame
    VrWindowOnSwap(WindowFromDC(hdc));  // Ctrl+Shift+Delete: big Zwift window (sharper eye images)
    ProfilerOnSwap();     // Ctrl+Shift+F8: sample this (render) thread for 4 s
    GlTraceArm();         // last: a newly armed trace starts with Zwift's next GL call
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_captureDisabled = true;
    CamDisable();  // CamOnSwap won't run again: don't leave a stale eye / reflection skip behind
    VrWindowDisable();  // nor Zwift's window bigger than the screen with the hotkey gone
    Log("exception 0x%08lx in capture - capture disabled for this run", GetExceptionCode());
  }
}

BOOL TimedSwap(PFN_SwapBuffers orig, HDC hdc) {
  const bool outer = t_depth++ == 0;
  double t0 = 0, t1 = 0;
  if (outer) {
    static double last = 0;
    t0 = NowMs();
    if (last) g_tFrameInterval.Add(t0 - last);
    last = t0;
    SafeCapture(hdc);
    t1 = NowMs();
    g_tOurHook.Add(t1 - t0);
  }
  BOOL r = orig(hdc);
  if (outer) g_tZwiftSwap.Add(NowMs() - t1);
  t_depth--;
  return r;
}

BOOL WINAPI HookWglSwapBuffers(HDC hdc) { return TimedSwap(g_origWglSwapBuffers, hdc); }
BOOL WINAPI HookGdiSwapBuffers(HDC hdc) { return TimedSwap(g_origGdiSwapBuffers, hdc); }

bool Hook(const wchar_t* module, const char* fn, void* detour, void** orig) {
  MH_STATUS st = MH_CreateHookApi(module, fn, detour, orig);
  if (st != MH_OK) { Log("hook %ls!%s failed: %s", module, fn, MH_StatusToString(st)); return false; }
  Log("hooked %ls!%s", module, fn);
  return true;
}

DWORD WINAPI InitThread(void*) {
  LogInit();
  Log("zwift_vr loaded into pid %lu", GetCurrentProcessId());
  static Config cfg = LoadConfig();
  GlCaptureConfigure(cfg.cropAspect, cfg.stereo);
  GlCaptureConfigureHud(cfg.hud, cfg.hudCompositeDraws, cfg.hudWindowAlpha);
  CamOptions co;
  co.startImmersive = cfg.immersive;
  co.worldScale = cfg.worldScale;
  co.parallax = cfg.parallax;
  co.recentrePosition = cfg.recentrePosition;
  co.coherentMode = cfg.coherentMode;
  co.autoScreenInMenus = cfg.autoScreenInMenus;
  CamConfigure(co);
  VrWindowConfigure(cfg.bigWindowWidth, cfg.autoBigWindow);
  GlTraceSetShadows(cfg.shadows);
  CamSetReflections(cfg.reflections);

  if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return 1; }
  bool any = false;
  any |= Hook(L"opengl32", "wglSwapBuffers", reinterpret_cast<void*>(&HookWglSwapBuffers),
              reinterpret_cast<void**>(&g_origWglSwapBuffers));
  any |= Hook(L"gdi32", "SwapBuffers", reinterpret_cast<void*>(&HookGdiSwapBuffers),
              reinterpret_cast<void**>(&g_origGdiSwapBuffers));
  if (!any) { Log("no hooks installed - giving up"); return 1; }
  if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { Log("MH_EnableHook failed"); return 1; }

  HANDLE xr = CreateThread(nullptr, 0, XrThreadMain, &cfg, 0, nullptr);
  if (xr) CloseHandle(xr);
  return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    // Real work happens off the loader lock.
    HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
  }
  return TRUE;
}

#include "vr_window.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "log.h"
#include "zwift_state.h"

namespace {

int g_width = 3840;
UINT g_msg = 0;  // our private window message: the resize/restore runs on Zwift's own message pump
HWND g_wnd = nullptr;
WNDPROC g_orig = nullptr;
bool g_unicode = true;
std::atomic<bool> g_big{false};
std::atomic<LONG> g_maxW{0}, g_maxH{0};          // outer window size Windows must allow while big
std::atomic<LONG> g_tx{0}, g_ty{0}, g_tw{0}, g_th{0};  // the big outer rect (screen coordinates)
// Placement before going big. Written on the render thread before the message is posted, read by the window
// thread when it handles that message (posting is a full barrier).
WINDOWPLACEMENT g_saved{};
bool g_keyWas = false;
bool g_auto = false;        // [vr] auto_big_window: follow Zwift's ride / menu state
uint32_t g_autoSeq = 0;     // last ZwiftStateSeq() acted on (render thread)
unsigned g_framesSinceRequest = 0;
LONG g_wantClientW = 0, g_wantClientH = 0;

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  // Our requests, on Zwift's window thread - outside the SwapBuffers hook, in posting order, never re-entrant.
  if (g_msg && msg == g_msg) {
    if (wp == 1) {  // go big: un-maximize first (its restore size would otherwise undo ours), then resize
      if (IsZoomed(h) || IsIconic(h)) ShowWindow(h, SW_SHOWNORMAL);
      SetWindowPos(h, nullptr, g_tx.load(), g_ty.load(), g_tw.load(), g_th.load(), SWP_NOZORDER | SWP_NOACTIVATE);
    } else {  // back: show state + normal rect together (workspace coordinates handled by Windows)
      WINDOWPLACEMENT p = g_saved;
      p.length = sizeof(p);
      p.flags = 0;
      if (p.showCmd == SW_SHOWMINIMIZED || p.showCmd == SW_MINIMIZE || p.showCmd == SW_SHOWMINNOACTIVE)
        p.showCmd = SW_SHOWNORMAL;
      SetWindowPlacement(h, &p);
    }
    return 0;
  }
  const LRESULT r = g_unicode ? CallWindowProcW(g_orig, h, msg, wp, lp) : CallWindowProcA(g_orig, h, msg, wp, lp);
  // Raise only the TRACK size cap (what SetWindowPos is held to), and only while big. The maximized size is left
  // alone, so maximizing while big still fits the monitor.
  if (msg == WM_GETMINMAXINFO && g_big.load() && lp) {
    auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
    mmi->ptMaxTrackSize.x = std::max(mmi->ptMaxTrackSize.x, g_maxW.load());
    mmi->ptMaxTrackSize.y = std::max(mmi->ptMaxTrackSize.y, g_maxH.load());
  }
  return r;
}

bool Subclass(HWND wnd) {
  if (g_wnd == wnd && g_orig) return true;
  g_unicode = IsWindowUnicode(wnd) != FALSE;
  const LONG_PTR prev = g_unicode ? SetWindowLongPtrW(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Proc))
                                  : SetWindowLongPtrA(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Proc));
  if (!prev) {
    Log("big window: could not subclass Zwift's window (error %lu)", GetLastError());
    return false;
  }
  g_orig = reinterpret_cast<WNDPROC>(prev);
  g_wnd = wnd;
  return true;
}

void MakeBig(HWND wnd) {
  RECT cr{};
  if (!g_msg || !GetClientRect(wnd, &cr) || cr.right <= 0 || cr.bottom <= 0) return;
  if (cr.right >= g_width) {  // e.g. the 5120x1440 desktop: "big" would make it smaller
    Log("big window: Zwift's window is already %ldx%ld (not narrower than big_window_width %d) - nothing to do",
        cr.right, cr.bottom, g_width);
    return;
  }
  const double aspect = static_cast<double>(cr.right) / cr.bottom;  // kept: main-camera detection uses it
  const LONG cw = g_width, ch = static_cast<LONG>(std::lround(g_width / aspect));
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(wnd, GWL_STYLE));
  const DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(wnd, GWL_EXSTYLE));
  RECT r{0, 0, cw, ch};
  AdjustWindowRectExForDpi(&r, style, FALSE, ex, GetDpiForWindow(wnd));
  const LONG ow = r.right - r.left, oh = r.bottom - r.top;
  if (!Subclass(wnd)) return;

  g_saved.length = sizeof(g_saved);
  GetWindowPlacement(wnd, &g_saved);  // exact previous state (maximized or not) for the way back
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
  g_tx.store(mi.rcWork.left);
  g_ty.store(mi.rcWork.top);
  g_tw.store(ow);
  g_th.store(oh);
  g_maxW.store(ow + 64);
  g_maxH.store(oh + 64);
  g_big.store(true);
  g_wantClientW = cw;
  g_wantClientH = ch;
  g_framesSinceRequest = 0;
  if (!PostMessageW(wnd, g_msg, 1, 0)) {
    g_big.store(false);
    Log("big window: request to Zwift's window failed (error %lu)", GetLastError());
    return;
  }
  DWORD pid = 0;
  const DWORD tid = GetWindowThreadProcessId(wnd, &pid);
  Log("big window ON: Zwift's window %ldx%ld -> client %ldx%ld (outer %ldx%ld)%s; window thread %lu, render thread %lu",
      cr.right, cr.bottom, cw, ch, ow, oh, g_saved.showCmd == SW_SHOWMAXIMIZED ? ", was maximized" : "", tid,
      GetCurrentThreadId());
}

void Restore(HWND wnd) {
  g_big.store(false);  // drops the raised size cap before the window is put back
  if (!g_msg || !PostMessageW(wnd, g_msg, 0, 0)) {
    Log("big window: restore request failed (error %lu)", GetLastError());
    return;
  }
  const RECT& n = g_saved.rcNormalPosition;
  Log("big window OFF: Zwift's window back to %ldx%ld%s", n.right - n.left, n.bottom - n.top,
      g_saved.showCmd == SW_SHOWMAXIMIZED ? " (maximized)" : "");
}

}  // namespace

void VrWindowConfigure(int width, bool autoBig) {
  g_width = std::clamp(width, 640, 7680);
  g_msg = RegisterWindowMessageW(L"zwift_vr_big_window");
  g_auto = autoBig;
  if (g_auto) ZwiftStateStart();
  Log("big window: Ctrl+Shift+Delete makes Zwift's window %d wide (sharper eye images; [vr] big_window_width)%s",
      g_width, g_auto ? "; automatic when a ride starts, normal size in the menus ([vr] auto_big_window)" : "");
}

void VrWindowOnSwap(HWND wnd) {
  if (!wnd) return;
  const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                    (GetAsyncKeyState(VK_DELETE) & 0x8000);
  if (down && !g_keyWas) {
    if (g_big.load()) Restore(g_wnd ? g_wnd : wnd);
    else MakeBig(wnd);
  }
  g_keyWas = down;

  // Automatic: act on Zwift's state CHANGES only, so the hotkey stays in effect until the next ride start / end.
  if (g_auto) {
    const uint32_t seq = ZwiftStateSeq();
    if (seq != g_autoSeq) {
      g_autoSeq = seq;
      const int riding = ZwiftStateRiding();
      if (riding == 1 && !g_big.load()) {
        Log("big window: Zwift riding -> big");
        MakeBig(wnd);
      } else if (riding == 0 && g_big.load()) {
        Log("big window: Zwift in menus -> normal size");
        Restore(g_wnd ? g_wnd : wnd);
      }
    }
  }

  // One report ~1 s after a request: did Windows / Zwift accept the size? (Zwift may clamp or refuse it.)
  if (g_big.load() && g_framesSinceRequest < 1000 && ++g_framesSinceRequest == 60) {
    RECT cr{};
    GetClientRect(wnd, &cr);
    Log("big window: client is now %ldx%ld (asked %ldx%ld)%s", cr.right, cr.bottom, g_wantClientW, g_wantClientH,
        cr.right == g_wantClientW ? "" : " - Zwift or Windows limited it");
  }
}

void VrWindowDisable() {
  if (g_big.load() && g_wnd) Restore(g_wnd);  // capture is off for good: don't leave Zwift bigger than the screen
}

#pragma once
// Big Zwift window for sharper eye images (Ctrl+Shift+Delete).
//
// The mod captures Zwift's WINDOW, so each eye image is only as sharp as the window is big - on a 1080p laptop
// panel about 1902x1033, although Zwift renders its 3D scene at USER_RESOLUTION_PREF (e.g. 3840x2160) and shrinks it
// into the window. This resizes Zwift's window to `width` (height keeps the current aspect, so the main-camera
// detection by aspect is unaffected) - larger than the screen. Windows normally caps a window at about the desktop
// size (WM_GETMINMAXINFO), and Zwift runs as admin so outside tools can't resize it; from inside the process we
// subclass the window to raise that cap. Only the top-left of the window is visible on the monitor while big, so
// set up the ride at normal size and go big for riding - automatic with `autoBig` ([vr] auto_big_window): big when
// Zwift's log says a ride started, back in its menus (zwift_state.h); the hotkey overrides until the next change.
// Restoring puts back the previous placement. The actual resize/restore runs on Zwift's own window thread (a posted
// private message), never inside the SwapBuffers hook.
#include <windows.h>

void VrWindowConfigure(int width, bool autoBig);  // client width while big ([vr] big_window_width, default 3840)
void VrWindowOnSwap(HWND wnd);      // Zwift's render thread, every SwapBuffers: polls Ctrl+Shift+Delete
void VrWindowDisable();             // swap hook's SEH handler: put Zwift's window back (posts, never blocks)

#pragma once
// Cheap CPU-side timing for the per-frame paths (diagnosing frame-rate drops).
#include <windows.h>

struct Stat {
  double sum = 0, max = 0;
  unsigned n = 0;
  void Add(double ms) {
    sum += ms;
    if (ms > max) max = ms;
    n++;
  }
  double Avg() const { return n ? sum / n : 0.0; }
  void Reset() { *this = Stat{}; }
};

inline double NowMs() {
  static const double freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

// Render-thread stats, filled by the SwapBuffers hook (dllmain) and logged by gl_capture.
inline Stat g_tFrameInterval;  // between consecutive SwapBuffers = Zwift's frame time
inline Stat g_tZwiftSwap;      // inside the original SwapBuffers (present / driver)
inline Stat g_tOurHook;        // everything we do before calling the original

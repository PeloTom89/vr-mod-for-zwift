#include "profiler.h"

#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod (excluded by WIN32_LEAN_AND_MEAN)

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "log.h"

namespace {

constexpr int kFrames = 12;     // unwound frames per sample
constexpr int kMaxSamples = 6000;
constexpr DWORD kDurationMs = 4000;

struct Sample { DWORD64 pc[kFrames]; int n; };

std::atomic<bool> g_running{false};
DWORD g_renderTid = 0;
bool g_keyWasDown = false;

std::string ModuleName(DWORD64 addr, HMODULE* outMod = nullptr) {
  HMODULE mod = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(addr), &mod) || !mod) {
    if (outMod) *outMod = nullptr;
    return "?";
  }
  if (outMod) *outMod = mod;
  char path[MAX_PATH];
  GetModuleFileNameA(mod, path, MAX_PATH);
  std::string s(path);
  return s.substr(s.find_last_of("\\/") + 1);
}

// Walks the suspended thread's stack with the x64 unwind tables. No allocation, no logging, and an
// SEH guard: a bad leaf-frame guess must not take Zwift down with an access violation.
int Unwind(CONTEXT ctx, DWORD64* pcs) {
  int n = 0;
  __try {
    for (; n < kFrames && ctx.Rip; ++n) {
      pcs[n] = ctx.Rip;
      DWORD64 base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
      if (!fn) {  // leaf function: return address is at [rsp]
        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
        ctx.Rsp += 8;
      } else {
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

DWORD WINAPI SamplerMain(void* param) {
  const DWORD tid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(param));
  HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!th) {
    Log("profiler: OpenThread(%lu) failed: %lu", tid, GetLastError());
    g_running = false;
    return 1;
  }
  std::vector<Sample> samples;
  samples.reserve(kMaxSamples);  // no allocation while the target is suspended
  timeBeginPeriod(1);
  const ULONGLONG end = GetTickCount64() + kDurationMs;
  while (GetTickCount64() < end && samples.size() < kMaxSamples) {
    if (SuspendThread(th) == static_cast<DWORD>(-1)) break;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    Sample s{};
    if (GetThreadContext(th, &ctx)) s.n = Unwind(ctx, s.pc);
    ResumeThread(th);
    if (s.n) samples.push_back(s);
    Sleep(1);
  }
  timeEndPeriod(1);
  CloseHandle(th);

  // ---- aggregate (target running again; safe to allocate/log) ----
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&SamplerMain), &self);
  HMODULE zwift = GetModuleHandleW(nullptr);
  std::map<std::string, unsigned> byLeaf, byOwner, ourRvas, zwiftRvas;
  for (const Sample& s : samples) {
    byLeaf[ModuleName(s.pc[0])]++;
    std::string owner = "(no ZwiftApp/zwift_vr frame)";
    for (int i = 0; i < s.n; ++i) {
      HMODULE m = nullptr;
      ModuleName(s.pc[i], &m);
      if (m == self) {
        char buf[48];
        sprintf_s(buf, "zwift_vr.dll+0x%llx", s.pc[i] - reinterpret_cast<DWORD64>(self));
        ourRvas[buf]++;
        owner = "zwift_vr.dll (ours)";
        break;
      }
      if (m == zwift) {
        char buf[48];
        sprintf_s(buf, "ZwiftApp+0x%llx", (s.pc[i] - reinterpret_cast<DWORD64>(zwift)) & ~0xFFFull);
        zwiftRvas[buf]++;
        owner = "ZwiftApp.exe";
        break;
      }
    }
    byOwner[owner]++;
  }
  auto dump = [&](const char* title, const std::map<std::string, unsigned>& m, size_t top) {
    std::vector<std::pair<unsigned, std::string>> v;
    for (auto& [k, n] : m) v.push_back({n, k});
    std::sort(v.rbegin(), v.rend());
    Log("profiler: %s", title);
    for (size_t i = 0; i < v.size() && i < top; ++i)
      Log("  %5.1f%%  %s", 100.0 * v[i].first / samples.size(), v[i].second.c_str());
  };
  Log("profiler: %zu samples of render thread %lu over %.1fs", samples.size(), tid, kDurationMs / 1000.0);
  if (!samples.empty()) {
    dump("by module of current instruction", byLeaf, 10);
    dump("by first frame in ZwiftApp or zwift_vr (who is responsible)", byOwner, 5);
    dump("hottest call sites inside zwift_vr.dll (RVA, see build\\zwift_vr.map)", ourRvas, 12);
    dump("hottest 4KB pages of ZwiftApp code on the stack", zwiftRvas, 8);
  }
  g_running = false;
  return 0;
}

}  // namespace

void ProfilerOnSwap() {
  g_renderTid = GetCurrentThreadId();
  const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                    (GetAsyncKeyState(VK_F8) & 0x8000);
  if (down && !g_keyWasDown && !g_running.exchange(true)) {
    Log("Ctrl+Shift+F8: profiling render thread %lu for %.1fs", g_renderTid, kDurationMs / 1000.0);
    HANDLE t = CreateThread(nullptr, 0, SamplerMain, reinterpret_cast<void*>(static_cast<uintptr_t>(g_renderTid)),
                            0, nullptr);
    if (t) CloseHandle(t); else g_running = false;
  }
  g_keyWasDown = down;
}

// zvr_loader: waits for ZwiftApp.exe and injects zwift_vr.dll (from this exe's folder)
// with the classic CreateRemoteThread(LoadLibraryW) technique.
// Must run elevated because ZwiftApp.exe does (an unelevated process can't open it to inject).
// It also does the one-time setup itself (setup.h): VR graphics settings, graphics card, headset check.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>

#include "setup.h"

static DWORD FindProcess(const wchar_t* exe) {
  DWORD pid = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe{sizeof(pe)};
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
    if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
  }
  CloseHandle(snap);
  return pid;
}

static bool HasModule(DWORD pid, const wchar_t* name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return false;
  MODULEENTRY32W me{sizeof(me)};
  bool found = false;
  for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
    if (_wcsicmp(me.szModule, name) == 0) { found = true; break; }
  }
  CloseHandle(snap);
  return found;
}

static int Fail(const char* what) {
  fprintf(stderr, "ERROR: %s (Win32 error %lu)\n", what, GetLastError());
  fprintf(stderr, "Press Enter to close.\n");
  (void)getchar();
  return 1;
}

// Injects `dll` into `pid`. Returns false (and prints why) on failure.
static bool Inject(DWORD pid, const std::wstring& dll) {
  HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
  if (!proc) { fprintf(stderr, "OpenProcess failed (%lu) - is this running as admin?\n", GetLastError()); return false; }

  const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote || !WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr)) {
    fprintf(stderr, "writing DLL path into Zwift failed (%lu)\n", GetLastError());
    CloseHandle(proc);
    return false;
  }
  // kernel32 is mapped at the same address in every process of a boot session.
  auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
  HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
  DWORD code = 0;
  if (th) {
    WaitForSingleObject(th, 15000);
    GetExitCodeThread(th, &code);  // low 32 bits of the HMODULE; 0 = LoadLibrary failed
    CloseHandle(th);
  } else {
    fprintf(stderr, "CreateRemoteThread failed (%lu)\n", GetLastError());
  }
  VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
  CloseHandle(proc);
  if (th && !code) fprintf(stderr, "LoadLibraryW inside Zwift returned NULL - the DLL failed to load.\n");
  return th && code;
}

static void PressEnter() {
  printf("\nPress Enter to close.\n");
  (void)getchar();
}

// Watch mode (default): stays open and injects into every ZwiftApp.exe that starts, so Zwift can be
// restarted (by the user, or by the restart task after a rebuild) without re-running the loader.
//   --once     inject into the current/next Zwift and exit
//   --check    report the setup without changing anything, then exit
//   --restore  put Zwift's own graphics settings back ("Restore Zwift graphics.cmd"), then exit
int wmain(int argc, wchar_t** argv) {
  bool once = false, check = false, restore = false;
  for (int i = 1; i < argc; ++i) {
    once = once || _wcsicmp(argv[i], L"--once") == 0;
    check = check || _wcsicmp(argv[i], L"--check") == 0;
    restore = restore || _wcsicmp(argv[i], L"--restore") == 0;
  }
  SetConsoleTitleW(L"VR mod for Zwift");
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const std::wstring dir = std::wstring(self).substr(0, std::wstring(self).find_last_of(L"\\/") + 1);
  const std::wstring dll = dir + L"zwift_vr.dll";

  printf("VR mod for Zwift\n================\n\n");
  DWORD running = FindProcess(L"ZwiftApp.exe");
  if (running) SetZwiftExe(ProcessImagePath(running));
  if (restore) {
    printf("Putting Zwift's own graphics settings back:\n");
    RestoreZwiftGraphics(running != 0);
    PressEnter();
    return 0;
  }
  if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return Fail("zwift_vr.dll not found next to zvr_loader.exe");

  const SetupOptions setup = ReadSetupOptions(dir);
  printf("Setup%s:\n", check ? " (--check: nothing is changed)" : "");
  RunSetup(dir, setup, running != 0, !check, false);
  CheckHeadset(false);
  if (check) { PressEnter(); return 0; }

  printf("\n%s\nLog: %%LOCALAPPDATA%%\\zwift-vr\\zwift-vr.log\n\n",
         once ? "Injecting once." : "Leave this window open while you ride - close it to stop the mod.");
  for (;;) {
    printf("Waiting for Zwift - start it as usual...\n");
    DWORD pid = 0;
    while (!(pid = FindProcess(L"ZwiftApp.exe"))) Sleep(1000);
    printf("Zwift started (pid %lu)\n", pid);
    SetZwiftExe(ProcessImagePath(pid));
    CheckHeadset(true);  // warnings only, e.g. Meta Horizon Link not running
    Sleep(3000);  // let it finish loading its imports (opengl32/gdi32) before we hook them

    if (HasModule(pid, L"zwift_vr.dll")) {
      printf("The mod is already running in this Zwift.\n");
    } else if (Inject(pid, dll)) {
      printf("The mod is running in Zwift (%ls)\n", dll.c_str());
    } else if (once) {
      return Fail("injection failed");
    }
    if (once) { Sleep(3000); return 0; }

    // Wait for this Zwift to exit, then go round again for the next launch.
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    else while (FindProcess(L"ZwiftApp.exe") == pid) Sleep(1000);
    printf("Zwift closed.\n");
    // A Zwift update (done by its launcher before the next start) can put Zwift's own graphics settings back.
    Sleep(2000);
    if (!FindProcess(L"ZwiftApp.exe")) RunSetup(dir, ReadSetupOptions(dir), false, true, true);
    printf("\n");
  }
}

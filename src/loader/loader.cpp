// zvr_loader: waits for ZwiftApp.exe and injects zwift_vr.dll (from this exe's folder)
// with the classic CreateRemoteThread(LoadLibraryW) technique.
// Must run elevated because ZwiftApp.exe does (an unelevated process can't open it to inject).
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>

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

// Watch mode (default): stays open and injects into every ZwiftApp.exe that starts, so Zwift can be
// restarted (by the user, or by the restart task after a rebuild) without re-running the loader.
// "--once" injects into the current/next Zwift and exits.
int wmain(int argc, wchar_t** argv) {
  const bool once = argc > 1 && _wcsicmp(argv[1], L"--once") == 0;
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring dll(self);
  dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"zwift_vr.dll";
  if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return Fail("zwift_vr.dll not found next to zvr_loader.exe");

  printf("zwift-vr loader - %s\nLog: %%LOCALAPPDATA%%\\zwift-vr\\zwift-vr.log\n\n",
         once ? "injecting once" : "watching for Zwift (leave this open; close the window to stop)");
  for (;;) {
    printf("Waiting for ZwiftApp.exe (start Zwift from its launcher)...\n");
    DWORD pid = 0;
    while (!(pid = FindProcess(L"ZwiftApp.exe"))) Sleep(1000);
    printf("Found ZwiftApp.exe, pid %lu\n", pid);
    Sleep(3000);  // let it finish loading its imports (opengl32/gdi32) before we hook them

    if (HasModule(pid, L"zwift_vr.dll")) {
      printf("Already injected.\n");
    } else if (Inject(pid, dll)) {
      printf("Injected %ls\n", dll.c_str());
    } else if (once) {
      return Fail("injection failed");
    }
    if (once) { Sleep(3000); return 0; }

    // Wait for this Zwift to exit, then go round again for the next launch.
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    else while (FindProcess(L"ZwiftApp.exe") == pid) Sleep(1000);
    printf("Zwift (pid %lu) exited.\n\n", pid);
  }
}

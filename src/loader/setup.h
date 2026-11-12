#pragma once
// What zvr_loader.exe sets up by itself, so installing is: unzip, double-click zvr_loader.exe, start Zwift.
//  - VR graphics settings: config/zwift-vr-graphics.txt (built into the loader) replaces Zwift's ultra/high/medium/
//    basic.txt graphics profiles. Zwift's own are kept as <name>.txt.zwiftvr-backup and the VR one is put back after
//    a Zwift update. [setup] vr_graphics = 0 (or "Restore Zwift graphics.cmd") puts Zwift's own back.
//  - Windows' graphics preference "High performance" for ZwiftApp.exe, for laptops with two graphics chips - only
//    when Windows has none for Zwift yet ([setup] gpu_high_performance).
//  - The download's "Unblock" flag (Mark of the Web) removed from the files next to the loader.
//  - The active OpenXR runtime and, for Meta Horizon Link, whether it is running: reported, never changed.
#include <windows.h>

#include <string>

struct SetupOptions {
  bool vrGraphics = true;
  bool gpuHighPerformance = true;
};
SetupOptions ReadSetupOptions(const std::wstring& modDir);  // zwift_vr.ini [setup], next to the loader

// Zwift's folder (with a trailing backslash), from a running ZwiftApp.exe. Otherwise the installer's registry entry
// or Program Files (x86)\Zwift is used.
void SetZwiftExe(const std::wstring& exePath);
std::wstring ProcessImagePath(DWORD pid);

// One pass of the checks above. apply = false only reports what would change (--check). quiet = print only what
// changed or needs attention (the re-check after Zwift closes). Zwift must not be running for the graphics part.
void RunSetup(const std::wstring& modDir, const SetupOptions& o, bool zwiftRunning, bool apply, bool quiet);

// Headset runtime + Meta Horizon Link running. quiet = only warnings.
void CheckHeadset(bool quiet);

// Zwift's own graphics settings back from the backups (--restore). False if nothing could be restored.
bool RestoreZwiftGraphics(bool zwiftRunning);

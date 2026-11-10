#pragma once
#include <windows.h>

#include "config.h"

// Entry point of the OpenXR thread. Runs for the life of the process: creates the
// instance/session (retrying while the headset is off), owns the D3D11 device, and each
// frame copies the captured Zwift image into a quad layer in front of the user.
DWORD WINAPI XrThreadMain(void* config /* Config*, owned by the caller, never freed */);

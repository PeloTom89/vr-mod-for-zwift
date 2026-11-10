#pragma once
// Tiny thread-safe file logger: %LOCALAPPDATA%\zwift-vr\zwift-vr.log (truncated per Zwift launch).
void LogInit();
void Log(const char* fmt, ...);

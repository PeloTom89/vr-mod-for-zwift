#include "config.h"

#include <windows.h>
#include <algorithm>
#include <cwchar>
#include <string>

#include "log.h"

static float ReadFloat(const wchar_t* ini, const wchar_t* section, const wchar_t* key, float def) {
  wchar_t buf[64];
  GetPrivateProfileStringW(section, key, L"", buf, 64, ini);
  if (!buf[0]) return def;
  wchar_t* end = nullptr;
  float v = wcstof(buf, &end);
  return end != buf ? v : def;
}

// Accepts "16:9", "1.778" or "0" (off).
static float ReadAspect(const wchar_t* ini, const wchar_t* section, const wchar_t* key) {
  wchar_t buf[64];
  GetPrivateProfileStringW(section, key, L"0", buf, 64, ini);
  wchar_t* end = nullptr;
  float a = wcstof(buf, &end);
  if (end == buf) return 0.0f;
  if (*end == L':') {
    float b = wcstof(end + 1, nullptr);
    return b > 0.0f ? a / b : 0.0f;
  }
  return a > 0.0f ? a : 0.0f;
}

Config LoadConfig() {
  Config c;
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&LoadConfig), &self);
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(self, path, MAX_PATH);
  std::wstring ini(path);
  ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"zwift_vr.ini";

  c.distance = ReadFloat(ini.c_str(), L"screen", L"distance", c.distance);
  c.width = ReadFloat(ini.c_str(), L"screen", L"width", c.width);
  c.height = ReadFloat(ini.c_str(), L"screen", L"height", c.height);
  c.cropAspect = ReadAspect(ini.c_str(), L"screen", L"crop_aspect");
  c.stereo = ReadFloat(ini.c_str(), L"depth", L"stereo", 1.0f) != 0.0f;
  c.ipdMm = ReadFloat(ini.c_str(), L"depth", L"ipd_mm", c.ipdMm);
  c.strength = ReadFloat(ini.c_str(), L"depth", L"strength", c.strength);
  c.convergenceM = ReadFloat(ini.c_str(), L"depth", L"convergence", c.convergenceM);
  wchar_t mode[32];
  GetPrivateProfileStringW(L"vr", L"mode", L"immersive", mode, 32, ini.c_str());
  c.immersive = _wcsicmp(mode, L"screen") != 0;
  c.worldScale = ReadFloat(ini.c_str(), L"vr", L"world_scale", c.worldScale);
  c.parallax = std::clamp(ReadFloat(ini.c_str(), L"vr", L"parallax", c.parallax), 0.0f, 2.0f);
  c.recentrePosition = ReadFloat(ini.c_str(), L"vr", L"recentre_position", 1.0f) != 0.0f;
  c.coherentMode =
      std::clamp(static_cast<int>(ReadFloat(ini.c_str(), L"vr", L"coherent_when_moving", 2.0f) + 0.5f), 0, 2);
  c.cropToEyeFov = ReadFloat(ini.c_str(), L"vr", L"crop_to_eye_fov", 1.0f) != 0.0f;
  c.bigWindowWidth = static_cast<int>(ReadFloat(ini.c_str(), L"vr", L"big_window_width", 3840.0f));
  c.autoScreenInMenus = ReadFloat(ini.c_str(), L"vr", L"auto_screen_in_menus", 1.0f) != 0.0f;
  c.autoBigWindow = ReadFloat(ini.c_str(), L"vr", L"auto_big_window", 1.0f) != 0.0f;
  c.eyeResolution = std::clamp(ReadFloat(ini.c_str(), L"vr", L"eye_resolution", 1.0f), 0.0f, 4.0f);
  Log("config [vr]: mode=%ls world_scale=%.2f parallax=%.2f recentre_position=%d coherent_when_moving=%d "
      "crop_to_eye_fov=%d",
      c.immersive ? L"immersive" : L"screen", c.worldScale, c.parallax, c.recentrePosition, c.coherentMode,
      c.cropToEyeFov);
  c.shadows = ReadFloat(ini.c_str(), L"render", L"shadows", 1.0f) != 0.0f;
  c.reflections = ReadFloat(ini.c_str(), L"render", L"reflections", 1.0f) != 0.0f;
  Log("config [render]: shadows=%d reflections=%d", c.shadows, c.reflections);

  c.hud = ReadFloat(ini.c_str(), L"hud", L"enabled", 1.0f) != 0.0f;
  c.hudCompositeDraws = std::clamp(static_cast<int>(ReadFloat(ini.c_str(), L"hud", L"composite_draws", 1.0f)), 0, 64);
  c.hudWindowAlpha = ReadFloat(ini.c_str(), L"hud", L"window_alpha", 1.0f) != 0.0f;
  for (int i = 1; i <= Config::kMaxHudCards; ++i) {
    wchar_t key[16], buf[256];
    swprintf_s(key, L"card%d", i);
    GetPrivateProfileStringW(L"hud", key, L"", buf, 256, ini.c_str());
    if (!buf[0]) continue;
    Config::HudCard k{};
    // "x0 y0 x1 y1  right up ahead  width" - anything after a ';' is a comment
    if (wchar_t* semi = wcschr(buf, L';')) *semi = 0;
    if (swscanf_s(buf, L"%f %f %f %f %f %f %f %f", &k.x0, &k.y0, &k.x1, &k.y1, &k.px, &k.py, &k.dist, &k.width) != 8 ||
        k.x1 <= k.x0 || k.y1 <= k.y0 || k.width <= 0.0f || k.dist <= 0.0f) {
      Log("config [hud]: %ls ignored - needs 8 numbers: x0 y0 x1 y1 right up ahead width", key);
      continue;
    }
    k.x0 = std::clamp(k.x0, 0.0f, 1.0f);
    k.y0 = std::clamp(k.y0, 0.0f, 1.0f);
    k.x1 = std::clamp(k.x1, 0.0f, 1.0f);
    k.y1 = std::clamp(k.y1, 0.0f, 1.0f);
    c.hudCards[c.hudCardCount++] = k;
  }
  if (c.hudCardCount == 0) c.hudCards[c.hudCardCount++] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, -0.1f, 1.0f, 1.2f};
  c.hudSpread = std::clamp(ReadFloat(ini.c_str(), L"hud", L"spread", 1.0f), 0.5f, 2.5f);
  Log("config [hud]: enabled=%d composite_draws=%d, %d card(s), spread %.2f", c.hud, c.hudCompositeDraws,
      c.hudCardCount, c.hudSpread);
  for (int i = 0; i < c.hudCardCount; ++i) {
    const Config::HudCard& k = c.hudCards[i];
    Log("  card%d: window %.2f,%.2f-%.2f,%.2f -> %.2f m right, %.2f m up, %.2f m ahead, %.2f m wide", i + 1, k.x0, k.y0,
        k.x1, k.y1, k.px, k.py, k.dist, k.width);
  }
  Log("config %ls: distance=%.2f width=%.2f height=%.2f crop_aspect=%.3f | stereo=%d ipd_mm=%.1f strength=%.2f "
      "convergence=%.2f",
      ini.c_str(), c.distance, c.width, c.height, c.cropAspect, c.stereo, c.ipdMm, c.strength, c.convergenceM);
  return c;
}

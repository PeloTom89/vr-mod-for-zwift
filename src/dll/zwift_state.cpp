#include "zwift_state.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <string>

#include "log.h"

namespace {

std::atomic<int> g_riding{-1};
std::atomic<uint32_t> g_seq{0};

void Set(int riding, const std::string& why) {
  if (g_riding.load() == riding) return;
  g_riding.store(riding);
  g_seq.fetch_add(1);
  Log("zwift state: %s (%s)", riding ? "RIDING" : "MENUS", why.c_str());
}

// One complete log line (no line break).
void OnLine(const std::string& line) {
  if (line.find("[GameState]") == std::string::npos) return;
  const size_t arrow = line.find(" -> ZSF_");
  if (arrow != std::string::npos) {
    size_t end = arrow + 4;
    while (end < line.size() && (isupper(static_cast<unsigned char>(line[end])) || line[end] == '_')) ++end;
    const std::string to = line.substr(arrow + 4, end - (arrow + 4));
    Set(to == "ZSF_RIDE" ? 1 : 0, to);
    return;
  }
  // Not "Starting Ride": Zwift logs it when the drop-in starts loading the world, ~25 s before ZSF_RIDE, so
  // immersive mode would show the loading screen around you (seen 2026-11-09).
  if (line.find("Ending Ride") != std::string::npos) Set(0, "Ending Ride");
  else if (line.find("EndGameSession") != std::string::npos) Set(0, "EndGameSession");
}

DWORD WINAPI ReaderThread(void*) {
  wchar_t dir[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) { Log("zwift state: no LOCALAPPDATA"); return 1; }
  const std::wstring path = std::wstring(dir) + L"\\Zwift\\Logs\\Log.txt";
  LONGLONG pos = 0;
  FILETIME created{};
  std::string partial;
  bool logged = false;
  for (;;) {
    // Shared read: Zwift keeps writing (and at its next launch renames) the file.
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      if (!logged) { Log("zwift state: reading %ls", path.c_str()); logged = true; }
      BY_HANDLE_FILE_INFORMATION fi{};
      LARGE_INTEGER size{};
      if (GetFileInformationByHandle(f, &fi) && GetFileSizeEx(f, &size)) {
        // A new file (Zwift relaunched) or a shorter one: start over from its beginning.
        if (CompareFileTime(&fi.ftCreationTime, &created) != 0 || size.QuadPart < pos) {
          created = fi.ftCreationTime;
          pos = 0;
          partial.clear();
        }
        if (size.QuadPart > pos) {
          LARGE_INTEGER at{};
          at.QuadPart = pos;
          SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
          char buf[64 * 1024];
          DWORD got = 0;
          while (pos < size.QuadPart && ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) {
            pos += got;
            partial.append(buf, got);
            size_t start = 0, nl;
            while ((nl = partial.find('\n', start)) != std::string::npos) {
              OnLine(partial.substr(start, nl - start));
              start = nl + 1;
            }
            partial.erase(0, start);
            if (partial.size() > 1 << 20) partial.clear();  // no line is this long; don't grow forever
          }
        }
      }
      CloseHandle(f);
    }
    Sleep(250);
  }
}

}  // namespace

void ZwiftStateStart() {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  HANDLE t = CreateThread(nullptr, 0, ReaderThread, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
}

int ZwiftStateRiding() { return g_riding.load(); }
uint32_t ZwiftStateSeq() { return g_seq.load(); }

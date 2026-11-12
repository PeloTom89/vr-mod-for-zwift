#include "camera.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "gl_trace.h"
#include "gl_util.h"
#include "log.h"
#include "shared.h"
#include "zwift_state.h"

using namespace glu;

namespace {

bool g_enabled = false;
bool g_keyWasDown = false;
// [vr] auto_screen_in_menus: follow Zwift's ride/menu state (zwift_state.h). Applied on state CHANGES only, so a
// manual Ctrl+Shift+F10 holds until Zwift next enters or leaves a ride.
bool g_autoScreen = false;
uint32_t g_autoSeq = 0;
float g_cmPerMetre = 100.0f;  // Zwift world units are cm (phase-2/3 recon); worldScale multiplies


// Zwift's own (original) camera position + rotation per overridden frame, to report how far it moves /
// turns between consecutive (= alternate-eye) frames, and to switch to COHERENT while it moves.
float g_frameCam[3], g_lastCam[3], g_frameRot[9], g_lastRot[9], g_frameFwd[3];
bool g_haveFrameCam = false, g_haveLastCam = false;
double g_camStepSum = 0, g_camTurnSum = 0;
unsigned g_camSteps = 0, g_camCuts = 0;
float g_camStepMax = 0, g_camTurnMax = 0;
unsigned g_framesCoherent = 0;
uint64_t g_frameNo = 0;

// Last 30 per-frame (step cm, turn deg) samples of Zwift's ORIGINAL camera; camera cuts (> 10 m) skipped.
struct Motion {
  float step[30] = {}, turn[30] = {};
  int n = 0, next = 0;
  void Add(float s, float t) { step[next] = s; turn[next] = t; next = (next + 1) % 30; n = std::min(n + 1, 30); }
  void Clear() { n = 0; next = 0; }
  // averages over the newest k samples
  void Avg(int k, float* s, float* t) const {
    k = std::min(k, n);
    double a = 0, b = 0;
    for (int i = 0; i < k; ++i) { const int j = (next - 1 - i + 30) % 30; a += step[j]; b += turn[j]; }
    *s = k ? static_cast<float>(a / k) : 0.0f;
    *t = k ? static_cast<float>(b / k) : 0.0f;
  }
};
Motion g_motion;

// Eye origin: Zwift's camera sits at g_home (LOCAL, metres) instead of at the runtime's LOCAL origin, so a
// head that is 45 cm in front of the last Quest recentre doesn't push you 45 cm into the chase camera.
bool g_recentre = true;
float g_home[3] = {};
bool g_haveHome = false, g_rehome = false;
float g_parallax = 1.0f;  // eye-separation scale (Ctrl+Shift+PageUp/PageDown)
float g_lastMid[3] = {};  // latest mid-eye position (LOCAL, metres), for the condition line

// COHERENT: while Zwift's camera moves, alternate-eye pairs come from frames 20-50 cm apart and can't
// fuse (sign-flipping disparity at ~15 Hz). Then both eyes get the newest frame, rendered from the mid-eye
// with no parallax: flat, but fused and updated at Zwift's full rate. Stereo returns when it stops.
// Mode 2 (always) trades the 3D you'd get while standing still for Zwift's full rate in each eye: alternate-eye
// stereo gives each eye only every other frame, so riders passing a stopped rider jump (~20 updates/s).
int g_coherentMode = 0;  // 0 never, 1 while moving, 2 always ([vr] coherent_when_moving; no hotkey)
bool g_coherentActive = false;
ULONGLONG g_motionSince = 0, g_stillSince = 0;

const char* CoherentName(int m) {
  return m == 2 ? "ALWAYS (both eyes from every frame, no 3D)"
       : m == 1 ? "WHEN MOVING (both eyes from every frame while Zwift's camera moves, 3D when still)"
                : "OFF (alternate-eye 3D always)";
}

// Test keys. F3/F4/F5 belong to reflections / lighting-ray A/B / shadows, so these live elsewhere.
bool g_f1Was = false, g_pgUpWas = false, g_pgDnWas = false, g_homeWas = false, g_endWas = false;

struct Latch {
  int eye = -1;  // 0/1 = that eye, 2 = COHERENT (both eyes)
  float H[16], Hinv[16], Pp[16], C[16];
  bool haveC = false;
  bool stereo = false;  // this frame renders the symmetric eye frustum (needs the scale field)
  float parallax = 1.0f;
  XrPosef pose{};      // the pose SUBMITTED for this image (the runtime's eye pose; mid-eye in COHERENT)
  XrFovf fov{};        // the eye's real (asymmetric) FOV from the runtime (union of both in COHERENT)
  XrFovf renderFov{};  // the frustum actually rendered = what gets submitted for this frame
};
Latch g_cur;
int g_nextEye = 0;

// Zwift's own main-camera projection (changes continuously: Zwift varies its FOV while riding) and
// latest main view.
float g_P0[16], g_P0inv[16];
bool g_haveP0 = false;
float g_windowAspect = 0.0f;
bool g_lastProjMain = true;  // was the most recent g_PROJ upload the main camera's?
unsigned g_nP0Changes = 0, g_nOtherProj = 0;
float g_m0Min = 1e9f, g_m0Max = 0.0f;
float g_V[16], g_Vinv[16];
bool g_haveV = false;

struct ProgInfo {
  GLint w2v = -1, proj = -1, wvp = -1, l2w = -1, refl = -1;
  bool projPatched = false;  // this program's last g_PROJ upload was replaced with the eye projection
};
std::unordered_map<GLint, ProgInfo> g_progs;

// ---- mapped-buffer camera (B_Scene / B_Object), layout from the phase-3 camera hunt ----
constexpr GLenum kSsbo = 0x90D2, kUbo = 0x8A11;
constexpr GLuint kSceneBinding = 5, kObjectBinding = 1;
constexpr ptrdiff_t kSceneV = 32, kSceneP = 176, kScenePinv = 240, kSceneMin = 992;
constexpr ptrdiff_t kSceneSearch = 2048;  // bytes of a B_Scene slice searched for the other fields
constexpr ptrdiff_t kObjectVP = 64, kObjectMin = 128;

// B_Scene is CAMERA-RELATIVE (diagnosis workflow 2026-11-08, Zwift's own GLSL in global.wad):
//   Transform(view, v) = mat3(view.m) * (v.xyz - view.viewPos * v.w)
//   Project(proj, v)   = v.xyzz * proj.scale + proj.bias       (diagonal only: no off-axis term)
// so the world only ever saw the ROTATION of our V@+32 write. The eye position must go into viewPos,
// the frustum into scale (and it must be symmetric). Their offsets are found by value once per session
// (expected viewPos +96, dir +112, scale +144, bias +160; +928 only if it really is V^-1).
struct SceneLayout { bool searched = false; int pos = -1, dir = -1, scale = -1, vinv = -1; };
SceneLayout g_lay;

struct MappedBuf {
  bool queried = false;
  uint8_t* ptr = nullptr;
  long long mapOffset = 0, mapLength = 0;
  GLint access = 0;
  unsigned framesSinceQuery = 0;
};
std::unordered_map<GLuint, MappedBuf> g_mapped;
std::unordered_set<long long> g_patchedScene, g_patchedObj;  // slices patched this frame
enum SliceKind { kSliceOther, kSliceMain, kSliceRefl };
std::unordered_map<long long, int> g_sliceKind;  // B_Scene slice offset -> kind, this frame
constexpr ptrdiff_t kSceneClipPlane = 352;
constexpr ptrdiff_t kSceneFogColor = 448;  // vec4 fogColor (B_Scene std430, from the resolve shader s765)
bool g_reflSkip = true;   // [render] reflections = 0 -> skip the water-reflection pass
bool g_raysLegacy = false;  // Ctrl+Shift+F4: Zwift's own deferred-resolve rays (A/B)
bool g_f3WasDown = false, g_f4WasDown = false;
// Original P*V of every camera slice (main + water reflection) patched this frame and last frame, to
// recognise the B_Objects drawn with them. (v2 kept only one: the reflection slice overwrote the
// main camera's and ~77% of the world's objects were rejected as "other camera".)
struct PVList {
  float m[8][16];
  int n = 0;
  void Add(const float* pv) { if (n < 8) memcpy(m[n++], pv, 64); }
  bool Match(const float* vp) const {
    for (int i = 0; i < n; ++i) if (Diff(vp, m[i]) < 5e-2) return true;
    return false;
  }
};
PVList g_pvThis, g_pvPrev;
bool g_havePV = false;

// A slice Zwift rebinds without rewriting it still holds OUR V from an earlier frame: patching it again
// would compound H (and the viewPos delta). Remember what we wrote per offset and skip such rebinds.
struct StaleEntry { long long offset = -1; float V2[16]; };
StaleEntry g_stale[8];
int g_staleNext = 0;

// Swap-time readback probes: did what we wrote at bind time survive until the end of the frame?
enum ProbeKind { kProbeV, kProbePos, kProbeScale, kProbeObj, kProbeKinds };
struct Probe { float* addr; float wrote[4]; float orig[4]; int kind; };
Probe g_probes[64];
int g_nProbes = 0, g_nObjProbes = 0;
unsigned g_probeCounts[kProbeKinds][3] = {};  // intact / reverted / other

// stats
unsigned g_nSceneMain = 0, g_nSceneRefl = 0, g_nObjWvp = 0;
unsigned g_nScene = 0, g_nObj = 0, g_nObjOther = 0, g_nObjNoPtr = 0;
unsigned g_nSceneReject = 0, g_nSceneStale = 0, g_nPos = 0, g_nPosSkip = 0, g_nDir = 0, g_nScale = 0,
         g_nScaleSkip = 0, g_nVinv = 0;
bool g_loggedLayout = false;
unsigned g_nW2V = 0, g_nProj = 0, g_nWVP = 0, g_nL2W = 0, g_nRefl = 0, g_frames = 0;
unsigned g_nW2VOther = 0, g_nWVPOther = 0;  // uniform uploads of other cameras (shadow/spotlight) left alone
ULONGLONG g_statsSince = 0;

// HUD phase (CamEnterHudPhase until the swap): every camera upload and new B_Scene slice is the HUD's own 3D render
// (jersey icons) and stays on Zwift's camera. Counted to show it happens: perspective PROJ, other W2V/WVP, slices.
bool g_hudPhase = false;
unsigned g_nHudProj = 0, g_nHudOther = 0, g_nHudSlices = 0;

// [vr] steady_camera (see Steady() below): Zwift's camera shake filtered out of the camera each frame renders from.
float g_steadyTau = 0.1f;     // filter time constant, seconds; 0 = off
float g_H0[16], g_H0inv[16];  // this frame's head-only transform, latched at swap; g_cur.H = g_H0 * S once V is seen
bool g_steadyFrame = false;   // S already applied for this frame
float g_steadyV[16];          // the raw V it was computed from
struct SteadyState {
  bool have = false;
  double c[3] = {}, v[3] = {};      // filtered camera position (world cm) and velocity (cm/s)
  double q[4] = {1, 0, 0, 0};       // filtered camera->world rotation (w x y z)
  double w[3] = {};                 // its angular velocity (world frame, rad/s)
  double rawC[3] = {}, rawQ[4] = {1, 0, 0, 0};  // the previous frame's raw camera (velocities after a snap)
  LARGE_INTEGER t{};
};
SteadyState g_st;
unsigned g_stFrames = 0, g_stSnaps = 0, g_stEarly = 0, g_stMismatch = 0;  // stats, 10 s window
double g_stPosSum = 0, g_stRotSum = 0;
float g_stPosMax = 0, g_stRotMax = 0;

// Shadow-cascade evidence: does Zwift's sun-cascade origin (B_Scene ShadowViewPos[0], +976 by the
// captured struct layout) move while only the head moves? Range over each 10 s window, with the head-yaw
// range for comparison.
constexpr ptrdiff_t kSceneShadowViewPos0 = 976;
float g_svpMin[3], g_svpMax[3];
bool g_haveSvp = false;
float g_yawMin = 1e9f, g_yawMax = -1e9f;

void PoseToMatrix(const XrPosef& p, float scale, float* m) {
  const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
  m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + w * z);     m[2] = 2 * (x * z - w * y);     m[3] = 0;
  m[4] = 2 * (x * y - w * z);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + w * x);     m[7] = 0;
  m[8] = 2 * (x * z + w * y);     m[9] = 2 * (y * z - w * x);     m[10] = 1 - 2 * (x * x + y * y); m[11] = 0;
  m[12] = p.position.x * scale;   m[13] = p.position.y * scale;   m[14] = p.position.z * scale;   m[15] = 1;
}

void UpdateC() {
  g_cur.haveC = false;
  if (g_cur.eye < 0 || !g_haveP0) return;
  float t[16];
  Mul(g_cur.Pp, g_cur.H, t);
  Mul(t, g_P0inv, g_cur.C);
  g_cur.haveC = true;
}

// The projection this frame renders with (Pp) and the matching FOV to submit (renderFov).
// Zwift's world Project() is diagonal-only, so an off-axis eye frustum cannot be rendered: STEREO uses
// the smallest SYMMETRIC frustum enclosing the eye's FOV and submits exactly that; MONO (or no scale
// field) keeps Zwift's own frustum. Pp starts as a copy of P0, keeping its m8/m9/m10/m11/m14 terms.
void ComputeEyeProjection() {
  if (!g_haveP0) {  // nothing to base it on yet: identity-like placeholder, C stays unavailable
    memset(g_cur.Pp, 0, sizeof(g_cur.Pp));
    g_cur.renderFov = g_cur.fov;
    UpdateC();
    return;
  }
  memcpy(g_cur.Pp, g_P0, sizeof(g_P0));
  if (g_cur.stereo) {
    const float th = std::max(std::tan(-g_cur.fov.angleLeft), std::tan(g_cur.fov.angleRight));
    const float tv = std::max(std::tan(g_cur.fov.angleUp), std::tan(-g_cur.fov.angleDown));
    g_cur.Pp[0] = 1.0f / th;
    g_cur.Pp[5] = std::copysign(1.0f / tv, g_P0[5]);
    g_cur.renderFov = {-std::atan(th), std::atan(th), std::atan(tv), -std::atan(tv)};
  } else {  // Zwift's frustum: ndc.x = m0*tan - m8  =>  edges at tan = (m8 -+ 1)/m0
    g_cur.renderFov = {std::atan((g_P0[8] - 1.0f) / g_P0[0]), std::atan((g_P0[8] + 1.0f) / g_P0[0]),
                       std::atan((g_P0[9] + 1.0f) / std::fabs(g_P0[5])),
                       std::atan((g_P0[9] - 1.0f) / std::fabs(g_P0[5]))};
  }
  UpdateC();
}

ProgInfo& Info(GLint prog) {  // unordered_map nodes are stable: the reference stays valid
  auto it = g_progs.find(prog);
  if (it != g_progs.end()) return it->second;
  ProgInfo pi;
  for (auto& [loc, raw] : UniformNames(prog)) {
    std::string name = raw;
    if (name.size() > 3 && name.compare(name.size() - 3, 3, "[0]") == 0) name.resize(name.size() - 3);
    if (name == "g_W2V") pi.w2v = loc;
    else if (name == "g_PROJ") pi.proj = loc;
    else if (name == "g_WVP") pi.wvp = loc;
    else if (name == "g_L2W") pi.l2w = loc;
    else if (name == "reflectedViewMatrix") pi.refl = loc;
  }
  return g_progs.emplace(prog, pi).first->second;
}

float Det3(const float* m) {
  return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

float Rel(float a, float b) { return std::fabs(a - b) / std::max(std::fabs(b), 1e-6f); }

// --- mapped-buffer access (no C++ objects in the SEH function) ---
bool SafeCopy(void* dst, const void* src, size_t n) {
  __try {
    memcpy(dst, src, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
bool SafeRead(float* dst, const float* src) { return SafeCopy(dst, src, 64); }
bool SafeWrite(float* dst, const float* src) { return SafeCopy(dst, src, 64); }

void AddProbe(float* addr, const float* wrote, const float* orig, int kind) {
  if (g_nProbes >= 64) return;
  Probe& p = g_probes[g_nProbes++];
  p.addr = addr;
  memcpy(p.wrote, wrote, 16);
  memcpy(p.orig, orig, 16);
  p.kind = kind;
}

// At swap (frame done, before Zwift writes the next one): classify every probe.
void CheckProbes() {
  for (int i = 0; i < g_nProbes; ++i) {
    const Probe& p = g_probes[i];
    float now[4];
    if (!SafeCopy(now, p.addr, 16)) continue;
    const int cls = memcmp(now, p.wrote, 16) == 0 ? 0 : memcmp(now, p.orig, 16) == 0 ? 1 : 2;
    g_probeCounts[p.kind][cls]++;
  }
  g_nProbes = 0;
  g_nObjProbes = 0;
}

using PFN_GetNamedBufferPointerv = void(APIENTRY*)(GLuint, GLenum, void**);
using PFN_GetNamedBufferParameteri64v = void(APIENTRY*)(GLuint, GLenum, long long*);
using PFN_GetNamedBufferParameteriv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_FlushMappedNamedBufferRange = void(APIENTRY*)(GLuint, ptrdiff_t, ptrdiff_t);

// CPU address of [offset, offset+len) of a persistently mapped buffer, or null.
float* CpuPtr(GLuint buf, ptrdiff_t offset, ptrdiff_t len) {
  MappedBuf& m = g_mapped[buf];
  // (re)query once, then every ~600 frames in case Zwift remaps (the queries may sync the driver)
  if (!m.queried || (!m.ptr && m.framesSinceQuery > 600)) {
    static auto getPtr = reinterpret_cast<PFN_GetNamedBufferPointerv>(wglGetProcAddress("glGetNamedBufferPointerv"));
    static auto get64 =
        reinterpret_cast<PFN_GetNamedBufferParameteri64v>(wglGetProcAddress("glGetNamedBufferParameteri64v"));
    static auto geti = reinterpret_cast<PFN_GetNamedBufferParameteriv>(wglGetProcAddress("glGetNamedBufferParameteriv"));
    m = MappedBuf{};
    m.queried = true;
    if (getPtr && get64 && geti) {
      void* p = nullptr;
      getPtr(buf, 0x88BD /* BUFFER_MAP_POINTER */, &p);
      get64(buf, 0x9121 /* BUFFER_MAP_OFFSET */, &m.mapOffset);
      get64(buf, 0x9120 /* BUFFER_MAP_LENGTH */, &m.mapLength);
      geti(buf, 0x911F /* BUFFER_ACCESS_FLAGS */, &m.access);
      m.ptr = static_cast<uint8_t*>(p);
    }
    Log("camera: buffer %u mapping: ptr %p offset %lld length %lld access 0x%X%s", buf, m.ptr, m.mapOffset,
        m.mapLength, m.access, (m.access & 0x80) ? " COHERENT" : (m.access & 0x10) ? " FLUSH_EXPLICIT" : "");
  }
  if (!m.ptr || offset < m.mapOffset || offset + len > m.mapOffset + m.mapLength) return nullptr;
  return reinterpret_cast<float*>(m.ptr + (offset - m.mapOffset));
}

void FlushIfNeeded(GLuint buf, ptrdiff_t offset, ptrdiff_t len) {
  const MappedBuf& m = g_mapped[buf];
  if (!(m.access & 0x10 /* MAP_FLUSH_EXPLICIT */)) return;  // coherent / auto-flushed mappings need nothing
  static auto flush =
      reinterpret_cast<PFN_FlushMappedNamedBufferRange>(wglGetProcAddress("glFlushMappedNamedBufferRange"));
  if (flush) flush(buf, offset - static_cast<ptrdiff_t>(m.mapOffset), len);
}

// Same rotation (per element) and nearby translation (cm).
bool CloseRigid(const float* a, const float* b, float rotTol, float transTol) {
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r)
      if (std::fabs(a[c * 4 + r] - b[c * 4 + r]) > rotTol) return false;
  return std::fabs(a[12] - b[12]) < transTol && std::fabs(a[13] - b[13]) < transTol &&
         std::fabs(a[14] - b[14]) < transTol;
}

// Once per session, on the first validated MAIN B_Scene slice (original values, before we write):
// find viewPos / dir / scale(+bias) / V^-1 by value, and log the slice head so a miss can be resolved
// offline without another headset session.
void LocateSceneFields(const float* base, ptrdiff_t avail, const float* V, const float* P) {
  g_lay.searched = true;
  const int nFloats = static_cast<int>(std::min(avail, kSceneSearch) / 4);
  float s[kSceneSearch / 4];
  if (!SafeCopy(s, base, static_cast<size_t>(nFloats) * 4)) {
    Log("camera: B_Scene field search: slice unreadable");
    return;
  }
  float Vinv[16];
  Inverse(V, Vinv);
  const float c[3] = {Vinv[12], Vinv[13], Vinv[14]};   // camera position (cm)
  const float f[3] = {-V[2], -V[6], -V[10]};            // camera forward in world
  for (int k = 0; k + 4 <= nFloats; k += 4) {
    const int off = k * 4;
    // skip the known matrices (+928 too: its columns look like dir / viewPos when it is V^-1)
    if ((off >= kSceneV && off < kSceneV + 64) || (off >= kSceneP && off < kScenePinv + 64) || (off >= 928 && off < 992))
      continue;
    const float* q = s + k;
    if (g_lay.pos < 0 && std::fabs(q[0] - c[0]) < 10 && std::fabs(q[1] - c[1]) < 10 && std::fabs(q[2] - c[2]) < 10)
      g_lay.pos = off;
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (g_lay.dir < 0 && std::fabs(len - 1.0f) < 1e-3f && std::fabs(q[0] * f[0] + q[1] * f[1] + q[2] * f[2]) > 0.999f)
      g_lay.dir = off;
    if (g_lay.scale < 0 && k + 8 <= nFloats && Rel(q[0], P[0]) < 1e-4f && Rel(q[1], P[5]) < 1e-4f &&
        Rel(q[2], P[10]) < 1e-4f && Rel(q[3], P[11]) < 1e-4f) {
      const float* b = q + 4;
      if (std::fabs(b[0]) < 1e-2f && std::fabs(b[1]) < 1e-2f && std::fabs(b[2] - P[14]) < 1e-3f && std::fabs(b[3]) < 1e-3f)
        g_lay.scale = off;
    }
  }
  if (nFloats >= 928 / 4 + 16) {  // +928 only if its 3x3 really is R_V^T (the tracer's match ignored rotation)
    const float* X = s + 928 / 4;
    bool ok = true;
    for (int r = 0; r < 3 && ok; ++r)
      for (int cc = 0; cc < 3 && ok; ++cc) ok = std::fabs(X[cc * 4 + r] - V[r * 4 + cc]) < 1e-3f;
    if (ok) g_lay.vinv = 928;
  }
  Log("camera: B_Scene fields: viewPos %+d, dir %+d, scale %+d (bias +16), V^-1 %+d   (-1 = not found)", g_lay.pos,
      g_lay.dir, g_lay.scale, g_lay.vinv);
  for (int k = 0; k + 4 <= nFloats && k < kSceneMin / 4; k += 4)
    Log("  B_Scene +%-4d: %14.6f %14.6f %14.6f %14.6f", k * 4, s[k], s[k + 1], s[k + 2], s[k + 3]);
}

}  // namespace

void CamConfigure(const CamOptions& o) {
  g_enabled = o.startImmersive;
  g_autoScreen = o.autoScreenInMenus;
  if (g_autoScreen) {
    // Zwift always starts in its menus: flat screen until its log says a ride started (if the mod is
    // injected mid-ride, the log replay switches to immersive within a second).
    g_enabled = false;
    ZwiftStateStart();
    Log("camera: auto_screen_in_menus on - virtual screen in Zwift's menus, immersive while riding "
        "(Ctrl+Shift+F10 overrides until the next ride start/end)");
  }
  g_cmPerMetre = 100.0f * o.worldScale;
  g_parallax = o.parallax;
  g_recentre = o.recentrePosition;
  g_coherentMode = std::clamp(o.coherentMode, 0, 2);
  g_steadyTau = std::clamp(o.steadyCamera, 0.0f, 0.5f);
  Log("camera: immersive mode %s at start (Ctrl+Shift+F10 toggles), %.1f world units per metre",
      g_enabled ? "ON" : "off", g_cmPerMetre);
  Log("camera: steady_camera %.2f s (%s)", g_steadyTau,
      g_steadyTau > 0.0f ? "Zwift's camera shake on bridges, cobbles and gravel filtered out" : "off: Zwift's camera as is");
  Log("camera: parallax %.2f (Ctrl+Shift+PageUp/PageDown +-0.25), recentre_position %s (Ctrl+Shift+F1 re-homes), "
      "coherent %s, verdict keys Ctrl+Shift+Home = MERGED / End = DOUBLED",
      g_parallax, g_recentre ? "on" : "off", CoherentName(g_coherentMode));
}

void CamHome(float out[3]) {
  // Plain float reads from another thread: a torn read for one frame only shifts a HUD card by millimetres.
  const bool home = g_recentre && g_haveHome;
  for (int i = 0; i < 3; ++i) out[i] = home ? g_home[i] : 0.0f;
}

bool CamCurrentFrameInfo(FrameInfo* out) {
  if (g_cur.eye < 0 || !out) return false;
  *out = FrameInfo{};
  out->frameNo = g_frameNo;
  out->haveCam = g_haveFrameCam;
  if (g_haveFrameCam) {
    memcpy(out->zwiftCam, g_frameCam, sizeof(g_frameCam));
    memcpy(out->zwiftFwd, g_frameFwd, sizeof(g_frameFwd));
  }
  out->mode = g_cur.eye == 2 ? 2 : 0;
  out->parallax = g_cur.parallax;
  return true;
}

namespace {

const char* ModeName() {
  if (g_cur.eye == 2) return g_coherentMode == 2 ? "COHERENT (always)" : g_coherentActive ? "COHERENT (auto, moving)" : "COHERENT";
  return g_cur.stereo ? "STEREO" : "MONO (no scale field)";
}

// One line with everything that decides what the headset shows right now - logged on every test hotkey,
// so a verdict (Ctrl+Shift+Home/End) can be matched to its exact conditions afterwards.
void LogCondition(const char* why) {
  float s30, t30, s8, t8;
  g_motion.Avg(30, &s30, &t30);
  g_motion.Avg(8, &s8, &t8);
  const bool still = g_motion.n >= 8 && s30 < 1.0f && t30 < 0.05f;
  const double kDeg = 180.0 / 3.14159265358979;
  const float rx = (g_lastMid[0] - (g_recentre ? g_home[0] : 0.0f)) * 100.0f;
  const float ry = (g_lastMid[1] - (g_recentre ? g_home[1] : 0.0f)) * 100.0f;
  const float rz = (g_lastMid[2] - (g_recentre ? g_home[2] : 0.0f)) * 100.0f;
  Log("cond [%s]: mode %s | parallax %.2f | recentre %s, home (%.1f %.1f %.1f) cm, head vs LOCAL (%.1f %.1f %.1f) cm, "
      "vs home (%.1f %.1f %.1f) cm | crop %s | coherent %d | Zwift camera last 30 frames %.2f cm + %.3f "
      "deg/frame (last 8: %.2f cm + %.3f deg) -> %s | rendering %.1f x %.1f deg",
      why, ModeName(), g_cur.parallax, g_recentre ? "on" : "off", g_home[0] * 100.0f, g_home[1] * 100.0f,
      g_home[2] * 100.0f, g_lastMid[0] * 100.0f, g_lastMid[1] * 100.0f, g_lastMid[2] * 100.0f, rx, ry, rz,
      g_xrViews.crop.load() ? "ON" : "off", g_coherentMode, s30, t30, s8, t8,
      g_motion.n < 8 ? "UNKNOWN (no camera samples)" : still ? "STATIONARY" : "MOVING",
      (g_cur.renderFov.angleRight - g_cur.renderFov.angleLeft) * kDeg,
      (g_cur.renderFov.angleUp - g_cur.renderFov.angleDown) * kDeg);
}

// Rotation angle (deg) between two 3x3 rotations stored as the upper-left of column-major 4x4s:
// ||A-B||_F = 2*sqrt(2)*sin(theta/2), well conditioned for small angles (acos(trace) is not).
float TurnDeg(const float* a, const float* b) {
  double f = 0;
  for (int i = 0; i < 9; ++i) { const double d = static_cast<double>(a[i]) - b[i]; f += d * d; }
  const double x = std::min(1.0, std::sqrt(f) / (2.0 * std::sqrt(2.0)));
  return static_cast<float>(2.0 * std::asin(x) * 180.0 / 3.14159265358979);
}

bool KeyEdge(int vk, bool mods, bool& was) {
  const bool down = mods && (GetAsyncKeyState(vk) & 0x8000);
  const bool edge = down && !was;
  was = down;
  return edge;
}

// ---------------- [vr] steady_camera: Zwift's camera shake taken out ----------------
// On bridges, cobbles and gravel Zwift shakes its chase camera. On a monitor that is road feel; in a headset the
// whole world shakes around you, which is nauseating. The shake is fast jitter on top of the camera's smooth path,
// so each frame Zwift's camera (position + orientation) goes through a tracking filter - alpha-beta: it follows
// steady speed and steady turning with no lag, and passes ~1 Hz at 0.1 s - and the frame renders from the filtered
// camera instead. That is one more transform in camera space, H' = H * S with S = Vs * V^-1 (Zwift's camera ->
// the steadied one), so every patched path (B_Scene, g_W2V, g_WVP via C, g_L2W) picks it up at once. Jumps past
// the prediction (> 1.5 m or 20 deg: camera cuts, view changes, teleports) and gaps (> 0.25 s) snap back to Zwift's.

void QMul(const double* a, const double* b, double* o) {  // o = a * b, quaternions as (w x y z)
  const double w = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
  const double x = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
  const double y = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
  const double z = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
  o[0] = w, o[1] = x, o[2] = y, o[3] = z;
}
void QConj(const double* q, double* o) { o[0] = q[0], o[1] = -q[1], o[2] = -q[2], o[3] = -q[3]; }
void QNorm(double* q) {
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  for (int i = 0; i < 4; ++i) q[i] /= n;
}
void QExp(const double* r, double* q) {  // rotation vector (rad) -> unit quaternion
  const double a = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  const double s = a < 1e-12 ? 0.5 : std::sin(a * 0.5) / a;
  q[0] = std::cos(a * 0.5), q[1] = r[0] * s, q[2] = r[1] * s, q[3] = r[2] * s;
}
void QLog(const double* q, double* r) {  // unit quaternion -> rotation vector, the short way round
  const double sg = q[0] < 0 ? -1.0 : 1.0;
  const double x = q[1] * sg, y = q[2] * sg, z = q[3] * sg, w = q[0] * sg;
  const double s = std::sqrt(x * x + y * y + z * z);
  const double k = s < 1e-12 ? 2.0 : 2.0 * std::atan2(s, w) / s;
  r[0] = x * k, r[1] = y * k, r[2] = z * k;
}
// 3x3 rotation M(r,c) = m[r*3+c] <-> unit quaternion
void QFromMat(const double* m, double* q) {
  const double tr = m[0] + m[4] + m[8];
  if (tr > 0) {
    const double s = std::sqrt(tr + 1.0) * 2.0;
    q[0] = 0.25 * s, q[1] = (m[7] - m[5]) / s, q[2] = (m[2] - m[6]) / s, q[3] = (m[3] - m[1]) / s;
  } else if (m[0] > m[4] && m[0] > m[8]) {
    const double s = std::sqrt(1.0 + m[0] - m[4] - m[8]) * 2.0;
    q[0] = (m[7] - m[5]) / s, q[1] = 0.25 * s, q[2] = (m[1] + m[3]) / s, q[3] = (m[2] + m[6]) / s;
  } else if (m[4] > m[8]) {
    const double s = std::sqrt(1.0 + m[4] - m[0] - m[8]) * 2.0;
    q[0] = (m[2] - m[6]) / s, q[1] = (m[1] + m[3]) / s, q[2] = 0.25 * s, q[3] = (m[5] + m[7]) / s;
  } else {
    const double s = std::sqrt(1.0 + m[8] - m[0] - m[4]) * 2.0;
    q[0] = (m[3] - m[1]) / s, q[1] = (m[2] + m[6]) / s, q[2] = (m[5] + m[7]) / s, q[3] = 0.25 * s;
  }
  QNorm(q);
}
void QToMat(const double* q, double* m) {
  const double w = q[0], x = q[1], y = q[2], z = q[3];
  m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - w * z), m[2] = 2 * (x * z + w * y);
  m[3] = 2 * (x * y + w * z), m[4] = 1 - 2 * (x * x + z * z), m[5] = 2 * (y * z - w * x);
  m[6] = 2 * (x * z - w * y), m[7] = 2 * (y * z + w * x), m[8] = 1 - 2 * (x * x + y * y);
}
double Len3(const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// One filter step on this frame's raw camera: position c (world cm) and camera->world rotation Q.
void SteadyStep(const double* c, const double* Q) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  const double dt = g_st.have ? static_cast<double>(now.QuadPart - g_st.t.QuadPart) / freq.QuadPart : 0.0;
  g_st.t = now;
  constexpr double kJumpCm = 150.0, kJumpRad = 0.35, kGapS = 0.25;
  bool snap = !g_st.have || dt <= 0.0 || dt > kGapS;
  double pc[3], pq[4], e[3], r[3];
  if (!snap) {  // predict with the tracked velocities, then compare with Zwift's camera
    double dq[4], wdt[3] = {g_st.w[0] * dt, g_st.w[1] * dt, g_st.w[2] * dt}, qc[4], rq[4];
    for (int i = 0; i < 3; ++i) pc[i] = g_st.c[i] + g_st.v[i] * dt, e[i] = c[i] - pc[i];
    QExp(wdt, dq);
    QMul(dq, g_st.q, pq);
    QNorm(pq);
    QConj(pq, qc);
    QMul(Q, qc, rq);
    QLog(rq, r);
    snap = Len3(e) > kJumpCm || Len3(r) > kJumpRad;
  }
  if (snap) {
    // Back on Zwift's camera. Start the velocities from its last step, unless that step was the jump itself.
    double d[3] = {c[0] - g_st.rawC[0], c[1] - g_st.rawC[1], c[2] - g_st.rawC[2]}, qc[4], rq[4], rr[3];
    QConj(g_st.rawQ, qc);
    QMul(Q, qc, rq);
    QLog(rq, rr);
    const bool steps = g_st.have && dt > 0.0 && dt <= kGapS && Len3(d) < kJumpCm && Len3(rr) < kJumpRad;
    for (int i = 0; i < 3; ++i) {
      g_st.v[i] = steps ? d[i] / dt : 0.0;
      g_st.w[i] = steps ? rr[i] / dt : 0.0;
      g_st.c[i] = c[i];
    }
    memcpy(g_st.q, Q, sizeof(g_st.q));
    if (g_st.have) g_stSnaps++;
  } else {
    const double a = 1.0 - std::exp(-dt / g_steadyTau), b = a * a / (2.0 - a);  // critically damped alpha-beta
    double ar[3] = {r[0] * a, r[1] * a, r[2] * a}, dq[4];
    for (int i = 0; i < 3; ++i) {
      g_st.c[i] = pc[i] + a * e[i];
      g_st.v[i] += b / dt * e[i];
      g_st.w[i] += b / dt * r[i];
    }
    QExp(ar, dq);
    QMul(dq, pq, g_st.q);
    QNorm(g_st.q);
  }
  memcpy(g_st.rawC, c, sizeof(g_st.rawC));
  memcpy(g_st.rawQ, Q, sizeof(g_st.rawQ));
  g_st.have = true;
}

// First sighting of Zwift's main view V in a frame (B_Scene main slice or main g_W2V upload): filter, then render
// this frame from the steadied camera. Later sightings in the same frame reuse it.
void Steady(const float* V) {
  if (g_steadyTau <= 0.0f || g_cur.eye < 0) return;
  if (g_steadyFrame) {
    if (!CloseRigid(V, g_steadyV, 1e-4f, 0.5f)) g_stMismatch++;
    return;
  }
  g_steadyFrame = true;
  memcpy(g_steadyV, V, sizeof(g_steadyV));

  // Zwift's camera: camera->world rotation Rc (= R_V^T, Rc(r,c) = V[r*4+c]) and position c = -R_V^T t.
  double Rc[9], c[3], Q[4];
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k) Rc[r * 3 + k] = V[r * 4 + k];
  for (int i = 0; i < 3; ++i)
    c[i] = -(static_cast<double>(V[i * 4 + 0]) * V[12] + static_cast<double>(V[i * 4 + 1]) * V[13] +
             static_cast<double>(V[i * 4 + 2]) * V[14]);
  QFromMat(Rc, Q);
  SteadyStep(c, Q);

  // S: Zwift's camera space -> the steadied camera's. Rotation Rs^T * Rc, translation Rs^T * (c - c_s).
  double Rs[9], SR[9], St[3];
  QToMat(g_st.q, Rs);
  for (int r = 0; r < 3; ++r) {
    St[r] = 0.0;
    for (int k = 0; k < 3; ++k) {
      SR[r * 3 + k] = Rs[0 * 3 + r] * Rc[0 * 3 + k] + Rs[1 * 3 + r] * Rc[1 * 3 + k] + Rs[2 * 3 + r] * Rc[2 * 3 + k];
      St[r] += Rs[k * 3 + r] * (c[k] - g_st.c[k]);
    }
  }
  float S[16] = {}, Sinv[16] = {};
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) {
      S[k * 4 + r] = static_cast<float>(SR[r * 3 + k]);
      Sinv[k * 4 + r] = static_cast<float>(SR[k * 3 + r]);
    }
    S[12 + r] = static_cast<float>(St[r]);
    Sinv[12 + r] = static_cast<float>(-(SR[0 * 3 + r] * St[0] + SR[1 * 3 + r] * St[1] + SR[2 * 3 + r] * St[2]));
  }
  S[15] = Sinv[15] = 1.0f;
  Mul(g_H0, S, g_cur.H);
  Mul(Sinv, g_H0inv, g_cur.Hinv);
  UpdateC();

  // How much shake came out this frame: position offset and rotation angle between Zwift's camera and ours.
  const double dp = std::sqrt(St[0] * St[0] + St[1] * St[1] + St[2] * St[2]);
  const double ca = std::clamp((SR[0] + SR[4] + SR[8] - 1.0) * 0.5, -1.0, 1.0);
  const double da = std::acos(ca) * 57.29578;
  g_stFrames++;
  g_stPosSum += dp;
  g_stRotSum += da;
  g_stPosMax = std::max(g_stPosMax, static_cast<float>(dp));
  g_stRotMax = std::max(g_stRotMax, static_cast<float>(da));
}

}  // namespace

void CamSetWindowAspect(float aspect) { g_windowAspect = aspect; }

bool g_camSkipReflDraws = false;
float g_camReflClear[4] = {};
bool g_camHaveReflClear = false;
void CamSetReflections(bool on) { g_reflSkip = !on; }

void CamDisable() {  // plain stores only: called from the swap hook's SEH handler
  g_enabled = false;
  g_cur.eye = -1;
  g_camSkipReflDraws = false;
  g_hudPhase = false;
}

void CamEnterHudPhase() {
  if (g_cur.eye >= 0) g_hudPhase = true;
}

int CamCurrentEye(XrPosef* pose, XrFovf* fov) {
  if (g_cur.eye >= 0) {
    if (pose) *pose = g_cur.pose;
    if (fov) *fov = g_cur.renderFov;  // what was RENDERED - the XR thread submits exactly this
  }
  return g_cur.eye;
}

void CamOnSwap() {
  CheckProbes();  // first: the frame that just finished

  if (g_autoScreen) {
    const uint32_t seq = ZwiftStateSeq();
    if (seq != g_autoSeq) {
      g_autoSeq = seq;
      const int riding = ZwiftStateRiding();
      if (riding >= 0 && g_enabled != (riding == 1)) {
        g_enabled = riding == 1;
        if (g_enabled) g_rehome = true;  // same as the hotkey: re-capture the eye origin
        Log("auto: Zwift %s -> immersive mode %s", riding ? "riding" : "in menus", g_enabled ? "ON" : "OFF (screen)");
      }
    }
  }
  const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
  const bool down = mods && (GetAsyncKeyState(VK_F10) & 0x8000);
  if (down && !g_keyWasDown) {
    g_enabled = !g_enabled;
    if (g_enabled) g_rehome = true;  // re-capture the eye origin when immersive comes back
    Log("Ctrl+Shift+F10: immersive mode %s", g_enabled ? "ON" : "OFF (screen)");
  }
  g_keyWasDown = down;
  const bool f3 = mods && (GetAsyncKeyState(VK_F3) & 0x8000);
  if (f3 && !g_f3WasDown) {
    g_reflSkip = !g_reflSkip;
    Log("Ctrl+Shift+F3: water reflections %s", g_reflSkip ? "OFF (reflection pass skipped)" : "ON (Zwift's camera)");
  }
  g_f3WasDown = f3;
  const bool f4 = mods && (GetAsyncKeyState(VK_F4) & 0x8000);
  if (f4 && !g_f4WasDown) {
    g_raysLegacy = !g_raysLegacy;
    GlTraceSetLegacyRays(g_raysLegacy);
    Log("Ctrl+Shift+F4: deferred lighting rays %s", g_raysLegacy ? "LEGACY (Zwift's own - lighting slides)" : "FIXED (eye camera)");
  }
  g_f4WasDown = f4;
  bool logCond = false;
  const char* condWhy = "";
  if (KeyEdge(VK_F1, mods, g_f1Was)) {
    g_recentre = true;
    g_rehome = true;
    Log("Ctrl+Shift+F1: re-homing the eye origin at the current head position");
    logCond = true, condWhy = "F1 re-home";
  }
  const bool pgUp = KeyEdge(VK_PRIOR, mods, g_pgUpWas), pgDn = KeyEdge(VK_NEXT, mods, g_pgDnWas);
  if (pgUp || pgDn) {
    // 0.25 steps, snapped so 1.0 / 0.5 / 0 are always reachable exactly
    g_parallax = std::clamp(std::round((g_parallax + (pgUp ? 0.25f : -0.25f)) * 4.0f) / 4.0f, 0.0f, 1.5f);
    Log("Ctrl+Shift+%s: parallax %.2f", pgUp ? "PageUp" : "PageDown", g_parallax);
    logCond = true, condWhy = "parallax";
  }
  if (KeyEdge(VK_HOME, mods, g_homeWas)) { Log("VERDICT MERGED"); logCond = true, condWhy = "VERDICT MERGED"; }
  if (KeyEdge(VK_END, mods, g_endWas)) { Log("VERDICT DOUBLED"); logCond = true, condWhy = "VERDICT DOUBLED"; }
  if (g_xrViews.recentreAtTick.load() && GetTickCount64() >= g_xrViews.recentreAtTick.load()) {
    g_xrViews.recentreAtTick.store(0);
    if (g_recentre) g_rehome = true;
  }

  const int prevEye = g_cur.eye;
  if (prevEye >= 0 && g_haveFrameCam) {  // camera step / turn between this frame and the previous one
    if (g_haveLastCam) {
      const float dx = g_frameCam[0] - g_lastCam[0], dy = g_frameCam[1] - g_lastCam[1], dz = g_frameCam[2] - g_lastCam[2];
      const float s = std::sqrt(dx * dx + dy * dy + dz * dz);
      const float t = TurnDeg(g_frameRot, g_lastRot);
      if (s > 1000.0f) {  // > 10 m in one frame: a camera cut, not motion
        g_camCuts++;
      } else {
        g_camStepSum += s;
        g_camTurnSum += t;
        g_camSteps++;
        g_camStepMax = std::max(g_camStepMax, s);
        g_camTurnMax = std::max(g_camTurnMax, t);
        g_motion.Add(s, t);
      }
    }
    memcpy(g_lastCam, g_frameCam, sizeof(g_lastCam));
    memcpy(g_lastRot, g_frameRot, sizeof(g_lastRot));
    g_haveLastCam = true;
  } else {
    g_haveLastCam = false;
  }
  g_haveFrameCam = false;

  // COHERENT switching, with hysteresis: in after 0.5 s of motion, out after 1 s of stillness.
  {
    const ULONGLONG t = GetTickCount64();
    float s8, t8;
    g_motion.Avg(8, &s8, &t8);
    const bool moving = g_motion.n >= 4 && (s8 > 2.0f || t8 > 0.1f);
    const bool still = g_motion.n < 4 || (s8 < 0.5f && t8 < 0.03f);
    if (moving) { if (!g_motionSince) g_motionSince = t; } else g_motionSince = 0;
    if (still) { if (!g_stillSince) g_stillSince = t; } else g_stillSince = 0;
    const bool was = g_coherentActive;
    if (g_coherentMode == 0) g_coherentActive = false;
    else if (g_coherentMode == 2) g_coherentActive = true;
    else if (!g_coherentActive && g_motionSince && t - g_motionSince >= 500) g_coherentActive = true;
    else if (g_coherentActive && g_stillSince && t - g_stillSince >= 1000) g_coherentActive = false;
    if (was != g_coherentActive && g_coherentMode == 1)  // only mode 1 switches on its own
      Log("camera: %s (Zwift camera %.1f cm + %.2f deg per frame)",
          g_coherentActive ? "MOVING -> COHERENT (both eyes from each frame, no parallax)" : "STILL -> STEREO", s8, t8);
  }
  g_cur.eye = -1;
  g_hudPhase = false;      // the next frame starts with its world pass
  g_steadyFrame = false;   // ... and its camera is filtered when first seen
  g_sliceKind.clear();     // new frame: B_Scene slices are classified (and patched) once again
  g_camSkipReflDraws = false;
  g_patchedScene.clear();
  g_patchedObj.clear();
  g_pvPrev = g_pvThis;
  g_pvThis.n = 0;
  for (auto& [buf, m] : g_mapped) m.framesSinceQuery++;
  if (g_enabled) {
    XrPosef pose[2];
    XrFovf fov[2];
    bool valid;
    {
      std::lock_guard lock(g_xrViews.m);
      valid = g_xrViews.valid;
      memcpy(pose, g_xrViews.pose, sizeof(pose));
      memcpy(fov, g_xrViews.fov, sizeof(fov));
    }
    if (valid) {
      const XrVector3f& p0 = pose[0].position;
      const XrVector3f& p1 = pose[1].position;
      const float mid[3] = {(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f, (p0.z + p1.z) * 0.5f};
      memcpy(g_lastMid, mid, sizeof(mid));
      if (g_recentre && (!g_haveHome || g_rehome)) {
        memcpy(g_home, mid, sizeof(mid));
        g_haveHome = true;
        Log("camera: eye origin homed at LOCAL (%.1f %.1f %.1f) cm - Zwift's camera is now where your head is",
            mid[0] * 100.0f, mid[1] * 100.0f, mid[2] * 100.0f);
        if (!logCond) logCond = true, condWhy = "home";
      }
      g_rehome = false;
      const float home[3] = {g_recentre ? g_home[0] : 0.0f, g_recentre ? g_home[1] : 0.0f, g_recentre ? g_home[2] : 0.0f};

      // Render pose rp (where Zwift's camera is put) vs submitted pose (the runtime's own eye pose: no depth
      // is submitted, so the compositor only uses its orientation). The eye position is decomposed as
      //   (mid-eye - home) + parallax * (eye - mid-eye)
      // so recentring removes head drift and parallax scales only the eye separation.
      const bool coherent = g_coherentActive;
      XrPosef rp{}, sub{};
      XrFovf ef{};
      int eye;
      if (coherent) {
        eye = 2;
        sub.orientation = pose[0].orientation;
        sub.position = {mid[0], mid[1], mid[2]};
        rp.orientation = pose[0].orientation;
        rp.position = {mid[0] - home[0], mid[1] - home[1], mid[2] - home[2]};
        ef.angleLeft = std::min(fov[0].angleLeft, fov[1].angleLeft);  // union: one image covers both eyes
        ef.angleRight = std::max(fov[0].angleRight, fov[1].angleRight);
        ef.angleUp = std::max(fov[0].angleUp, fov[1].angleUp);
        ef.angleDown = std::min(fov[0].angleDown, fov[1].angleDown);
      } else {
        eye = g_nextEye;
        g_nextEye ^= 1;
        const XrVector3f& pe = pose[eye].position;
        sub = pose[eye];
        rp.orientation = pose[eye].orientation;
        rp.position = {mid[0] - home[0] + g_parallax * (pe.x - mid[0]), mid[1] - home[1] + g_parallax * (pe.y - mid[1]),
                       mid[2] - home[2] + g_parallax * (pe.z - mid[2])};
        ef = fov[eye];
      }
      float E[16];
      PoseToMatrix(rp, g_cmPerMetre, E);
      memcpy(g_cur.Hinv, E, sizeof(E));
      Inverse(E, g_cur.H);
      memcpy(g_H0, g_cur.H, sizeof(g_H0));  // head only; Steady() adds this frame's shake removal
      memcpy(g_H0inv, g_cur.Hinv, sizeof(g_H0inv));
      g_cur.pose = sub;
      g_cur.fov = ef;
      g_cur.eye = eye;
      g_cur.parallax = coherent ? 0.0f : g_parallax;
      g_cur.stereo = g_lay.scale >= 0;
      ComputeEyeProjection();
      g_frameNo++;
      if (coherent) g_framesCoherent++;
      const XrQuaternionf& q = sub.orientation;  // head yaw about +Y (OpenXR), degrees
      const float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y)) * 57.29578f;
      g_yawMin = std::min(g_yawMin, yaw);
      g_yawMax = std::max(g_yawMax, yaw);
    }
  }
  if ((prevEye >= 0) != (g_cur.eye >= 0))
    Log("camera: override %s", g_cur.eye >= 0 ? "ACTIVE (alternate-eye rendering)" : "inactive");
  if (logCond) LogCondition(condWhy);

  if (g_cur.eye >= 0) g_frames++;
  ULONGLONG now = GetTickCount64();
  if (!g_statsSince) g_statsSince = now;
  if (now - g_statsSince >= 10000) {
    // Drain the draw counters every window (discarded when nothing was overridden), so a window with
    // immersive off doesn't pile its draws into the next one's per-frame figures.
    unsigned shadowFbos = 0, reflSkipped = 0, resolveDraws = 0;
    bool raysRewritten = false;
    const unsigned skipped = GlTraceTakeShadowDrawsSkipped(&shadowFbos);
    GlTraceTakeRenderStats(&reflSkipped, &resolveDraws, &raysRewritten);
    if (g_frames) {
      const double kDeg = 180.0 / 3.14159265358979;
      Log("camera: %u overridden frames in %.1fs | mode %s, rendering %.1f x %.1f deg | patched W2V %u, PROJ %u, "
          "WVP %u, L2W %u, refl %u | other-camera projections %u | Zwift FOV changed %u times, hfov %.1f-%.1f deg%s",
          g_frames, (now - g_statsSince) / 1000.0, g_cur.stereo ? "STEREO" : "MONO (no scale field)",
          (g_cur.renderFov.angleRight - g_cur.renderFov.angleLeft) * kDeg,
          (g_cur.renderFov.angleUp - g_cur.renderFov.angleDown) * kDeg, g_nW2V, g_nProj, g_nWVP, g_nL2W, g_nRefl,
          g_nOtherProj, g_nP0Changes, g_m0Max > 0 ? 2 * std::atan(1.0 / g_m0Max) * kDeg : 0.0,
          g_m0Max > 0 ? 2 * std::atan(1.0 / g_m0Min) * kDeg : 0.0, g_haveP0 ? "" : " (main projection not seen yet)");
      Log("camera: B_Scene patched %u (main %u, reflection %u, rejected %u, stale rebinds %u) | viewPos %u (skipped %u), "
          "dir %u, scale %u (skipped %u), V^-1 %u | B_Object patched %u (%u via P*V*M), other-camera %u, no ptr %u",
          g_nScene, g_nSceneMain, g_nSceneRefl, g_nSceneReject, g_nSceneStale, g_nPos, g_nPosSkip, g_nDir, g_nScale,
          g_nScaleSkip, g_nVinv, g_nObj, g_nObjWvp, g_nObjOther, g_nObjNoPtr);
      const unsigned(*pc)[3] = g_probeCounts;
      Log("camera: swap-time readback intact/reverted/other: V %u/%u/%u, viewPos %u/%u/%u, scale %u/%u/%u, "
          "B_Object %u/%u/%u",
          pc[kProbeV][0], pc[kProbeV][1], pc[kProbeV][2], pc[kProbePos][0], pc[kProbePos][1], pc[kProbePos][2],
          pc[kProbeScale][0], pc[kProbeScale][1], pc[kProbeScale][2], pc[kProbeObj][0], pc[kProbeObj][1],
          pc[kProbeObj][2]);
      const double fr = g_frames ? static_cast<double>(g_frames) : 1.0;
      Log("render: shadow draws skipped %.0f/frame into %u shadow-map FBOs | reflection-pass draws skipped %.0f/frame "
          "(reflection slices %u) | deferred resolve %.1f draws/frame, rays %s",
          skipped / fr, shadowFbos, reflSkipped / fr, g_nSceneRefl, resolveDraws / fr,
          !raysRewritten ? "NOT rewritten (Zwift's own)" : g_raysLegacy ? "rewritten but LEGACY (F4)" : "FIXED");
      Log("camera: uniform other-camera uploads left alone: W2V %u, WVP %u (shadow/spotlight passes) | sun-cascade "
          "origin range %.0f/%.0f/%.0f cm, head yaw range %.1f deg",
          g_nW2VOther, g_nWVPOther, g_haveSvp ? g_svpMax[0] - g_svpMin[0] : -1.0f,
          g_haveSvp ? g_svpMax[1] - g_svpMin[1] : -1.0f, g_haveSvp ? g_svpMax[2] - g_svpMin[2] : -1.0f,
          g_yawMax >= g_yawMin ? g_yawMax - g_yawMin : 0.0f);
      Log("camera: HUD phase left on Zwift's camera: perspective PROJ %u, W2V/WVP %u, B_Scene slices %u "
          "(the HUD's own 3D renders, e.g. jersey icons)",
          g_nHudProj, g_nHudOther, g_nHudSlices);
      if (g_steadyTau > 0.0f && g_stFrames)
        Log("camera: steady_camera %.2f s: Zwift's camera shake taken out avg %.1f cm + %.2f deg per frame (max %.1f cm, "
            "%.2f deg) over %u frames | %u snaps back to Zwift's camera (cuts, view changes) | %u uploads before the "
            "frame's camera, %u V mismatches",
            g_steadyTau, g_stPosSum / g_stFrames, g_stRotSum / g_stFrames, g_stPosMax, g_stRotMax, g_stFrames,
            g_stSnaps, g_stEarly, g_stMismatch);
      if (g_camSteps) {
        const bool still = g_camStepSum / g_camSteps < 1.0 && g_camTurnSum / g_camSteps < 0.05;
        Log("camera: Zwift camera moved %.1f cm + turned %.3f deg per frame avg (max %.1f cm, %.3f deg), %u cuts, "
            "between consecutive frames (%s; eye offset is ~6 cm) | COHERENT %u of %u frames | parallax %.2f",
            g_camStepSum / g_camSteps, g_camTurnSum / g_camSteps, g_camStepMax, g_camTurnMax, g_camCuts,
            still ? "STATIONARY - stereo judgement valid" : "MOVING - alternate-eye pairs skew", g_framesCoherent,
            g_frames, g_parallax);
      } else {
        Log("camera: Zwift camera motion: no samples yet (no main B_Scene slice seen)");
      }
    }
    g_camStepSum = g_camTurnSum = 0;
    g_camSteps = g_camCuts = 0;
    g_camStepMax = g_camTurnMax = 0;
    g_framesCoherent = 0;
    g_nW2VOther = g_nWVPOther = 0;
    g_nHudProj = g_nHudOther = g_nHudSlices = 0;
    g_stFrames = g_stSnaps = g_stEarly = g_stMismatch = 0;
    g_stPosSum = g_stRotSum = 0;
    g_stPosMax = g_stRotMax = 0;
    g_haveSvp = false;
    g_yawMin = 1e9f;
    g_yawMax = -1e9f;
    g_nScene = g_nObj = g_nObjOther = g_nObjNoPtr = g_nSceneMain = g_nSceneRefl = g_nObjWvp = 0;
    g_nSceneReject = g_nSceneStale = g_nPos = g_nPosSkip = g_nDir = g_nScale = g_nScaleSkip = g_nVinv = 0;
    g_nW2V = g_nProj = g_nWVP = g_nL2W = g_nRefl = g_frames = g_nOtherProj = g_nP0Changes = 0;
    memset(g_probeCounts, 0, sizeof(g_probeCounts));
    g_m0Min = 1e9f;
    g_m0Max = 0.0f;
    g_statsSince = now;
  }
}

void CamOnBindRange(GLenum target, GLuint index, GLuint buffer, ptrdiff_t offset, ptrdiff_t size) {
  if (g_cur.eye < 0 || !buffer) return;

  if (target == kSsbo && index == kSceneBinding) {
    // Each slice offset is classified once per frame (main / water reflection / other); a rebind just
    // re-applies the reflection-pass skip for that slice without re-reading the write-combined memory.
    auto known = g_sliceKind.find(offset);
    if (known != g_sliceKind.end()) {
      g_camSkipReflDraws = g_reflSkip && known->second == kSliceRefl;
      return;
    }
    g_camSkipReflDraws = false;  // any other B_Scene bind ends a reflection pass
    g_sliceKind[offset] = kSliceOther;
    if (g_hudPhase) {  // first bound after the world was composited: the HUD's own 3D render, Zwift's camera
      g_nHudSlices++;
      return;
    }
    if (size < kSceneMin) return;
    float* base = CpuPtr(buffer, offset, kSceneMin);
    if (!base) return;
    float V[16], P[16];
    if (!SafeRead(V, base + kSceneV / 4) || !SafeRead(P, base + kSceneP / 4)) return;
    for (const StaleEntry& st : g_stale)
      if (st.offset == offset && memcmp(st.V2, V, sizeof(V)) == 0) { g_nSceneStale++; return; }
    if (!IsRigid(V) || TranslationL1(V) < 1000 || !IsPerspective(P) || P[11] > 0) {
      g_nSceneReject++;  // shadow cascades (orthographic) etc.
      if (!g_loggedLayout) {
        Log("camera: B_Scene slice at %lld doesn't have V@%d / P@%d (other pass or layout changed) - skipped",
            static_cast<long long>(offset), static_cast<int>(kSceneV), static_cast<int>(kSceneP));
        g_loggedLayout = true;
      }
      return;
    }
    const float aspect = std::fabs(P[5]) / P[0];
    if (g_windowAspect <= 0 || std::fabs(aspect - g_windowAspect) > 0.25f * g_windowAspect) {
      g_nSceneReject++;  // not the main camera
      return;
    }

    // Water-reflection camera? Zwift builds it double-mirrored: det +1 but upside down (V[5] < 0), with a
    // world clip plane at +352 (diagnosis 2026-11-08: V_r = F_x*V*S_h, clipPlane = water plane). A true
    // mirror (det -1) counts too. It is NEVER patched: with reflections off its whole pass is skipped; with
    // them on it stays on Zwift's camera together with reflectedViewMatrix, so render and lookup agree.
    {
      float pl[4] = {};
      const bool havePl = SafeCopy(pl, base + kSceneClipPlane / 4, 16);
      const float pn = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
      const float dV = Det3(V);
      if (dV < 0 || (dV > 0 && V[5] < 0.0f && havePl && pn > 0.5f)) {
        g_sliceKind[offset] = kSliceRefl;
        g_camSkipReflDraws = g_reflSkip;
        // With the pass skipped, the reflection map still gets its colour clear: make that the reflection's
        // fog colour so the water blends toward haze instead of darkening toward black.
        g_camHaveReflClear = SafeCopy(g_camReflClear, base + kSceneFogColor / 4, 16);
        g_nSceneRefl++;
        return;
      }
    }
    g_sliceKind[offset] = kSliceMain;

    // This is the frame's authoritative camera: refresh P0 / V / original P*V, then write the eye's.
    if (!g_haveP0 || Diff(P, g_P0) > 1e-5) {
      memcpy(g_P0, P, sizeof(P));
      g_haveP0 = Inverse(g_P0, g_P0inv);
      ComputeEyeProjection();
    }
    float pv[16];
    Mul(P, V, pv);
    g_pvThis.Add(pv);
    g_havePV = true;
    const bool mainCam = true;  // reflection slices returned above
    if (mainCam) {
      memcpy(g_V, V, sizeof(V));
      g_haveV = Inverse(g_V, g_Vinv);
      if (g_haveV && !g_haveFrameCam) {  // Zwift's ORIGINAL camera (V read before our write)
        memcpy(g_frameCam, g_Vinv + 12, sizeof(g_frameCam));
        for (int c = 0; c < 3; ++c)
          for (int r = 0; r < 3; ++r) g_frameRot[c * 3 + r] = V[c * 4 + r];
        g_frameFwd[0] = -V[2], g_frameFwd[1] = -V[6], g_frameFwd[2] = -V[10];  // -(3rd row of R)
        g_haveFrameCam = true;
      }
      float svp[4];  // sun-cascade origin (we never write it): does it follow the head?
      if (SafeCopy(svp, base + kSceneShadowViewPos0 / 4, 16)) {
        for (int i = 0; i < 3; ++i) {
          if (!g_haveSvp || svp[i] < g_svpMin[i]) g_svpMin[i] = svp[i];
          if (!g_haveSvp || svp[i] > g_svpMax[i]) g_svpMax[i] = svp[i];
        }
        g_haveSvp = true;
      }
      g_nSceneMain++;
    } else {
      g_nSceneRefl++;
    }
    if (!g_lay.searched && mainCam) {
      const ptrdiff_t avail = std::min(size, kSceneSearch);
      if (const float* full = CpuPtr(buffer, offset, avail)) LocateSceneFields(full, avail, V, P);
    }
    if (!g_haveP0) return;  // P0 not invertible: nothing consistent to write
    Steady(V);  // [vr] steady_camera: from here on g_cur.H/Hinv/C include this frame's shake removal

    float Vinv[16], V2[16], P2inv[16];
    Inverse(V, Vinv);
    Mul(g_cur.H, V, V2);  // mat3 = R_E^T * R_V: the head rotation (the shader ignores the translation)
    Inverse(g_cur.Pp, P2inv);
    bool ok = SafeWrite(base + kSceneV / 4, V2) && SafeWrite(base + kSceneP / 4, g_cur.Pp) &&
              SafeWrite(base + kScenePinv / 4, P2inv);
    if (!ok) return;
    AddProbe(base + kSceneV / 4, V2, V, kProbeV);
    ptrdiff_t flushLen = kSceneMin;

    // viewPos: the eye's world position c + R_V^T * e  (= translation of (H*V)^-1 = V^-1 * E). Also right
    // for the mirrored reflection camera (orthogonal: inverse = transpose).
    if (g_lay.pos >= 0 && g_lay.pos + 16 <= size) {
      float* qa = CpuPtr(buffer, offset + g_lay.pos, 16);
      float q[4];
      if (qa && SafeCopy(q, qa, 16) && std::fabs(q[0] - Vinv[12]) < 10 && std::fabs(q[1] - Vinv[13]) < 10 &&
          std::fabs(q[2] - Vinv[14]) < 10) {
        const float* e = g_cur.Hinv + 12;
        float w[4];
        for (int i = 0; i < 3; ++i) w[i] = q[i] + V[i * 4 + 0] * e[0] + V[i * 4 + 1] * e[1] + V[i * 4 + 2] * e[2];
        w[3] = q[3];
        if (SafeCopy(qa, w, 16)) {
          AddProbe(qa, w, q, kProbePos);
          g_nPos++;
          flushLen = std::max(flushLen, static_cast<ptrdiff_t>(g_lay.pos + 16));
        }
      } else {
        g_nPosSkip++;
      }
    }
    // dir: rotate the camera-forward vector with the head: d' = R_V^T * R_E * R_V * d
    if (g_lay.dir >= 0 && g_lay.dir + 16 <= size) {
      float* da = CpuPtr(buffer, offset + g_lay.dir, 16);
      float d[4];
      if (da && SafeCopy(d, da, 16)) {
        const float* E = g_cur.Hinv;
        float a[3], b[3], o[4];
        for (int i = 0; i < 3; ++i) a[i] = V[0 * 4 + i] * d[0] + V[1 * 4 + i] * d[1] + V[2 * 4 + i] * d[2];
        for (int i = 0; i < 3; ++i) b[i] = E[0 * 4 + i] * a[0] + E[1 * 4 + i] * a[1] + E[2 * 4 + i] * a[2];
        for (int i = 0; i < 3; ++i) o[i] = V[i * 4 + 0] * b[0] + V[i * 4 + 1] * b[1] + V[i * 4 + 2] * b[2];
        o[3] = d[3];
        if (SafeCopy(da, o, 16)) {
          g_nDir++;
          flushLen = std::max(flushLen, static_cast<ptrdiff_t>(g_lay.dir + 16));
        }
      }
    }
    // scale: the diagonal projection the world actually uses (bias - depth terms - stays Zwift's)
    if (g_lay.scale >= 0 && g_lay.scale + 32 <= size) {
      float* sa = CpuPtr(buffer, offset + g_lay.scale, 16);
      float q[4];
      if (sa && SafeCopy(q, sa, 16) && Rel(q[0], P[0]) < 1e-3f && Rel(q[1], P[5]) < 1e-3f) {
        float w[4] = {q[0] * (g_cur.Pp[0] / P[0]), q[1] * (g_cur.Pp[5] / P[5]), q[2], q[3]};
        if (SafeCopy(sa, w, 16)) {
          AddProbe(sa, w, q, kProbeScale);
          g_nScale++;
          flushLen = std::max(flushLen, static_cast<ptrdiff_t>(g_lay.scale + 32));
        }
      } else {
        g_nScaleSkip++;
      }
    }
    // +928: only where it verifiably is V^-1 (3x3 = R_V^T): (H*V)^-1 = V^-1 * E
    if (g_lay.vinv >= 0) {
      float X[16], out[16];
      if (SafeRead(X, base + g_lay.vinv / 4)) {
        bool isVinv = true;
        for (int r = 0; r < 3 && isVinv; ++r)
          for (int c = 0; c < 3 && isVinv; ++c) isVinv = std::fabs(X[c * 4 + r] - V[r * 4 + c]) < 1e-3f;
        if (isVinv) {
          Mul(X, g_cur.Hinv, out);
          if (SafeWrite(base + g_lay.vinv / 4, out)) g_nVinv++;
        }
      }
    }

    FlushIfNeeded(buffer, offset, flushLen);
    StaleEntry& st = g_stale[g_staleNext];
    g_staleNext = (g_staleNext + 1) % 8;
    st.offset = offset;
    memcpy(st.V2, V2, sizeof(V2));
    g_nScene++;
    return;
  }

  // B_Object WVP is NOT patched any more: the captured shader sources (329, 201 of them declaring
  // B_Object { L2W, WVP, ShadowColor, ShadowScaleParams, ShadowBiasParams, windMagnitude }) never read
  // b_object.WVP - geometry is placed via L2W + B_Scene. Patching it cost ~1300 write-combined
  // read/modify/write+flush per frame on Zwift's render thread for no visible effect.
  constexpr bool kPatchObjectWvp = false;
  if (target == kUbo && index == kObjectBinding && kPatchObjectWvp) {
    if (size < kObjectMin || !g_havePV || !g_cur.haveC) return;
    if (!g_patchedObj.insert(offset).second) return;  // a slice rebound for another pass: already done
    float* base = CpuPtr(buffer, offset, kObjectMin);
    if (!base) { g_nObjNoPtr++; return; }
    // B_Object = { model M @+0, WVP = P*V*M @+64, ... }. (v2/v3 assumed +64 was P*V: true only for
    // identity-model objects like terrain, so ~80% of the world was left on Zwift's camera.)
    float M[16], VP[16];
    if (!SafeRead(M, base) || !SafeRead(VP, base + kObjectVP / 4)) return;
    if (IsAffine(VP)) { g_nObjOther++; return; }  // orthographic: shadow / UI
    bool main = g_pvThis.Match(VP) || g_pvPrev.Match(VP);
    if (!main) {  // P*V*M for one of this/last frame's camera slices?
      float t[16];
      for (const PVList* l : {&g_pvThis, &g_pvPrev})
        for (int i = 0; i < l->n && !main; ++i) {
          Mul(l->m[i], M, t);
          main = Diff(VP, t) < 5e-3;
        }
      if (main) g_nObjWvp++;
    }
    if (!main) { g_nObjOther++; return; }  // shadow / light passes carry their own camera
    float out[16];
    Mul(g_cur.C, VP, out);  // P'*H*P^-1 * (P*V*M) = P'*(H*V)*M, for any M
    if (SafeWrite(base + kObjectVP / 4, out)) {
      if (g_nObjProbes < 16) {
        AddProbe(base + kObjectVP / 4, out, VP, kProbeObj);
        g_nObjProbes++;
      }
      FlushIfNeeded(buffer, offset + kObjectVP, 64);
      g_nObj++;
    }
  }
}

bool CamOverrideUniform(GLint prog, GLint loc, const GLfloat* v, bool transpose, float* out) {
  if (g_cur.eye < 0 || !v || loc < 0) return false;
  ProgInfo& pi = Info(prog);
  if (loc != pi.w2v && loc != pi.proj && loc != pi.wvp && loc != pi.l2w && loc != pi.refl) return false;

  float m[16];
  if (transpose) Transpose(v, m); else memcpy(m, v, sizeof(m));

  if (g_hudPhase) {
    // After the world was composited only the HUD draws, and its 3D bits (the jersey icons in Zwifters Nearby) use
    // perspective cameras of their own, some with the window's aspect. Left untouched - and kept out of P0/V
    // tracking, which belongs to the world camera.
    if (loc == pi.proj) {
      pi.projPatched = false;
      if (IsPerspective(m) && m[11] <= 0) g_nHudProj++;
    } else if ((loc == pi.wvp && !IsAffine(m)) || (loc == pi.w2v && IsRigid(m))) {
      g_nHudOther++;
    }
    return false;
  }

  if (loc == pi.proj) {
    pi.projPatched = false;  // until this upload is actually replaced below
    if (!IsPerspective(m) || m[11] > 0) return false;  // not a camera projection (e.g. shadow/ortho)
    // Only the main camera's projection has the window's aspect; anything else (the square preview /
    // probe camera seen in the first immersive test) is another camera and is left alone, along with
    // the WVPs of its pass (g_lastProjMain).
    const float aspect = std::fabs(m[5]) / m[0];
    // 25%: Zwift's dynamic FOV moves the main aspect a little (2% dropped it entirely in the second
    // immersive test); the other camera is square (1.0 vs ~3.6), far outside this.
    g_lastProjMain = g_windowAspect > 0 && std::fabs(aspect - g_windowAspect) < 0.25f * g_windowAspect;
    if (!g_lastProjMain) {
      g_nOtherProj++;
      return false;
    }
    if (!g_haveP0 || Diff(m, g_P0) > 1e-5) {  // Zwift's FOV changes as you ride: track it every time
      const bool first = !g_haveP0;
      memcpy(g_P0, m, sizeof(m));
      g_haveP0 = Inverse(g_P0, g_P0inv);
      if (first) Log("camera: main projection m0 %.4f m5 %.4f m14 %.3f (aspect %.3f)", m[0], m[5], m[14], aspect);
      g_nP0Changes++;
      g_m0Min = std::min(g_m0Min, m[0]);
      g_m0Max = std::max(g_m0Max, m[0]);
      ComputeEyeProjection();  // pick up its exact near term
    }
    if (!g_haveP0) return false;
    memcpy(out, g_cur.Pp, sizeof(g_cur.Pp));
    pi.projPatched = true;
    g_nProj++;
    return true;
  }
  if (loc == pi.refl) {
    // Water VS: reflectedVP = g_PROJ * reflectedViewMatrix * g_L2W, sampled against the reflection texture
    // that was rendered UNPATCHED (P0 * V_r). This program's g_PROJ is now the eye's Pp, so upload
    // Pp^-1 * P0 * V_r: the product is P0 * V_r again (identity correction when Pp == P0, Zwift's own frustum).
    float ppInv[16], t[16];
    if (!g_haveP0 || !pi.projPatched || !Inverse(g_cur.Pp, ppInv)) return false;
    Mul(ppInv, g_P0, t);
    Mul(t, m, out);
    g_nRefl++;
    return true;
  }
  if (loc == pi.w2v) {
    if (!IsRigid(m) || TranslationL1(m) < 1000) return false;  // not a world camera
    if (Det3(m) < 0) return false;  // a true-mirror reflection camera: also left on Zwift's camera
    // Only the MAIN camera (and the water reflection, the only mirrored one) may follow the head. The
    // sun-cascade and headlight-spotlight shadow passes upload THEIR view here too; rotating those with
    // the head moved the shadow maps with the headset ("shadows move as I move the headset").
    if (loc == pi.w2v && Det3(m) > 0) {
      // Compare camera POSITION (c = -R^T t), not V's raw translation column: that is -R*c with |c| ~ 5e5 cm,
      // so a 0.01 rad turn alone moves it ~50 m. Rotation within ~8.6 deg (normal per-frame turning).
      if (g_haveV) {
        bool same = true;
        for (int i = 0; i < 9 && same; ++i) same = std::fabs(m[(i / 3) * 4 + i % 3] - g_V[(i / 3) * 4 + i % 3]) < 0.15f;
        for (int i = 0; i < 3 && same; ++i) {
          const float ci = -(m[i * 4 + 0] * m[12] + m[i * 4 + 1] * m[13] + m[i * 4 + 2] * m[14]);
          same = std::fabs(ci - g_Vinv[12 + i]) < 100.0f;
        }
        if (!same) { g_nW2VOther++; return false; }  // sun / headlight / other camera
      }
      memcpy(g_V, m, sizeof(m));
      g_haveV = Inverse(g_V, g_Vinv);
    }
    Steady(m);  // the frame's main camera (first sighting filters, later ones reuse it)
    Mul(g_cur.H, m, out);
    (loc == pi.refl ? g_nRefl : g_nW2V)++;
    return true;
  }
  if (loc == pi.wvp) {
    // orthographic UI/HUD and sun-cascade passes stay put
    if (IsAffine(m) || !g_cur.haveC || !g_haveP0) return false;
    // Which camera is this P*V*M from? For rigid V and uniformly scaled M the row norms give the
    // projection: |row0|/|row3| = m0, |row1|/|row3| = m5. Classify by the matrix itself, not by the last
    // g_PROJ upload (that flag went stale: the square headlight-spotlight pass returns before updating it,
    // so its WVPs were rewritten with the eye camera and its shadow map followed the head).
    const float r0 = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    const float r1 = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    const float r3 = std::sqrt(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
    if (r3 < 1e-6f || Rel(r0 / r3, g_P0[0]) > 0.05f || Rel(r1 / r3, std::fabs(g_P0[5])) > 0.05f) {
      g_nWVPOther++;
      return false;
    }
    if (g_steadyTau > 0.0f && !g_steadyFrame) g_stEarly++;  // patched before the frame's camera was seen
    Mul(g_cur.C, m, out);
    g_nWVP++;
    return true;
  }
  if (loc == pi.l2w) {
    if (!g_haveV || !CloseRigid(m, g_Vinv, 0.02f, 200.0f)) return false;  // a real model matrix
    if (g_steadyTau > 0.0f && !g_steadyFrame) g_stEarly++;
    Mul(m, g_cur.Hinv, out);
    g_nL2W++;
    return true;
  }
  return false;
}

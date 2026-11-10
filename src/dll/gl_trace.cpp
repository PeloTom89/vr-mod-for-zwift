#include "gl_trace.h"

#include <GL/gl.h>
#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include "camera.h"
#include "gl_capture.h"
#include "gl_util.h"
#include "log.h"

// ---- GL bits not in the 1.1 gl.h ----
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_RENDERBUFFER 0x8D41
#define GL_RENDERBUFFER_WIDTH 0x8D42
#define GL_RENDERBUFFER_HEIGHT 0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT 0x8D44
#define GL_RENDERBUFFER_SAMPLES 0x8CAB
#define GL_TEXTURE_SAMPLES 0x9106
#define GL_TEXTURE_TARGET 0x1006
#ifndef GL_TEXTURE_INTERNAL_FORMAT
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#endif
#define GL_DEPTH_STENCIL 0x84F9

using PFN_GetFbAttachParam = void(APIENTRY*)(GLenum, GLenum, GLenum, GLint*);
using PFN_GetTexLevelParam = void(APIENTRY*)(GLuint, GLint, GLenum, GLint*);
using PFN_GetTexParam = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_GetRbParam = void(APIENTRY*)(GLuint, GLenum, GLint*);

namespace {

using namespace glu;

// ---------------- trace state (render thread only) ----------------
bool g_tracing = false;
DWORD g_traceThread = 0;
bool g_hooksTried = false;
bool g_keyWasDown = false;
unsigned g_frameNo = 0;

PFN_GetFbAttachParam pGetFbAttach = nullptr;
PFN_GetTexLevelParam pGetTexLevel = nullptr;
PFN_GetTexParam pGetTexParam = nullptr;
PFN_GetRbParam pGetRbParam = nullptr;

struct Att { GLint type = GL_NONE, name = 0; };
struct Key {
  GLint fbo = -1;
  Att color[4], depth;
  bool operator==(const Key& o) const {
    if (fbo != o.fbo || depth.type != o.depth.type || depth.name != o.depth.name) return false;
    for (int i = 0; i < 4; ++i)
      if (color[i].type != o.color[i].type || color[i].name != o.color[i].name) return false;
    return true;
  }
};

struct Seg {
  Key key;
  GLint vp[4] = {};
  unsigned draws = 0;
  std::map<std::string, unsigned> byFn;
  std::vector<std::string> clears;
};

std::vector<std::string> g_lines;  // ordered output
GLint g_curProgram = 0;           // tracked by the glUseProgram hook
std::map<GLint, unsigned> g_progDraws;  // traced frame: draws per program
Seg g_seg;
bool g_segOpen = false;
unsigned g_totalDraws = 0;
std::map<long long, unsigned> g_drawsPerDepth;  // depth attachment -> draws into it
std::map<long long, std::string> g_descCache;
Key g_topKey;            // render target of the segment with the most draws (the main scene pass)
unsigned g_topDraws = 0;
bool g_armNext = false;  // hotkey seen; start tracing after our own capture has run
bool g_silentNext = false, g_silent = false;  // silent = scene probe only, no files
GLuint g_sceneLinearDepth = 0;               // R32F colour target of the busiest pass (Zwift's T4)

long long AttId(const Att& a) { return (static_cast<long long>(a.type) << 32) | static_cast<unsigned>(a.name); }

const char* FmtName(GLint f) {
  switch (f) {
    case 0x88F0: return "DEPTH24_STENCIL8";
    case 0x81A5: return "DEPTH_COMPONENT16";
    case 0x81A6: return "DEPTH_COMPONENT24";
    case 0x81A7: return "DEPTH_COMPONENT32";
    case 0x8CAC: return "DEPTH_COMPONENT32F";
    case 0x8CAD: return "DEPTH32F_STENCIL8";
    case 0x1902: return "DEPTH_COMPONENT";
    case 0x8058: return "RGBA8";
    case 0x8C43: return "SRGB8_ALPHA8";
    case 0x8051: return "RGB8";
    case 0x881A: return "RGBA16F";
    case 0x881B: return "RGB16F";
    case 0x8814: return "RGBA32F";
    case 0x8C3A: return "R11F_G11F_B10F";
    case 0x8059: return "RGB10_A2";
    case 0x805B: return "RGBA16";
    case 0x822D: return "R16F";
    case 0x822E: return "R32F";
    case 0x822F: return "RG16F";
    case 0x8230: return "RG32F";
    case 0x8229: return "R8";
    case 0x822B: return "RG8";
    case 0x1908: return "RGBA";
    default: return nullptr;
  }
}

std::string Describe(const Att& a) {
  if (a.type == GL_NONE) return "-";
  auto it = g_descCache.find(AttId(a));
  if (it != g_descCache.end()) return it->second;
  GLint w = 0, h = 0, fmt = 0, samples = 0, target = 0;
  char kind = '?';
  if (a.type == GL_TEXTURE && pGetTexLevel) {
    kind = 'T';
    pGetTexLevel(a.name, 0, GL_TEXTURE_WIDTH, &w);
    pGetTexLevel(a.name, 0, GL_TEXTURE_HEIGHT, &h);
    pGetTexLevel(a.name, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
    pGetTexLevel(a.name, 0, GL_TEXTURE_SAMPLES, &samples);
    if (pGetTexParam) pGetTexParam(a.name, GL_TEXTURE_TARGET, &target);
  } else if (a.type == GL_RENDERBUFFER && pGetRbParam) {
    kind = 'R';
    pGetRbParam(a.name, GL_RENDERBUFFER_WIDTH, &w);
    pGetRbParam(a.name, GL_RENDERBUFFER_HEIGHT, &h);
    pGetRbParam(a.name, GL_RENDERBUFFER_INTERNAL_FORMAT, &fmt);
    pGetRbParam(a.name, GL_RENDERBUFFER_SAMPLES, &samples);
  }
  char buf[160];
  const char* fn = FmtName(fmt);
  char fhex[16];
  if (!fn) { sprintf_s(fhex, "fmt0x%X", fmt); fn = fhex; }
  sprintf_s(buf, "%c%d %dx%d %s%s%s", kind, a.name, w, h, fn,
            samples > 1 ? (" x" + std::to_string(samples)).c_str() : "",
            target == 0x8C1A ? " (2D_ARRAY)" : target == 0x9100 ? " (2D_MS)" : target == 0x8513 ? " (CUBE)" : "");
  g_descCache[AttId(a)] = buf;
  return buf;
}

Att QueryAtt(GLenum attachment) {
  Att a;
  pGetFbAttach(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &a.type);
  if (a.type != GL_NONE) pGetFbAttach(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &a.name);
  return a;
}

Key CurrentKey() {
  Key k;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &k.fbo);
  if (k.fbo != 0 && pGetFbAttach) {
    for (int i = 0; i < 4; ++i) k.color[i] = QueryAtt(GL_COLOR_ATTACHMENT0 + i);
    k.depth = QueryAtt(GL_DEPTH_ATTACHMENT);
  }
  return k;
}

void FlushSeg() {
  if (!g_segOpen) return;
  const Seg& s = g_seg;
  std::string line;
  char buf[128];
  if (s.key.fbo == 0) {
    line = "FBO 0 (window)";
  } else {
    sprintf_s(buf, "FBO %d  color[", s.key.fbo);
    line = buf;
    bool first = true;
    for (const Att& c : s.key.color) {
      if (c.type == GL_NONE) continue;
      if (!first) line += ", ";
      line += Describe(c);
      first = false;
    }
    line += "]  depth[" + Describe(s.key.depth) + "]";
  }
  sprintf_s(buf, "  vp %d,%d %dx%d  draws %u", s.vp[0], s.vp[1], s.vp[2], s.vp[3], s.draws);
  line += buf;
  if (!s.byFn.empty()) {
    line += " (";
    bool first = true;
    for (auto& [fn, n] : s.byFn) { line += (first ? "" : " ") + fn + ":" + std::to_string(n); first = false; }
    line += ")";
  }
  for (auto& c : s.clears) line += "  " + c;
  g_lines.push_back(line);
  g_segOpen = false;
  if (s.draws > g_topDraws) { g_topDraws = s.draws; g_topKey = s.key; }
}

// Puts us in the right segment for the current draw framebuffer + attachments.
void EnterSegment() {
  Key k = CurrentKey();
  if (g_segOpen && k == g_seg.key) return;
  FlushSeg();
  g_seg = Seg{};
  g_seg.key = k;
  glGetIntegerv(GL_VIEWPORT, g_seg.vp);
  g_segOpen = true;
}

bool Active() { return g_tracing && GetCurrentThreadId() == g_traceThread; }

void OnDraw(const char* fn) {
  if (!Active()) return;
  g_progDraws[g_curProgram]++;
  EnterSegment();
  g_seg.draws++;
  g_seg.byFn[fn]++;
  g_totalDraws++;
  if (g_seg.key.depth.type != GL_NONE) g_drawsPerDepth[AttId(g_seg.key.depth)]++;
}

void OnClear(const char* what) {
  if (!Active()) return;
  EnterSegment();
  g_seg.clears.push_back(what);
}

void OnEvent(const std::string& line) {
  if (!Active()) return;
  FlushSeg();
  g_lines.push_back(line);
}

// ---------------- camera recon: matrices uploaded during the traced frame ----------------
// Zwift has to give its shaders a projection (and view) matrix somehow: plain uniforms or a uniform
// buffer. During a traced frame we scan every such upload for 4x4 float windows that look like
//   - a perspective projection: column-major [a 0 0 0 | 0 c 0 0 | x y e +-1 | 0 0 f 0] (or transposed)
//   - a rigid transform (view/model): orthonormal 3x3 + translation, last row 0 0 0 1
// and keep, per source (uniform location / buffer offset), how often it was written and whether the
// value changed within the frame. The camera's matrices are written few times and stay constant.
#define GL_CURRENT_PROGRAM 0x8B8D

struct MatSource {
  unsigned writes = 0, changes = 0;
  float first[16] = {}, last[16] = {};
  bool transposed = false;
};
std::map<std::string, MatSource> g_projSrc, g_rigidSrc;
unsigned g_uniformCalls = 0, g_bufferCalls = 0, g_mapCalls = 0;
unsigned long long g_bufferBytes = 0;

void Record(std::map<std::string, MatSource>& map, const std::string& key, const float* m, bool transposed) {
  MatSource& s = map[key];
  if (s.writes == 0) {
    memcpy(s.first, m, sizeof(s.first));
    s.transposed = transposed;
  } else if (memcmp(s.last, m, sizeof(s.last)) != 0) {
    s.changes++;
  }
  memcpy(s.last, m, sizeof(s.last));
  s.writes++;
}

// Scans `count` floats at 4-float (16-byte) steps for matrix-shaped windows.
void ScanFloats(const std::string& prefix, const float* f, size_t count, bool transposeHint) {
  if (!f) return;
  for (size_t i = 0; i + 16 <= count; i += 4) {
    float m[16];
    if (transposeHint) Transpose(f + i, m); else memcpy(m, f + i, sizeof(m));
    float t[16];
    Transpose(m, t);
    std::string key = prefix + "+" + std::to_string(i * 4);
    if (IsPerspective(m)) Record(g_projSrc, key, m, false);
    else if (IsPerspective(t)) Record(g_projSrc, key, t, true);
    else if (IsRigid(m)) Record(g_rigidSrc, key, m, false);
    else if (IsRigid(t)) Record(g_rigidSrc, key, t, true);
  }
}

std::string UniformPrefix(GLint program, GLint loc) {
  return "prog" + std::to_string(program) + " uniform@" + std::to_string(loc);
}

// Every single-mat4 uniform upload (whatever its shape), so each can be related to V and P at dump time.
struct AllMat {
  unsigned writes = 0;
  float last[16] = {};
};
std::map<std::pair<GLint, GLint>, AllMat> g_allMat;  // (program, location)

void RecordUniformMat(GLint prog, GLint loc, const GLfloat* v, bool transpose) {
  if (!v) return;
  AllMat& a = g_allMat[{prog, loc}];
  if (transpose) Transpose(v, a.last); else memcpy(a.last, v, sizeof(a.last));
  a.writes++;
}


// ---------------- optional: shadows off ([render] shadows = 0) ----------------
// Skip every draw into a shadow-map framebuffer (depth-only, square, depth format: Zwift's 3 sun
// cascades at 2048^2 and the headlight spotlight at 1024^2 in the frame trace). Their clears still run,
// so the maps stay empty and everything renders lit. Saves ~3100 driver draw calls per frame and removes
// any shadow that could follow the head. Classification is cached per FBO id and dropped when Zwift
// deletes the FBO, so a recycled id can never hide normal drawing.
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
bool g_skipShadows = false;
bool g_drawFboIsShadow = false;  // render thread only
GLuint g_curDrawFbo = 0;
bool g_hudBlend = false;  // [hud] "over" alpha override active (GlTraceHudBlend; render thread only)
void ApplyBlend();
// Cached classification per FBO id. A NEGATIVE result is never final: in-world the skip found nothing
// (session 17:15: 158 skips/frame in the menu scene, 0 in-world) - Zwift creates the world's shadow FBOs
// and binds them BEFORE attaching the depth texture, so the first look said "not a shadow map" forever.
// Now: attachment calls re-classify, and negatives are re-checked every 120 binds.
struct FboClass { bool shadow = false; unsigned bindsSinceCheck = 0; };
std::unordered_map<GLuint, FboClass> g_fboShadow;
unsigned g_shadowDrawsSkipped = 0, g_reflDrawsSkipped = 0;
// The resolve-ray rewrite state (defined with the shader hooks further down) is read by DRAW_HOOK.
extern GLuint g_rayProg;
extern unsigned g_rayDraws;

bool IsDepthFormat(GLint f) {
  return f == 0x1902 || f == 0x81A5 || f == 0x81A6 || f == 0x81A7 || f == 0x8CAC || f == 0x88F0 || f == 0x8CAD;
}

std::unordered_set<GLuint> g_shadowSet;  // FBOs currently classified as shadow maps

bool IsShadowFbo(GLuint fbo) {
  auto it = g_fboShadow.find(fbo);
  if (it != g_fboShadow.end() && (it->second.shadow || ++it->second.bindsSinceCheck < 120)) return it->second.shadow;
  bool shadow = false;
  unsigned recheckSoon = 0;  // 112 = look again after 8 binds (FBO still being built)
  if (pGetFbAttach && pGetTexLevel) {
    GLint ct = GL_NONE, dt = GL_NONE, dn = 0;
    pGetFbAttach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &ct);
    pGetFbAttach(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &dt);
    if (ct == GL_NONE && dt == GL_NONE) recheckSoon = 112;  // empty: attachments not added yet
    if (ct == GL_NONE && dt == GL_TEXTURE) {
      pGetFbAttach(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &dn);
      GLint w = 0, h = 0, fmt = 0;
      pGetTexLevel(static_cast<GLuint>(dn), 0, GL_TEXTURE_WIDTH, &w);
      pGetTexLevel(static_cast<GLuint>(dn), 0, GL_TEXTURE_HEIGHT, &h);
      pGetTexLevel(static_cast<GLuint>(dn), 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
      if (w == 0 || h == 0) recheckSoon = 112;  // texture has no storage yet
      shadow = w == h && w >= 512 && IsDepthFormat(fmt);
      if (shadow && g_shadowSet.insert(fbo).second)
        Log("shadows off: FBO %u is a shadow map (depth T%d %dx%d)", fbo, dn, w, h);
    }
  }
  if (!shadow) g_shadowSet.erase(fbo);
  g_fboShadow[fbo] = FboClass{shadow, recheckSoon};
  return shadow;
}

using PFN_glBindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
PFN_glBindFramebuffer o_glBindFramebuffer = nullptr;
void APIENTRY h_glBindFramebuffer(GLenum target, GLuint fbo) {
  o_glBindFramebuffer(target, fbo);
  if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
    g_curDrawFbo = fbo;
    g_drawFboIsShadow = g_skipShadows && fbo != 0 && IsShadowFbo(fbo);
    if (fbo == 0) g_camSkipReflDraws = false;  // safety net: never skip window / HUD drawing
    if (g_hudBlend) ApplyBlend();  // "over" alpha only while the window is the target
  }
}

// An attachment change re-classifies that FBO; if it is the bound draw FBO (create -> bind -> attach ->
// draw, no rebind), the skip flag is updated right away.
void OnAttachmentChanged(GLuint fbo) {
  g_fboShadow.erase(fbo);
  if (fbo != 0 && fbo == g_curDrawFbo) g_drawFboIsShadow = g_skipShadows && IsShadowFbo(fbo);
}
GLuint BoundFboFor(GLenum target) { return (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) ? g_curDrawFbo : 0; }

using PFN_glFramebufferTexture2D = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
PFN_glFramebufferTexture2D o_glFramebufferTexture2D = nullptr;
void APIENTRY h_glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) {
  o_glFramebufferTexture2D(t, a, tt, tex, l);
  OnAttachmentChanged(BoundFboFor(t));
}
using PFN_glFramebufferTexture = void(APIENTRY*)(GLenum, GLenum, GLuint, GLint);
PFN_glFramebufferTexture o_glFramebufferTexture = nullptr;
void APIENTRY h_glFramebufferTexture(GLenum t, GLenum a, GLuint tex, GLint l) {
  o_glFramebufferTexture(t, a, tex, l);
  OnAttachmentChanged(BoundFboFor(t));
}
using PFN_glFramebufferTextureLayer = void(APIENTRY*)(GLenum, GLenum, GLuint, GLint, GLint);
PFN_glFramebufferTextureLayer o_glFramebufferTextureLayer = nullptr;
void APIENTRY h_glFramebufferTextureLayer(GLenum t, GLenum a, GLuint tex, GLint l, GLint layer) {
  o_glFramebufferTextureLayer(t, a, tex, l, layer);
  OnAttachmentChanged(BoundFboFor(t));
}
using PFN_glNamedFramebufferTexture = void(APIENTRY*)(GLuint, GLenum, GLuint, GLint);
PFN_glNamedFramebufferTexture o_glNamedFramebufferTexture = nullptr;
void APIENTRY h_glNamedFramebufferTexture(GLuint fb, GLenum a, GLuint tex, GLint l) {
  o_glNamedFramebufferTexture(fb, a, tex, l);
  OnAttachmentChanged(fb);
}
using PFN_glNamedFramebufferTextureLayer = void(APIENTRY*)(GLuint, GLenum, GLuint, GLint, GLint);
PFN_glNamedFramebufferTextureLayer o_glNamedFramebufferTextureLayer = nullptr;
void APIENTRY h_glNamedFramebufferTextureLayer(GLuint fb, GLenum a, GLuint tex, GLint l, GLint layer) {
  o_glNamedFramebufferTextureLayer(fb, a, tex, l, layer);
  OnAttachmentChanged(fb);
}
using PFN_glDeleteFramebuffers = void(APIENTRY*)(GLsizei, const GLuint*);
PFN_glDeleteFramebuffers o_glDeleteFramebuffers = nullptr;
void APIENTRY h_glDeleteFramebuffers(GLsizei n, const GLuint* ids) {
  if (ids)
    for (GLsizei i = 0; i < n; ++i) {
      if (g_shadowSet.erase(ids[i])) Log("shadows off: FBO %u deleted (was a shadow map)", ids[i]);
      g_fboShadow.erase(ids[i]);
      if (ids[i] == g_curDrawFbo) {  // only the bound one affects the current skip state
        g_curDrawFbo = 0;
        g_drawFboIsShadow = false;
      }
    }
  o_glDeleteFramebuffers(n, ids);
}

// ---------------- hooks ----------------
#define DRAW_HOOK(NAME, PARAMS, ARGS)                       \
  using PFN_##NAME = void(APIENTRY*) PARAMS;                \
  PFN_##NAME o_##NAME = nullptr;                            \
  void APIENTRY h_##NAME PARAMS {                           \
    if (g_drawFboIsShadow) { if (CamActive()) g_shadowDrawsSkipped++; return; } \
    if (g_camSkipReflDraws) { g_reflDrawsSkipped++; return; } \
    if (g_rayProg && g_curProgram == static_cast<GLint>(g_rayProg) && CamActive()) g_rayDraws++; \
    if (g_curDrawFbo == 0) GlCaptureOnWindowDraw(); /* [hud]: world snapshot before the HUD */ \
    OnDraw(#NAME + 2);                                      \
    o_##NAME ARGS;                                          \
  }

DRAW_HOOK(glDrawArrays, (GLenum m, GLint f, GLsizei c), (m, f, c))
DRAW_HOOK(glDrawElements, (GLenum m, GLsizei c, GLenum t, const void* i), (m, c, t, i))
DRAW_HOOK(glDrawArraysInstanced, (GLenum m, GLint f, GLsizei c, GLsizei n), (m, f, c, n))
DRAW_HOOK(glDrawElementsInstanced, (GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n), (m, c, t, i, n))
DRAW_HOOK(glDrawRangeElements, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void* i), (m, s, e, c, t, i))
DRAW_HOOK(glDrawElementsBaseVertex, (GLenum m, GLsizei c, GLenum t, const void* i, GLint b), (m, c, t, i, b))
DRAW_HOOK(glDrawRangeElementsBaseVertex, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void* i, GLint b),
          (m, s, e, c, t, i, b))
DRAW_HOOK(glDrawElementsInstancedBaseVertex, (GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n, GLint b),
          (m, c, t, i, n, b))
DRAW_HOOK(glDrawArraysInstancedBaseInstance, (GLenum m, GLint f, GLsizei c, GLsizei n, GLuint bi), (m, f, c, n, bi))
DRAW_HOOK(glDrawElementsInstancedBaseInstance, (GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n, GLuint bi),
          (m, c, t, i, n, bi))
DRAW_HOOK(glDrawElementsInstancedBaseVertexBaseInstance,
          (GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n, GLint b, GLuint bi), (m, c, t, i, n, b, bi))
DRAW_HOOK(glDrawArraysIndirect, (GLenum m, const void* ind), (m, ind))
DRAW_HOOK(glDrawElementsIndirect, (GLenum m, GLenum t, const void* ind), (m, t, ind))
DRAW_HOOK(glMultiDrawArrays, (GLenum m, const GLint* f, const GLsizei* c, GLsizei n), (m, f, c, n))
DRAW_HOOK(glMultiDrawElements, (GLenum m, const GLsizei* c, GLenum t, const void* const* i, GLsizei n), (m, c, t, i, n))
DRAW_HOOK(glMultiDrawArraysIndirect, (GLenum m, const void* ind, GLsizei n, GLsizei s), (m, ind, n, s))
DRAW_HOOK(glMultiDrawElementsIndirect, (GLenum m, GLenum t, const void* ind, GLsizei n, GLsizei s), (m, t, ind, n, s))

std::string ClearMask(GLbitfield m) {
  std::string s;
  if (m & GL_COLOR_BUFFER_BIT) s += "C";
  if (m & GL_DEPTH_BUFFER_BIT) s += "D";
  if (m & GL_STENCIL_BUFFER_BIT) s += "S";
  return s;
}

// [hud] window_alpha: while Zwift draws its HUD into the window (from the world snapshot to the swap), the ALPHA
// factors of every blend are forced to "over" (ONE, ONE_MINUS_SRC_ALPHA), so the window's alpha ends up as the HUD's
// true coverage whatever alpha factors Zwift asked for (some panels, e.g. the riders list, wrote too much alpha and
// kept moving scenery in their background). The colour factors - what the monitor shows - stay Zwift's. Zwift's
// requested state is tracked and put back at swap.
using PFN_glBlendFunc = void(APIENTRY*)(GLenum, GLenum);
using PFN_glBlendFuncSeparate = void(APIENTRY*)(GLenum, GLenum, GLenum, GLenum);
PFN_glBlendFunc o_glBlendFunc = nullptr;
PFN_glBlendFuncSeparate o_glBlendFuncSeparate = nullptr;
GLenum g_blendReq[4] = {GL_ONE, GL_ZERO, GL_ONE, GL_ZERO};  // Zwift's latest srcRGB, dstRGB, srcA, dstA

// Zwift's requested blend, with "over" alpha only while the HUD draws into the WINDOW: blend state is global, and an
// offscreen pass in the HUD phase (e.g. a minimap texture) must keep Zwift's own alpha. Re-applied on every
// framebuffer bind while the override is on.
void ApplyBlend() {
  if (!o_glBlendFuncSeparate) return;
  if (g_hudBlend && g_curDrawFbo == 0) o_glBlendFuncSeparate(g_blendReq[0], g_blendReq[1], GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  else o_glBlendFuncSeparate(g_blendReq[0], g_blendReq[1], g_blendReq[2], g_blendReq[3]);
}

void APIENTRY h_glBlendFunc(GLenum s, GLenum d) {
  g_blendReq[0] = g_blendReq[2] = s;
  g_blendReq[1] = g_blendReq[3] = d;
  if (g_hudBlend) { ApplyBlend(); return; }
  o_glBlendFunc(s, d);
}
void APIENTRY h_glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
  g_blendReq[0] = sr;
  g_blendReq[1] = dr;
  g_blendReq[2] = sa;
  g_blendReq[3] = da;
  if (g_hudBlend) { ApplyBlend(); return; }
  o_glBlendFuncSeparate(sr, dr, sa, da);
}

using PFN_glClear = void(APIENTRY*)(GLbitfield);
PFN_glClear o_glClear = nullptr;
void APIENTRY h_glClear(GLbitfield m) { OnClear(("clear:" + ClearMask(m)).c_str()); o_glClear(m); }

using PFN_glClearBufferfv = void(APIENTRY*)(GLenum, GLint, const GLfloat*);
PFN_glClearBufferfv o_glClearBufferfv = nullptr;
void APIENTRY h_glClearBufferfv(GLenum b, GLint d, const GLfloat* v) {
  if (g_camSkipReflDraws && g_camHaveReflClear && b == GL_COLOR && d == 0) v = g_camReflClear;
  OnClear(b == GL_DEPTH ? "clearbuf:D" : ("clearbuf:C" + std::to_string(d)).c_str());
  o_glClearBufferfv(b, d, v);
}
using PFN_glClearBufferfi = void(APIENTRY*)(GLenum, GLint, GLfloat, GLint);
PFN_glClearBufferfi o_glClearBufferfi = nullptr;
void APIENTRY h_glClearBufferfi(GLenum b, GLint d, GLfloat dep, GLint st) {
  OnClear("clearbuf:DS");
  o_glClearBufferfi(b, d, dep, st);
}

using PFN_glBlitFramebuffer = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
PFN_glBlitFramebuffer o_glBlitFramebuffer = nullptr;
void APIENTRY h_glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h,
                                  GLbitfield m, GLenum fl) {
  if (Active()) {
    GLint rf = 0, df = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rf);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &df);
    char buf[200];
    sprintf_s(buf, "  BLIT fbo %d (%d,%d)-(%d,%d) -> fbo %d (%d,%d)-(%d,%d) mask %s %s", rf, a, b, c, d, df, e, f, g,
              h, ClearMask(m).c_str(), fl == GL_LINEAR ? "linear" : "nearest");
    OnEvent(buf);
  }
  o_glBlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
}

using PFN_glBlitNamedFramebuffer = void(APIENTRY*)(GLuint, GLuint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                                   GLint, GLbitfield, GLenum);
PFN_glBlitNamedFramebuffer o_glBlitNamedFramebuffer = nullptr;
void APIENTRY h_glBlitNamedFramebuffer(GLuint r, GLuint dd, GLint a, GLint b, GLint c, GLint d, GLint e, GLint f,
                                       GLint g, GLint h, GLbitfield m, GLenum fl) {
  if (Active()) {
    char buf[200];
    sprintf_s(buf, "  BLIT(named) fbo %u (%d,%d)-(%d,%d) -> fbo %u (%d,%d)-(%d,%d) mask %s", r, a, b, c, d, dd, e, f,
              g, h, ClearMask(m).c_str());
    OnEvent(buf);
  }
  o_glBlitNamedFramebuffer(r, dd, a, b, c, d, e, f, g, h, m, fl);
}

using PFN_glCopyImageSubData = void(APIENTRY*)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint,
                                               GLint, GLint, GLsizei, GLsizei, GLsizei);
PFN_glCopyImageSubData o_glCopyImageSubData = nullptr;
void APIENTRY h_glCopyImageSubData(GLuint sn, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz, GLuint dn, GLenum dt,
                                   GLint dl, GLint dx, GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei d) {
  if (Active()) {
    char buf[160];
    sprintf_s(buf, "  COPYIMAGE %u (target 0x%X) -> %u (target 0x%X) %dx%d", sn, st, dn, dt, w, h);
    OnEvent(buf);
  }
  o_glCopyImageSubData(sn, st, sl, sx, sy, sz, dn, dt, dl, dx, dy, dz, w, h, d);
}

// --- current program, tracked so the camera override never has to glGet it ---
// (glGetIntegerv per uniform upload forced hundreds of driver syncs per frame with NVIDIA's threaded
// GL driver: Zwift fell to ~25 fps in the first immersive test.)
using PFN_glUseProgram = void(APIENTRY*)(GLuint);
PFN_glUseProgram o_glUseProgram = nullptr;
void APIENTRY h_glUseProgram(GLuint p) {
  g_curProgram = static_cast<GLint>(p);
  o_glUseProgram(p);
}

// --- matrix-upload hooks (camera recon) ---
using PFN_glUniformMatrix4fv = void(APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);
PFN_glUniformMatrix4fv o_glUniformMatrix4fv = nullptr;
void APIENTRY h_glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat* v) {
  if (Active()) {
    g_uniformCalls++;
    GLint prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    ScanFloats(UniformPrefix(prog, loc) + " mat4", v, static_cast<size_t>(n) * 16, tr != 0);
    if (n == 1) RecordUniformMat(prog, loc, v, tr != 0);
  }
  if (n == 1 && CamActive()) {
    float patched[16];
    if (CamOverrideUniform(g_curProgram, loc, v, tr != 0, patched)) {
      o_glUniformMatrix4fv(loc, 1, GL_FALSE, patched);
      return;
    }
  }
  o_glUniformMatrix4fv(loc, n, tr, v);
}

using PFN_glProgramUniformMatrix4fv = void(APIENTRY*)(GLuint, GLint, GLsizei, GLboolean, const GLfloat*);
PFN_glProgramUniformMatrix4fv o_glProgramUniformMatrix4fv = nullptr;
void APIENTRY h_glProgramUniformMatrix4fv(GLuint p, GLint loc, GLsizei n, GLboolean tr, const GLfloat* v) {
  if (Active()) {
    g_uniformCalls++;
    ScanFloats(UniformPrefix(static_cast<GLint>(p), loc) + " mat4", v, static_cast<size_t>(n) * 16, tr != 0);
    if (n == 1) RecordUniformMat(static_cast<GLint>(p), loc, v, tr != 0);
  }
  if (n == 1 && CamActive()) {
    float patched[16];
    if (CamOverrideUniform(static_cast<GLint>(p), loc, v, tr != 0, patched)) {
      o_glProgramUniformMatrix4fv(p, loc, 1, GL_FALSE, patched);
      return;
    }
  }
  o_glProgramUniformMatrix4fv(p, loc, n, tr, v);
}

using PFN_glUniform4fv = void(APIENTRY*)(GLint, GLsizei, const GLfloat*);
PFN_glUniform4fv o_glUniform4fv = nullptr;
void APIENTRY h_glUniform4fv(GLint loc, GLsizei n, const GLfloat* v) {
  if (Active() && n >= 4) {  // matrices passed as vec4 arrays
    g_uniformCalls++;
    GLint prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    ScanFloats(UniformPrefix(prog, loc) + " vec4[]", v, static_cast<size_t>(n) * 4, false);
  }
  o_glUniform4fv(loc, n, v);
}

GLint BoundBuffer(GLenum target) {
  GLenum binding = 0;
  switch (target) {
    case 0x8A11: binding = 0x8A28; break;  // UNIFORM_BUFFER
    case 0x8892: binding = 0x8894; break;  // ARRAY_BUFFER
    case 0x90D2: binding = 0x90D3; break;  // SHADER_STORAGE_BUFFER
    case 0x8F37: binding = 0x8F37; break;  // COPY_WRITE_BUFFER
    case 0x8F36: binding = 0x8F36; break;  // COPY_READ_BUFFER
    default: return -1;
  }
  GLint b = 0;
  glGetIntegerv(binding, &b);
  return b;
}

void ScanBufferWrite(const std::string& prefix, ptrdiff_t offset, ptrdiff_t size, const void* data) {
  g_bufferCalls++;
  g_bufferBytes += static_cast<unsigned long long>(size);
  if (!data || size <= 0 || size > 256 * 1024 || (offset & 3)) return;
  // key offsets are absolute within the buffer so the same slot matches across calls
  const float* f = static_cast<const float*>(data);
  for (ptrdiff_t i = 0; i + 64 <= size; i += 16) {
    float m[16], t[16];
    memcpy(m, f + i / 4, sizeof(m));
    Transpose(m, t);
    std::string key = prefix + "+" + std::to_string(offset + i);
    if (IsPerspective(m)) Record(g_projSrc, key, m, false);
    else if (IsPerspective(t)) Record(g_projSrc, key, t, true);
    else if (IsRigid(m)) Record(g_rigidSrc, key, m, false);
    else if (IsRigid(t)) Record(g_rigidSrc, key, t, true);
  }
}

// Buffer writes from threads other than the traced render thread: copied aside under a lock (the
// main recon maps aren't thread-safe) and checked against V/P at dump time.
struct OtherWrite {
  DWORD tid;
  std::string what;
  unsigned id;
  ptrdiff_t offset, size;
  std::vector<float> data;
};
std::mutex g_otherMx;
std::vector<OtherWrite> g_otherWrites;

void RecordOtherThreadWrite(const char* what, unsigned id, ptrdiff_t off, ptrdiff_t size, const void* data) {
  std::lock_guard lock(g_otherMx);
  if (g_otherWrites.size() >= 2000) return;
  OtherWrite w{GetCurrentThreadId(), what, id, off, size, {}};
  if (data && size > 0 && size <= 64 * 1024 && !(off & 3)) {
    w.data.resize(static_cast<size_t>(size) / 4);
    memcpy(w.data.data(), data, w.data.size() * 4);
  }
  g_otherWrites.push_back(std::move(w));
}

using PFN_glBufferSubData = void(APIENTRY*)(GLenum, ptrdiff_t, ptrdiff_t, const void*);
PFN_glBufferSubData o_glBufferSubData = nullptr;
void APIENTRY h_glBufferSubData(GLenum target, ptrdiff_t off, ptrdiff_t size, const void* data) {
  if (g_tracing && !Active()) RecordOtherThreadWrite("BufferSubData", target, off, size, data);
  if (Active()) {
    char p[48];
    sprintf_s(p, "buf%d(target 0x%X)", BoundBuffer(target), target);
    ScanBufferWrite(p, off, size, data);
  }
  o_glBufferSubData(target, off, size, data);
}

using PFN_glNamedBufferSubData = void(APIENTRY*)(GLuint, ptrdiff_t, ptrdiff_t, const void*);
PFN_glNamedBufferSubData o_glNamedBufferSubData = nullptr;
void APIENTRY h_glNamedBufferSubData(GLuint buf, ptrdiff_t off, ptrdiff_t size, const void* data) {
  if (g_tracing && !Active()) RecordOtherThreadWrite("NamedBufferSubData", buf, off, size, data);
  if (Active()) ScanBufferWrite("buf" + std::to_string(buf) + "(named)", off, size, data);
  o_glNamedBufferSubData(buf, off, size, data);
}

using PFN_glBufferData = void(APIENTRY*)(GLenum, ptrdiff_t, const void*, GLenum);
PFN_glBufferData o_glBufferData = nullptr;
void APIENTRY h_glBufferData(GLenum target, ptrdiff_t size, const void* data, GLenum usage) {
  if (g_tracing && !Active()) RecordOtherThreadWrite("BufferData", target, 0, size, data);
  if (Active()) {
    char p[48];
    sprintf_s(p, "buf%d(target 0x%X, BufferData)", BoundBuffer(target), target);
    ScanBufferWrite(p, 0, size, data);
  }
  o_glBufferData(target, size, data, usage);
}

// --- mapped buffers ---
// Writes through a mapped pointer are invisible to hooks. The first immersive eye dump proved the bulk
// of Zwift's geometry does NOT take its camera from the uniforms we patch, and the trace shows 3-4
// buffer maps per frame, so record every mapping in the traced frame and snapshot its memory at unmap
// (or at trace end for persistent mappings), then search it for V / P / P*V at dump time.
#define GL_MAP_PERSISTENT_BIT 0x0040
#define GL_MAP_COHERENT_BIT 0x0080
struct MapRec {
  GLint buffer = -1;
  GLenum target = 0;
  ptrdiff_t offset = 0, length = 0;
  GLbitfield access = 0;
  void* ptr = nullptr;
  bool unmapped = false;
  std::vector<float> snap;
};
std::vector<MapRec> g_maps;
constexpr ptrdiff_t kMaxSnap = 1 << 20;  // bytes

// No C++ objects with destructors in here, so SEH can guard reading a mapping that may be gone.
bool SafeCopy(void* dst, const void* src, size_t n) {
  __try {
    memcpy(dst, src, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Snapshot(MapRec& r) {
  if (!r.ptr || r.length <= 0) return;
  const size_t n = static_cast<size_t>(std::min(r.length, kMaxSnap)) / 4;
  r.snap.resize(n);
  if (!SafeCopy(r.snap.data(), r.ptr, n * 4)) r.snap.clear();
}

void RecordMap(GLint buffer, GLenum target, ptrdiff_t off, ptrdiff_t len, GLbitfield access, void* ptr) {
  g_mapCalls++;
  MapRec r;
  r.buffer = buffer;
  r.target = target;
  r.offset = off;
  r.length = len;
  r.access = access;
  r.ptr = ptr;
  g_maps.push_back(std::move(r));
}

void RecordUnmap(GLint buffer) {
  for (auto it = g_maps.rbegin(); it != g_maps.rend(); ++it)
    if (it->buffer == buffer && !it->unmapped) {
      Snapshot(*it);
      it->unmapped = true;
      return;
    }
}

using PFN_glMapBufferRange = void*(APIENTRY*)(GLenum, ptrdiff_t, ptrdiff_t, GLbitfield);
PFN_glMapBufferRange o_glMapBufferRange = nullptr;
void* APIENTRY h_glMapBufferRange(GLenum t, ptrdiff_t o, ptrdiff_t l, GLbitfield a) {
  void* p = o_glMapBufferRange(t, o, l, a);
  if (Active()) RecordMap(BoundBuffer(t), t, o, l, a, p);
  return p;
}
using PFN_glMapNamedBufferRange = void*(APIENTRY*)(GLuint, ptrdiff_t, ptrdiff_t, GLbitfield);
PFN_glMapNamedBufferRange o_glMapNamedBufferRange = nullptr;
void* APIENTRY h_glMapNamedBufferRange(GLuint b, ptrdiff_t o, ptrdiff_t l, GLbitfield a) {
  void* p = o_glMapNamedBufferRange(b, o, l, a);
  if (Active()) RecordMap(static_cast<GLint>(b), 0, o, l, a, p);
  return p;
}
using PFN_glMapBuffer = void*(APIENTRY*)(GLenum, GLenum);
PFN_glMapBuffer o_glMapBuffer = nullptr;
void* APIENTRY h_glMapBuffer(GLenum t, GLenum access) {
  void* p = o_glMapBuffer(t, access);
  if (Active()) {
    GLint size = 0;
    using PFN_GetBufferParameteriv = void(APIENTRY*)(GLenum, GLenum, GLint*);
    static auto getParam = reinterpret_cast<PFN_GetBufferParameteriv>(wglGetProcAddress("glGetBufferParameteriv"));
    if (getParam) getParam(t, 0x8764 /* BUFFER_SIZE */, &size);
    RecordMap(BoundBuffer(t), t, 0, size, 0, p);
  }
  return p;
}
using PFN_glUnmapBuffer = GLboolean(APIENTRY*)(GLenum);
PFN_glUnmapBuffer o_glUnmapBuffer = nullptr;
GLboolean APIENTRY h_glUnmapBuffer(GLenum t) {
  if (Active()) RecordUnmap(BoundBuffer(t));  // snapshot BEFORE the pointer becomes invalid
  return o_glUnmapBuffer(t);
}
using PFN_glUnmapNamedBuffer = GLboolean(APIENTRY*)(GLuint);
PFN_glUnmapNamedBuffer o_glUnmapNamedBuffer = nullptr;
GLboolean APIENTRY h_glUnmapNamedBuffer(GLuint b) {
  if (Active()) RecordUnmap(static_cast<GLint>(b));
  return o_glUnmapNamedBuffer(b);
}

// --- buffer-range binds: where the world's camera (mapped ring buffer) gets patched ---
using PFN_glBindBufferRange = void(APIENTRY*)(GLenum, GLuint, GLuint, ptrdiff_t, ptrdiff_t);
PFN_glBindBufferRange o_glBindBufferRange = nullptr;
void APIENTRY h_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, ptrdiff_t offset, ptrdiff_t size) {
  if (CamActive()) CamOnBindRange(target, index, buffer, offset, size);
  o_glBindBufferRange(target, index, buffer, offset, size);
}
using PFN_glBindBuffersRange = void(APIENTRY*)(GLenum, GLuint, GLsizei, const GLuint*, const ptrdiff_t*,
                                               const ptrdiff_t*);
PFN_glBindBuffersRange o_glBindBuffersRange = nullptr;
void APIENTRY h_glBindBuffersRange(GLenum target, GLuint first, GLsizei count, const GLuint* buffers,
                                   const ptrdiff_t* offsets, const ptrdiff_t* sizes) {
  if (CamActive() && buffers && offsets && sizes)
    for (GLsizei i = 0; i < count; ++i)
      CamOnBindRange(target, first + static_cast<GLuint>(i), buffers[i], offsets[i], sizes[i]);
  o_glBindBuffersRange(target, first, count, buffers, offsets, sizes);
}

// --- shader source capture (insurance for the camera work): every glShaderSource Zwift calls after
// injection goes to %LOCALAPPDATA%\zwift-vr\shaders\s<id>.glsl, program<->shader links to attach.txt.
// World shaders are compiled at world load, after the hooks go in, so they are captured.
unsigned g_shaderFiles = 0;
std::wstring ShaderDir() {
  wchar_t dir[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return L"";
  std::wstring d = std::wstring(dir) + L"\\zwift-vr\\shaders";
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}
using PFN_glShaderSource = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
PFN_glShaderSource o_glShaderSource = nullptr;

// --- deferred-resolve ray rewrite (camera consistency) ---
// Zwift's deferred lighting resolve (prog 766: VS s764 + FS s765) rebuilds world position as
// P = viewPos + l_norm * depth, where l_norm = i_norm is a per-corner view ray Zwift computes on the CPU
// from ITS OWN camera. With our patched viewPos/view.m/depth the reconstructed point swings with the head
// (shadows, specular, wet/IBL reflections and fog slid ~1.7 m at 10 m per 10 deg). The VS is rewritten to
// build the ray from the B_Scene slice bound at draw time: ray = R^T (ndc.xy * -scale.w / scale.xy, -1).
// For an unpatched slice that is exactly Zwift's ray (immersive off = stock). Trial-compiled; the link hook
// restores the original text if the program fails to link. zvr_legacy = 1 (Ctrl+Shift+F4) uses i_norm again.
std::mutex g_rwMx;
std::unordered_map<GLuint, std::string> g_rwOrig;  // rewritten shader -> original source
std::unordered_map<GLuint, GLuint> g_rwProgOf;     // rewritten shader -> program it was attached to
GLuint g_rayProg = 0;
GLint g_rayLegacyLoc = -1;
bool g_rayLegacy = false;
unsigned g_rayDraws = 0;

bool RewriteResolveVS(std::string& s) {
  if (s.compare(0, 9, "#version ") != 0) return false;
  const size_t pos = s.find("gl_Position = i_pos;"), ray = s.find("l_norm = i_norm;");
  if (pos == std::string::npos || ray == std::string::npos || s.find("B_Scene") != std::string::npos) return false;
  const size_t nl = s.find('\n');
  if (nl == std::string::npos) return false;
  s.replace(ray, 16,
            "l_norm = (zvr_legacy != 0u) ? i_norm : inverse(mat3(zvr.m)) * vec3((i_pos.xy / i_pos.w) * (-zvr.scale.w) / "
            "zvr.scale.xy, -1.0);");
  s.insert(nl + 1,
           "layout(binding = 5, std430) readonly buffer ZVR_Scene { uvec4 s0; uvec4 s1; mat4 m; vec4 viewPos; vec4 dir; "
           "vec4 clip; vec4 scale; vec4 bias; } zvr;\nuniform uint zvr_legacy;\n");
  return true;
}

using PFN_CreateShader = GLuint(APIENTRY*)(GLenum);
using PFN_CompileShader = void(APIENTRY*)(GLuint);
using PFN_GetShaderiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_GetShaderInfoLog = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
using PFN_DeleteShader = void(APIENTRY*)(GLuint);

bool TrialCompile(GLenum type, const std::string& src, std::string& err) {
  static auto create = reinterpret_cast<PFN_CreateShader>(wglGetProcAddress("glCreateShader"));
  static auto compile = reinterpret_cast<PFN_CompileShader>(wglGetProcAddress("glCompileShader"));
  static auto getiv = reinterpret_cast<PFN_GetShaderiv>(wglGetProcAddress("glGetShaderiv"));
  static auto infolog = reinterpret_cast<PFN_GetShaderInfoLog>(wglGetProcAddress("glGetShaderInfoLog"));
  static auto del = reinterpret_cast<PFN_DeleteShader>(wglGetProcAddress("glDeleteShader"));
  if (!create || !compile || !getiv || !infolog || !del || !o_glShaderSource) { err = "GL entry points missing"; return false; }
  const GLuint sh = create(type);
  if (!sh) { err = "glCreateShader failed"; return false; }
  const char* p = src.c_str();
  o_glShaderSource(sh, 1, &p, nullptr);  // trampoline: not captured, not rewritten again
  compile(sh);
  GLint ok = 0;
  getiv(sh, 0x8B81 /* COMPILE_STATUS */, &ok);
  if (!ok) {
    char log[1024] = {};
    infolog(sh, sizeof(log), nullptr, log);
    err = log;
  }
  del(sh);
  return ok != 0;
}

void APIENTRY h_glShaderSource(GLuint shader, GLsizei count, const char* const* str, const GLint* len) {
  if (g_shaderFiles < 3000 && str && count > 0) {
    static const std::wstring dir = ShaderDir();
    if (!dir.empty()) {
      FILE* f = nullptr;
      if (!_wfopen_s(&f, (dir + L"\\s" + std::to_wstring(shader) + L".glsl").c_str(), L"wb") && f) {
        for (GLsizei i = 0; i < count; ++i) {
          if (!str[i]) continue;
          const size_t n = (len && len[i] >= 0) ? static_cast<size_t>(len[i]) : strlen(str[i]);
          fwrite(str[i], 1, n, f);
        }
        fclose(f);
        g_shaderFiles++;
      }
    }
  }
  // The deferred-resolve VS is small and contains no B_Scene: cheap to recognise exactly.
  if (str && count > 0) {
    std::string s;
    for (GLsizei i = 0; i < count; ++i) {
      if (!str[i]) continue;
      s.append(str[i], (len && len[i] >= 0) ? static_cast<size_t>(len[i]) : strlen(str[i]));
    }
    s.erase(std::remove(s.begin(), s.end(), '\0'), s.end());
    if (s.size() < 4096 && s.find("l_norm = i_norm;") != std::string::npos) {
      const std::string orig = s;
      std::string err;
      if (RewriteResolveVS(s) && TrialCompile(0x8B31 /* VERTEX_SHADER */, s, err)) {
        {
          std::lock_guard lock(g_rwMx);
          g_rwOrig[shader] = orig;
        }
        const char* p = s.c_str();
        o_glShaderSource(shader, 1, &p, nullptr);
        Log("resolve VS: shader %u builds its view rays from the bound B_Scene (rewrite compiled OK)", shader);
        return;
      }
      Log("resolve VS: shader %u NOT rewritten (%s) - Zwift's own rays", shader, err.empty() ? "pattern" : err.c_str());
    }
  }
  o_glShaderSource(shader, count, str, len);
}
using PFN_glAttachShader = void(APIENTRY*)(GLuint, GLuint);
PFN_glAttachShader o_glAttachShader = nullptr;
void APIENTRY h_glAttachShader(GLuint prog, GLuint shader) {
  static const std::wstring dir = ShaderDir();
  if (!dir.empty()) {
    FILE* f = nullptr;
    if (!_wfopen_s(&f, (dir + L"\\attach.txt").c_str(), L"a") && f) {
      fprintf(f, "prog %u shader %u\n", prog, shader);
      fclose(f);
    }
  }
  o_glAttachShader(prog, shader);
  std::lock_guard lock(g_rwMx);
  if (g_rwOrig.count(shader)) g_rwProgOf[shader] = prog;
}

using PFN_GetProgramiv2 = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_GetUniformLocation2 = GLint(APIENTRY*)(GLuint, const char*);
using PFN_ProgramUniform1ui = void(APIENTRY*)(GLuint, GLint, GLuint);
using PFN_glLinkProgram = void(APIENTRY*)(GLuint);
PFN_glLinkProgram o_glLinkProgram = nullptr;
void APIENTRY h_glLinkProgram(GLuint prog) {
  o_glLinkProgram(prog);
  std::vector<GLuint> mine;
  {
    std::lock_guard lock(g_rwMx);
    for (auto& [sh, p] : g_rwProgOf) if (p == prog) mine.push_back(sh);
  }
  if (mine.empty()) return;
  static auto getiv = reinterpret_cast<PFN_GetProgramiv2>(wglGetProcAddress("glGetProgramiv"));
  static auto getLoc = reinterpret_cast<PFN_GetUniformLocation2>(wglGetProcAddress("glGetUniformLocation"));
  static auto compile = reinterpret_cast<PFN_CompileShader>(wglGetProcAddress("glCompileShader"));
  GLint ok = 0;
  if (getiv) getiv(prog, 0x8B82 /* LINK_STATUS */, &ok);
  if (ok) {
    g_rayProg = prog;
    g_rayLegacyLoc = getLoc ? getLoc(prog, "zvr_legacy") : -1;
    if (g_rayLegacy && g_rayLegacyLoc >= 0) {
      static auto set1ui = reinterpret_cast<PFN_ProgramUniform1ui>(wglGetProcAddress("glProgramUniform1ui"));
      if (set1ui) set1ui(prog, g_rayLegacyLoc, 1u);
    }
    Log("resolve VS: rewritten program %u linked OK (zvr_legacy @%d)", prog, g_rayLegacyLoc);
    return;
  }
  // A bad rewrite must never black out the world: put Zwift's original text back and relink.
  Log("resolve VS: program %u FAILED to link with the rewrite - restoring Zwift's original shader", prog);
  for (GLuint sh : mine) {
    std::string orig;
    {
      std::lock_guard lock(g_rwMx);
      orig = g_rwOrig[sh];
      g_rwOrig.erase(sh);
      g_rwProgOf.erase(sh);
    }
    const char* p = orig.c_str();
    o_glShaderSource(sh, 1, &p, nullptr);
    if (compile) compile(sh);
  }
  o_glLinkProgram(prog);
}

struct HookDef { const char* name; void* detour; void** orig; bool exported; };

#define H(NAME, EXP) {#NAME, reinterpret_cast<void*>(&h_##NAME), reinterpret_cast<void**>(&o_##NAME), EXP}
const HookDef kHooks[] = {
    H(glDrawArrays, true), H(glDrawElements, true), H(glClear, true),
    H(glDrawArraysInstanced, false), H(glDrawElementsInstanced, false), H(glDrawRangeElements, false),
    H(glDrawElementsBaseVertex, false), H(glDrawRangeElementsBaseVertex, false),
    H(glDrawElementsInstancedBaseVertex, false), H(glDrawArraysInstancedBaseInstance, false),
    H(glDrawElementsInstancedBaseInstance, false), H(glDrawElementsInstancedBaseVertexBaseInstance, false),
    H(glDrawArraysIndirect, false), H(glDrawElementsIndirect, false), H(glMultiDrawArrays, false),
    H(glMultiDrawElements, false), H(glMultiDrawArraysIndirect, false), H(glMultiDrawElementsIndirect, false),
    H(glClearBufferfv, false), H(glClearBufferfi, false), H(glBlitFramebuffer, false),
    H(glBlitNamedFramebuffer, false), H(glCopyImageSubData, false),
    H(glUniformMatrix4fv, false), H(glProgramUniformMatrix4fv, false), H(glUniform4fv, false),
    H(glBufferSubData, false), H(glNamedBufferSubData, false), H(glBufferData, false),
    H(glMapBufferRange, false), H(glMapNamedBufferRange, false), H(glUseProgram, false),
    H(glBindFramebuffer, false), H(glDeleteFramebuffers, false), H(glFramebufferTexture2D, false),
    H(glFramebufferTexture, false), H(glFramebufferTextureLayer, false), H(glNamedFramebufferTexture, false),
    H(glNamedFramebufferTextureLayer, false), H(glLinkProgram, false),
    H(glMapBuffer, false), H(glUnmapBuffer, false), H(glUnmapNamedBuffer, false),
    H(glBindBufferRange, false), H(glBindBuffersRange, false), H(glShaderSource, false), H(glAttachShader, false),
    H(glBlendFunc, true), H(glBlendFuncSeparate, false),
};
#undef H

bool ValidProc(void* p) {
  auto v = reinterpret_cast<intptr_t>(p);
  return p && v != 1 && v != 2 && v != 3 && v != -1;
}

void InstallHooks() {
  pGetFbAttach = reinterpret_cast<PFN_GetFbAttachParam>(wglGetProcAddress("glGetFramebufferAttachmentParameteriv"));
  pGetTexLevel = reinterpret_cast<PFN_GetTexLevelParam>(wglGetProcAddress("glGetTextureLevelParameteriv"));
  pGetTexParam = reinterpret_cast<PFN_GetTexParam>(wglGetProcAddress("glGetTextureParameteriv"));
  pGetRbParam = reinterpret_cast<PFN_GetRbParam>(wglGetProcAddress("glGetNamedRenderbufferParameteriv"));
  if (!pGetFbAttach) { Log("trace: glGetFramebufferAttachmentParameteriv missing - tracer disabled"); return; }

  unsigned ok = 0;
  std::string missing;
  for (const HookDef& h : kHooks) {
    MH_STATUS st;
    if (h.exported) {
      st = MH_CreateHookApi(L"opengl32", h.name, h.detour, h.orig);
    } else {
      void* p = reinterpret_cast<void*>(wglGetProcAddress(h.name));
      if (!ValidProc(p)) { missing += std::string(" ") + h.name; continue; }
      st = MH_CreateHook(p, h.detour, h.orig);
    }
    if (st == MH_OK) {
      ok++;
    } else {
      Log("trace: hook %s: %s", h.name, MH_StatusToString(st));
    }
  }
  MH_EnableHook(MH_ALL_HOOKS);
  glGetIntegerv(GL_CURRENT_PROGRAM, &g_curProgram);  // once; glUseProgram hook keeps it current
  Log("trace: %u GL hooks installed (press Ctrl+Shift+F12 in Zwift to trace one frame)%s%s", ok,
      missing.empty() ? "" : "; not available:", missing.c_str());
}

// ---------------- camera hunt: where do the main draws get their camera? ----------------
struct Rel { const char* name; const float* m; };

void ScanFloatsForCamera(FILE* f, const char* indent, size_t base, const float* d, size_t n, const Rel* rels,
                         int nrels, const float (*Ts)[16], unsigned maxLines) {
  unsigned lines = 0;
  for (size_t i = 0; i + 16 <= n && lines < maxLines; ++i) {  // every 4-byte offset (std140 or packed)
    for (int k = 0; k < nrels; ++k) {
      const double a = Diff(d + i, rels[k].m), b = Diff(d + i, Ts[k]);
      if (a < 2e-2 || b < 2e-2) {
        fprintf(f, "%s+%-7zu = %s%s (err %.1e)\n", indent, base + i * 4, rels[k].name, a < b ? "" : " TRANSPOSED",
                std::min(a, b));
        lines++;
        i += 15;
        break;
      }
    }
  }
}

using PFN_GetActiveUniformBlockName = void(APIENTRY*)(GLuint, GLuint, GLsizei, GLsizei*, char*);
using PFN_GetActiveUniformBlockiv = void(APIENTRY*)(GLuint, GLuint, GLenum, GLint*);
using PFN_GetIntegeri_v = void(APIENTRY*)(GLenum, GLuint, GLint*);
using PFN_GetInteger64i_v = void(APIENTRY*)(GLenum, GLuint, long long*);
using PFN_GetNamedBufferSubData = void(APIENTRY*)(GLuint, ptrdiff_t, ptrdiff_t, void*);
using PFN_GetProgramInterfaceiv = void(APIENTRY*)(GLuint, GLenum, GLenum, GLint*);
using PFN_GetProgramResourceName = void(APIENTRY*)(GLuint, GLenum, GLuint, GLsizei, GLsizei*, char*);
using PFN_GetProgramResourceiv = void(APIENTRY*)(GLuint, GLenum, GLuint, GLsizei, const GLenum*, GLsizei, GLsizei*,
                                                 GLint*);

// Reads a bound buffer range and reports any camera matrices in it.
void DumpBufferRange(FILE* f, GLuint buf, long long start, long long size, const Rel* rels, int nrels,
                     const float (*Ts)[16]) {
  static auto getSub = reinterpret_cast<PFN_GetNamedBufferSubData>(wglGetProcAddress("glGetNamedBufferSubData"));
  if (!getSub || !buf || size <= 0) return;
  size = std::min(size, 64LL * 1024);
  std::vector<float> d(static_cast<size_t>(size) / 4);
  while (glGetError() != GL_NO_ERROR) {}
  getSub(buf, static_cast<ptrdiff_t>(start), static_cast<ptrdiff_t>(d.size() * 4), d.data());
  if (GLenum e = glGetError()) { fprintf(f, "        (read failed: GL 0x%X)\n", e); return; }
  ScanFloatsForCamera(f, "        ", static_cast<size_t>(start), d.data(), d.size(), rels, nrels, Ts, 12);
}

void ProgramReport(FILE* f, GLint prog, unsigned draws, const Rel* rels, int nrels, const float (*Ts)[16]) {
  static auto getProgramiv = reinterpret_cast<PFN_GetProgramiv>(wglGetProcAddress("glGetProgramiv"));
  static auto blockName = reinterpret_cast<PFN_GetActiveUniformBlockName>(wglGetProcAddress("glGetActiveUniformBlockName"));
  static auto blockiv = reinterpret_cast<PFN_GetActiveUniformBlockiv>(wglGetProcAddress("glGetActiveUniformBlockiv"));
  static auto geti = reinterpret_cast<PFN_GetIntegeri_v>(wglGetProcAddress("glGetIntegeri_v"));
  static auto geti64 = reinterpret_cast<PFN_GetInteger64i_v>(wglGetProcAddress("glGetInteger64i_v"));
  static auto ifaceiv = reinterpret_cast<PFN_GetProgramInterfaceiv>(wglGetProcAddress("glGetProgramInterfaceiv"));
  static auto resName = reinterpret_cast<PFN_GetProgramResourceName>(wglGetProcAddress("glGetProgramResourceName"));
  static auto resiv = reinterpret_cast<PFN_GetProgramResourceiv>(wglGetProcAddress("glGetProgramResourceiv"));

  fprintf(f, "  prog %d: %u draws\n    uniforms:", prog, draws);
  unsigned k = 0;
  for (auto& [loc, name] : UniformNames(prog)) fprintf(f, "%s %s@%d", k++ ? "," : "", name.c_str(), loc);
  fprintf(f, "\n");
  if (!getProgramiv || prog <= 0) return;

  GLint nb = 0;
  getProgramiv(static_cast<GLuint>(prog), 0x8A36 /* ACTIVE_UNIFORM_BLOCKS */, &nb);
  for (GLint i = 0; i < nb && blockName && blockiv; ++i) {
    char name[256];
    GLsizei len = 0;
    blockName(static_cast<GLuint>(prog), static_cast<GLuint>(i), sizeof(name), &len, name);
    GLint binding = -1, dataSize = 0;
    blockiv(static_cast<GLuint>(prog), static_cast<GLuint>(i), 0x8A3F /* BINDING */, &binding);
    blockiv(static_cast<GLuint>(prog), static_cast<GLuint>(i), 0x8A40 /* DATA_SIZE */, &dataSize);
    GLint buf = 0;
    long long start = 0, size = 0;
    if (geti && binding >= 0) geti(0x8A28 /* UNIFORM_BUFFER_BINDING */, static_cast<GLuint>(binding), &buf);
    if (geti64 && binding >= 0) {
      geti64(0x8A29 /* UNIFORM_BUFFER_START */, static_cast<GLuint>(binding), &start);
      geti64(0x8A2A /* UNIFORM_BUFFER_SIZE */, static_cast<GLuint>(binding), &size);
    }
    fprintf(f, "    UBO '%s' binding %d, %d bytes -> buffer %d [%lld, +%lld]\n", name, binding, dataSize, buf, start, size);
    DumpBufferRange(f, static_cast<GLuint>(buf), start, size > 0 ? size : dataSize, rels, nrels, Ts);
  }
  GLint ns = 0;
  if (ifaceiv) ifaceiv(static_cast<GLuint>(prog), 0x92E6 /* SHADER_STORAGE_BLOCK */, 0x92F5 /* ACTIVE_RESOURCES */, &ns);
  for (GLint i = 0; i < ns && resName && resiv; ++i) {
    char name[256];
    GLsizei len = 0;
    resName(static_cast<GLuint>(prog), 0x92E6, static_cast<GLuint>(i), sizeof(name), &len, name);
    const GLenum prop = 0x9302;  // BUFFER_BINDING
    GLint binding = -1;
    resiv(static_cast<GLuint>(prog), 0x92E6, static_cast<GLuint>(i), 1, &prop, 1, &len, &binding);
    GLint buf = 0;
    long long start = 0, size = 0;
    if (geti && binding >= 0) geti(0x90D3 /* SHADER_STORAGE_BUFFER_BINDING */, static_cast<GLuint>(binding), &buf);
    if (geti64 && binding >= 0) {
      geti64(0x90D4 /* START */, static_cast<GLuint>(binding), &start);
      geti64(0x90D5 /* SIZE */, static_cast<GLuint>(binding), &size);
    }
    fprintf(f, "    SSBO '%s' binding %d -> buffer %d [%lld, +%lld]\n", name, binding, buf, start, size);
    DumpBufferRange(f, static_cast<GLuint>(buf), start, size, rels, nrels, Ts);
  }
}

// Whole-process memory scan for the camera (finds e.g. a UBO persistently mapped before we were
// injected and written with memcpy). No C++ objects with destructors: SEH guards each region.
struct MemHit { const float* addr; int rel; bool transposed; };
size_t ScanRegion(const float* p, size_t n, const Rel* rels, int nrels, const float (*Ts)[16], MemHit* hits,
                  size_t nHits, size_t maxHits) {
  __try {
    for (size_t i = 0; i + 16 <= n && nHits < maxHits; ++i) {
      for (int k = 0; k < nrels; ++k) {
        const float* m = rels[k].m;
        const float* t = Ts[k];
        const bool pre = std::fabs(p[i] - m[0]) < 1e-3f && std::fabs(p[i + 5] - m[5]) < 1e-3f;
        const bool preT = std::fabs(p[i] - t[0]) < 1e-3f && std::fabs(p[i + 5] - t[5]) < 1e-3f;
        if ((pre && Diff(p + i, m) < 1e-3) || (preT && Diff(p + i, t) < 1e-3)) {
          hits[nHits++] = {p + i, k, !(pre && Diff(p + i, m) < 1e-3)};
          i += 15;
          break;
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return nHits;
}

void MemoryScan(FILE* f, const Rel* rels, int nrels, const float (*Ts)[16]) {
  const double t0 = static_cast<double>(GetTickCount64());
  MemHit hits[64];
  size_t nHits = 0;
  unsigned long long scanned = 0;
  MEMORY_BASIC_INFORMATION mbi;
  for (char* a = nullptr; VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi) && nHits < 64;
       a = static_cast<char*>(mbi.BaseAddress) + mbi.RegionSize) {
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (mbi.State != MEM_COMMIT || !(mbi.Protect & readable) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) continue;
    nHits = ScanRegion(static_cast<const float*>(mbi.BaseAddress), mbi.RegionSize / 4, rels, nrels, Ts, hits, nHits, 64);
    scanned += mbi.RegionSize;
  }
  fprintf(f, "\n--- PROCESS MEMORY SCAN: %.2f GB in %.1f s, %zu hits (tolerance 1e-3; next frame's values) ---\n",
          scanned / 1073741824.0, (GetTickCount64() - t0) / 1000.0, nHits);
  for (size_t i = 0; i < nHits; ++i) {
    VirtualQuery(hits[i].addr, &mbi, sizeof(mbi));
    const char* type = mbi.Type == MEM_IMAGE ? "IMAGE" : mbi.Type == MEM_MAPPED ? "MAPPED" : "PRIVATE";
    char mod[MAX_PATH] = "";
    HMODULE h = nullptr;
    if (mbi.Type == MEM_IMAGE &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(hits[i].addr), &h))
      GetModuleFileNameA(h, mod, MAX_PATH);
    fprintf(f, "  %p = %s%s | alloc %p region %p +%zu KB, %s, protect 0x%lX %s\n", static_cast<const void*>(hits[i].addr),
            rels[hits[i].rel].name, hits[i].transposed ? " TRANSPOSED" : "", mbi.AllocationBase, mbi.BaseAddress,
            mbi.RegionSize / 1024, type, mbi.Protect, mod);
  }
}

// ---------------- value probe: read a texture back to a raw float file ----------------
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_RED 0x1903
using PFN_GetTextureImage = void(APIENTRY*)(GLuint, GLint, GLenum, GLenum, GLsizei, void*);
using PFN_BindBuffer = void(APIENTRY*)(GLenum, GLuint);

// Writes <base>-T<name>-<w>x<h>.f32: raw little-endian floats, GL row order (bottom row first).
void ProbeTexture(const std::wstring& base, const Att& a, bool depth) {
  if (a.type != GL_TEXTURE) return;
  auto getImage = reinterpret_cast<PFN_GetTextureImage>(wglGetProcAddress("glGetTextureImage"));
  auto bindBuffer = reinterpret_cast<PFN_BindBuffer>(wglGetProcAddress("glBindBuffer"));
  if (!ValidProc(reinterpret_cast<void*>(getImage)) || !ValidProc(reinterpret_cast<void*>(bindBuffer)) || !pGetTexLevel) {
    Log("probe: glGetTextureImage unavailable");
    return;
  }
  GLint w = 0, h = 0;
  pGetTexLevel(a.name, 0, GL_TEXTURE_WIDTH, &w);
  pGetTexLevel(a.name, 0, GL_TEXTURE_HEIGHT, &h);
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
  std::vector<float> px(static_cast<size_t>(w) * h);

  GLint packBuf = 0, packAlign = 4;
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuf);
  glGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);
  bindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  while (glGetError() != GL_NO_ERROR) {}
  getImage(a.name, 0, depth ? GL_DEPTH_COMPONENT : GL_RED, GL_FLOAT,
           static_cast<GLsizei>(px.size() * sizeof(float)), px.data());
  GLenum err = glGetError();
  glPixelStorei(GL_PACK_ALIGNMENT, packAlign);
  bindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuf));
  if (err != GL_NO_ERROR) { Log("probe: readback of T%d failed, GL error 0x%x", a.name, err); return; }

  wchar_t path[MAX_PATH];
  swprintf_s(path, L"%s-T%d-%dx%d.f32", base.c_str(), a.name, w, h);
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"wb") || !f) return;
  fwrite(px.data(), sizeof(float), px.size(), f);
  fclose(f);
  Log("probe: wrote %ls", path);
}

void Dump(unsigned frame) {
  FlushSeg();
  wchar_t dir[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t base[MAX_PATH], path[MAX_PATH];
  swprintf_s(base, L"%s\\zwift-vr\\frame-trace-%02d%02d%02d", dir, t.wHour, t.wMinute, t.wSecond);
  swprintf_s(path, L"%s.txt", base);
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"w") || !f) { Log("trace: can't write %ls", path); return; }

  fprintf(f, "zwift-vr frame trace - frame %u, %u draw calls, %zu entries\n", frame, g_totalDraws, g_lines.size());
  fprintf(f, "T=texture R=renderbuffer, then GL name, size, internal format. vp=viewport.\n\n");
  for (auto& l : g_lines) fprintf(f, "%s\n", l.c_str());

  fprintf(f, "\n--- draws per depth attachment (most first) ---\n");
  std::vector<std::pair<unsigned, long long>> byDepth;
  for (auto& [id, n] : g_drawsPerDepth) byDepth.push_back({n, id});
  std::sort(byDepth.rbegin(), byDepth.rend());
  for (auto& [n, id] : byDepth) {
    Att a{static_cast<GLint>(id >> 32), static_cast<GLint>(id & 0xffffffff)};
    fprintf(f, "%6u draws  %s\n", n, Describe(a).c_str());
  }

  // ---- camera recon ----
  fprintf(f, "\n--- matrix uploads: %u uniform calls, %u buffer writes (%llu bytes), %u buffer maps ---\n",
          g_uniformCalls, g_bufferCalls, g_bufferBytes, g_mapCalls);
  fprintf(f, "\nPERSPECTIVE candidates (%zu sources). Column-major after transpose fix.\n", g_projSrc.size());
  fprintf(f, "near/far given for GL [-1,1] depth and for [0,1] depth (glClipControl); measured near is 10.\n");
  for (auto& [key, s] : g_projSrc) {
    const float* m = s.last;
    const double kDeg = 180.0 / 3.14159265358979;
    double hfov = 2 * std::atan(1.0 / m[0]) * kDeg, vfov = 2 * std::atan(1.0 / std::fabs(m[5])) * kDeg;
    double nGL = m[14] / (m[10] - 1.0), fGL = m[14] / (m[10] + 1.0);
    double n01 = m[10] != 0 ? m[14] / m[10] : 0, f01 = (m[10] + 1.0) != 0 ? m[14] / (m[10] + 1.0) : 0;
    fprintf(f, "  %-48s writes %3u changes %3u%s\n    hfov %.2f vfov %.2f aspect %.4f | GL near %.3f far %.1f | [0,1] near "
               "%.3f far %.1f | offcentre %.4f %.4f | w-sign %+.0f\n",
            key.c_str(), s.writes, s.changes, s.transposed ? " (stored row-major)" : "", hfov, vfov,
            std::fabs(m[5]) / m[0], nGL, fGL, n01, f01, m[8], m[9], m[11]);
    fprintf(f, "    m = [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g] (columns)\n", m[0], m[1], m[2], m[3],
            m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
  }

  std::vector<std::pair<std::string, const MatSource*>> constRigid;
  unsigned changingRigid = 0;
  for (auto& [key, s] : g_rigidSrc) {
    if (s.changes == 0) constRigid.push_back({key, &s}); else changingRigid++;
  }
  std::sort(constRigid.begin(), constRigid.end(),
            [](auto& a, auto& b) { return a.second->writes > b.second->writes; });
  fprintf(f, "\nRIGID transforms constant within the frame: %zu sources (+%u that changed). Most-written first,\n",
          constRigid.size(), changingRigid);
  fprintf(f, "with 'cam pos' = position if it were a world->view matrix, 'fwd' = view -Z in world.\n");
  size_t shown = 0;
  for (auto& [key, sp] : constRigid) {
    if (shown++ >= 40) break;
    const float* m = sp->last;
    float px = -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]);
    float py = -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]);
    float pz = -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14]);
    fprintf(f, "  %-48s writes %3u%s  t (%.1f %.1f %.1f)  cam pos (%.1f %.1f %.1f)  fwd (%.3f %.3f %.3f)\n",
            key.c_str(), sp->writes, sp->transposed ? " (row-major)" : "", m[12], m[13], m[14], px, py, pz, -m[2],
            -m[6], -m[10]);
  }

  // ---- relate every mat4 uniform to the camera: which ones must a head-tracking override patch? ----
  // P = most-written perspective source; V = the world-space rigid value shared by the most uniform slots.
  const float* P = nullptr;
  unsigned bestP = 0;
  for (auto& [k, s] : g_projSrc) if (s.writes > bestP) { bestP = s.writes; P = s.last; }
  const float* V = nullptr;
  unsigned bestV = 0;
  for (auto& [k, a] : g_allMat) {
    if (!IsRigid(a.last) || std::fabs(a.last[12]) + std::fabs(a.last[13]) + std::fabs(a.last[14]) < 1000) continue;
    unsigned shared = 0;
    for (auto& [k2, b] : g_allMat) if (memcmp(a.last, b.last, sizeof(a.last)) == 0) shared++;
    if (shared > bestV) { bestV = shared; V = a.last; }
  }
  fprintf(f, "\n--- CAMERA UNIFORM MAP (%zu single-mat4 uniform slots) ---\n", g_allMat.size());
  if (!P || !V) {
    fprintf(f, "  could not identify %s\n", !P ? "P" : "V");
  } else {
    float Vi[16], Pi[16], PV[16], PVi[16];
    Inverse(V, Vi);
    Inverse(P, Pi);
    Mul(P, V, PV);
    Inverse(PV, PVi);
    fprintf(f, "  V shared by %u slots: cam pos (%.1f %.1f %.1f), fwd (%.3f %.3f %.3f)\n", bestV, Vi[12], Vi[13],
            Vi[14], -Vi[8], -Vi[9], -Vi[10]);
    const Rel rels[] = {{"V", V}, {"V^-1", Vi}, {"P", P}, {"P^-1", Pi}, {"P*V", PV}, {"(P*V)^-1", PVi}};
    // name -> label -> slots
    std::map<std::string, std::map<std::string, unsigned>> byName;
    std::map<GLint, std::map<GLint, std::string>> namesCache;
    for (auto& [pl, a] : g_allMat) {
      auto& names = namesCache.try_emplace(pl.first, UniformNames(pl.first)).first->second;
      auto it = names.find(pl.second);
      std::string name = it != names.end() ? it->second : "?loc" + std::to_string(pl.second);
      std::string label;
      for (const Rel& r : rels) if (Diff(a.last, r.m) < 1e-3) { label = r.name; break; }
      if (label.empty())
        for (const Rel& r : rels) if (Diff(a.last, r.m) < 5e-2) { label = std::string("~") + r.name + " (prev frame?)"; break; }
      if (label.empty()) {
        if (IsPerspective(a.last)) label = "other perspective";
        else if (IsRigid(a.last) && std::fabs(a.last[12]) + std::fabs(a.last[13]) + std::fabs(a.last[14]) > 1000)
          label = "other world rigid (reflection / other camera?)";
        else if (IsRigid(a.last)) label = "local rigid (model?)";
        else label = "unrelated";
      }
      byName[name][label]++;
    }
    for (auto& [name, labels] : byName) {
      fprintf(f, "  %-36s", name.c_str());
      for (auto& [label, n] : labels) fprintf(f, "  %s x%u", label.c_str(), n);
      fprintf(f, "\n");
    }

    // ---- mapped buffers: does the main geometry's camera live in one? ----
    fprintf(f, "\n--- MAPPED BUFFERS (%zu mappings this frame) ---\n", g_maps.size());
    for (MapRec& r : g_maps)
      if (!r.unmapped && (r.access & GL_MAP_PERSISTENT_BIT)) Snapshot(r);  // still mapped: read now
    float Ts[6][16];
    for (int i = 0; i < 6; ++i) Transpose(rels[i].m, Ts[i]);
    for (const MapRec& r : g_maps) {
      fprintf(f, "  buf %d target 0x%X offset %lld length %lld access 0x%X%s%s%s snapshot %zu floats\n", r.buffer,
              r.target, static_cast<long long>(r.offset), static_cast<long long>(r.length), r.access,
              (r.access & GL_MAP_PERSISTENT_BIT) ? " PERSISTENT" : "", (r.access & GL_MAP_COHERENT_BIT) ? " COHERENT" : "",
              r.unmapped ? " (unmapped in frame)" : " (still mapped)", r.snap.size());
      unsigned matches = 0, persp = 0, rigidBig = 0;
      for (size_t i = 0; i + 16 <= r.snap.size(); i += 4) {
        const float* w = r.snap.data() + i;
        if (IsPerspective(w)) persp++;
        if (IsRigid(w) && TranslationL1(w) > 1000) rigidBig++;
        for (int k = 0; k < 6; ++k) {
          const char* how = nullptr;
          if (Diff(w, rels[k].m) < 2e-2) how = "";
          else if (Diff(w, Ts[k]) < 2e-2) how = " (transposed)";
          if (how && matches < 40) {
            fprintf(f, "    +%-8zu = %s%s  (err %.1e)\n", static_cast<size_t>(r.offset) + i * 4, rels[k].name, how,
                    std::min(Diff(w, rels[k].m), Diff(w, Ts[k])));
            matches++;
            break;
          }
        }
      }
      if (persp || rigidBig) fprintf(f, "    windows: %u perspective-shaped, %u world-rigid-shaped\n", persp, rigidBig);
    }

    // ---- which programs draw the most, and where do THEY get the camera? ----
    std::vector<std::pair<unsigned, GLint>> progs;
    for (auto& [p, n] : g_progDraws) progs.push_back({n, p});
    std::sort(progs.rbegin(), progs.rend());
    fprintf(f, "\n--- TOP PROGRAMS BY DRAWS (%zu programs drew this frame) ---\n", progs.size());
    for (size_t i = 0; i < progs.size() && i < 8; ++i) ProgramReport(f, progs[i].second, progs[i].first, rels, 6, Ts);

    {
      std::lock_guard lock(g_otherMx);
      fprintf(f, "\n--- BUFFER WRITES FROM OTHER THREADS: %zu ---\n", g_otherWrites.size());
      unsigned shownW = 0;
      for (const OtherWrite& w : g_otherWrites) {
        if (shownW++ < 20)
          fprintf(f, "  thread %lu %s id %u offset %lld size %lld\n", w.tid, w.what.c_str(), w.id,
                  static_cast<long long>(w.offset), static_cast<long long>(w.size));
        ScanFloatsForCamera(f, "      ", static_cast<size_t>(w.offset), w.data.data(), w.data.size(), rels, 6, Ts, 6);
      }
    }

    MemoryScan(f, rels, 6, Ts);
  }
  fclose(f);
  Log("trace: frame %u written to %ls (%u draws, %zu entries)", frame, path, g_totalDraws, g_lines.size());

  // Read back the main pass's depth + any R32F colour targets (candidate linear depth).
  // This runs one swap later than the traced frame, so it shows the *next* frame - fine for value ranges.
  if (g_topKey.fbo > 0) {
    ProbeTexture(base, g_topKey.depth, true);
    for (const Att& c : g_topKey.color) {
      if (c.type != GL_TEXTURE || !pGetTexLevel) continue;
      GLint fmt = 0;
      pGetTexLevel(c.name, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
      if (fmt == 0x822E /* R32F */) ProbeTexture(base, c, false);
    }
  }
}

}  // namespace

// After a traced frame: the R32F colour target of the busiest pass = linear scene depth.
void UpdateSceneDepth() {
  GLuint found = 0;
  if (g_topKey.fbo > 0 && pGetTexLevel) {
    for (const Att& c : g_topKey.color) {
      if (c.type != GL_TEXTURE) continue;
      GLint fmt = 0;
      pGetTexLevel(c.name, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
      if (fmt == 0x822E /* R32F */) { found = static_cast<GLuint>(c.name); break; }
    }
  }
  if (found != g_sceneLinearDepth)
    Log("scene probe: linear depth = %s (busiest pass FBO %d, %u draws)",
        found ? ("T" + std::to_string(found)).c_str() : "none", g_topKey.fbo, g_topDraws);
  g_sceneLinearDepth = found;
}

void GlTraceOnSwap() {
  if (!wglGetCurrentContext()) return;
  g_frameNo++;
  if (!g_hooksTried) {
    g_hooksTried = true;
    InstallHooks();
  }

  if (g_tracing) {  // the traced frame just ended
    g_tracing = false;
    FlushSeg();
    UpdateSceneDepth();
    if (!g_silent) Dump(g_frameNo - 1);
    g_lines.clear();
    g_descCache.clear();
    g_drawsPerDepth.clear();
    g_totalDraws = 0;
    g_segOpen = false;
    g_topKey = Key{};
    g_topDraws = 0;
    g_projSrc.clear();
    g_rigidSrc.clear();
    g_allMat.clear();
    g_maps.clear();
    g_progDraws.clear();
    {
      std::lock_guard lock(g_otherMx);
      g_otherWrites.clear();
    }
    g_uniformCalls = g_bufferCalls = g_mapCalls = 0;
    g_bufferBytes = 0;
  }

  const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
  const bool down = mods && (GetAsyncKeyState(VK_F12) & 0x8000);
  if (down && !g_keyWasDown && pGetFbAttach) g_armNext = true;
  g_keyWasDown = down;
  static bool f5WasDown = false;  // Ctrl+Shift+F5: shadows on/off live
  const bool f5 = mods && (GetAsyncKeyState(VK_F5) & 0x8000);
  if (f5 && !f5WasDown) {
    g_skipShadows = !g_skipShadows;
    g_drawFboIsShadow = false;  // re-evaluated at the next bind
    Log("Ctrl+Shift+F5: shadows %s", g_skipShadows ? "OFF (shadow-map draws skipped)" : "ON");
  }
  f5WasDown = f5;
}

void GlTraceArm() {
  if (!g_armNext && !g_silentNext) return;
  g_silent = !g_armNext;  // a hotkey trace wins over a silent probe
  g_armNext = g_silentNext = false;
  if (!g_silent) Log("trace: recording frame %u", g_frameNo + 1);
  g_tracing = true;
  g_traceThread = GetCurrentThreadId();
}

void GlTraceRequestSceneProbe() {
  if (pGetFbAttach && !g_tracing) g_silentNext = true;
}

bool GlTraceProbeBusy() { return g_tracing || g_armNext || g_silentNext; }

GLuint GlTraceSceneLinearDepth() { return g_sceneLinearDepth; }

void GlTraceSetShadows(bool on) { g_skipShadows = !on; }

void GlTraceHudBlend(bool on) {
  if (on == g_hudBlend || !o_glBlendFuncSeparate) return;
  if (on) {  // Zwift may have set its blend before we hooked, or through calls we don't see: read it back
    GLint v[4] = {};
    glGetIntegerv(0x80C9 /*GL_BLEND_SRC_RGB*/, &v[0]);
    glGetIntegerv(0x80C8 /*GL_BLEND_DST_RGB*/, &v[1]);
    glGetIntegerv(0x80CB /*GL_BLEND_SRC_ALPHA*/, &v[2]);
    glGetIntegerv(0x80CA /*GL_BLEND_DST_ALPHA*/, &v[3]);
    for (int i = 0; i < 4; ++i) g_blendReq[i] = static_cast<GLenum>(v[i]);
  }
  g_hudBlend = on;
  ApplyBlend();  // on: "over" alpha if the window is bound; off: exactly Zwift's last request
}

void GlTraceSetLegacyRays(bool legacy) {
  g_rayLegacy = legacy;
  if (g_rayProg && g_rayLegacyLoc >= 0) {
    static auto set1ui = reinterpret_cast<PFN_ProgramUniform1ui>(wglGetProcAddress("glProgramUniform1ui"));
    if (set1ui) set1ui(g_rayProg, g_rayLegacyLoc, legacy ? 1u : 0u);
  }
}

unsigned GlTraceTakeShadowDrawsSkipped(unsigned* shadowFbos) {
  if (shadowFbos) *shadowFbos = static_cast<unsigned>(g_shadowSet.size());
  const unsigned n = g_shadowDrawsSkipped;
  g_shadowDrawsSkipped = 0;
  return n;
}

void GlTraceTakeRenderStats(unsigned* reflDrawsSkipped, unsigned* resolveDraws, bool* raysRewritten) {
  *reflDrawsSkipped = g_reflDrawsSkipped;
  *resolveDraws = g_rayDraws;
  *raysRewritten = g_rayProg != 0;
  g_reflDrawsSkipped = g_rayDraws = 0;
}

#pragma once
// 4x4 matrix helpers (GL column-major: element (row r, col c) at m[c*4 + r]) and GL uniform-name
// lookup, shared by the tracer (recon) and the camera override.
#include <windows.h>

#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace glu {

inline bool Zero(float v) { return std::fabs(v) < 1e-5f; }

// Perspective, GL convention: [a 0 0 0 | 0 c 0 0 | x y e -1 | 0 0 f 0].
inline bool IsPerspective(const float* m) {
  return std::fabs(std::fabs(m[11]) - 1.0f) < 1e-4f && Zero(m[15]) && m[0] > 0.01f && std::fabs(m[5]) > 0.01f &&
         Zero(m[1]) && Zero(m[2]) && Zero(m[3]) && Zero(m[4]) && Zero(m[6]) && Zero(m[7]) && Zero(m[12]) &&
         Zero(m[13]) && !Zero(m[14]);
}

// Orthonormal 3x3 + translation, bottom row 0 0 0 1 (identity excluded).
inline bool IsRigid(const float* m) {
  if (!Zero(m[3]) || !Zero(m[7]) || !Zero(m[11]) || std::fabs(m[15] - 1.0f) > 1e-4f) return false;
  for (int c = 0; c < 3; ++c) {
    float len = m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2];
    if (std::fabs(len - 1.0f) > 1e-3f) return false;
  }
  float d01 = m[0] * m[4] + m[1] * m[5] + m[2] * m[6];
  float d02 = m[0] * m[8] + m[1] * m[9] + m[2] * m[10];
  float d12 = m[4] * m[8] + m[5] * m[9] + m[6] * m[10];
  bool identity = std::fabs(m[0] - 1) < 1e-5f && std::fabs(m[5] - 1) < 1e-5f && std::fabs(m[10] - 1) < 1e-5f;
  return std::fabs(d01) < 1e-3f && std::fabs(d02) < 1e-3f && std::fabs(d12) < 1e-3f && !identity;
}

// Bottom row 0 0 0 1: affine (model, view, orthographic UI) - i.e. NOT a perspective clip transform.
inline bool IsAffine(const float* m) {
  return Zero(m[3]) && Zero(m[7]) && Zero(m[11]) && std::fabs(m[15] - 1.0f) < 1e-4f;
}

inline float TranslationL1(const float* m) { return std::fabs(m[12]) + std::fabs(m[13]) + std::fabs(m[14]); }

inline void Transpose(const float* in, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[c * 4 + r] = in[r * 4 + c];
}

inline void Mul(const float* a, const float* b, float* out) {  // out = a * b (out may not alias)
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += static_cast<double>(a[k * 4 + r]) * b[c * 4 + k];
      out[c * 4 + r] = static_cast<float>(s);
    }
}

inline bool Inverse(const float* m, float* out) {  // Gauss-Jordan in double
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) { a[r][c] = m[c * 4 + r]; a[r][c + 4] = r == c; }
  for (int col = 0; col < 4; ++col) {
    int piv = col;
    for (int r = col + 1; r < 4; ++r) if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
    if (std::fabs(a[piv][col]) < 1e-12) return false;
    if (piv != col) for (int c = 0; c < 8; ++c) std::swap(a[piv][c], a[col][c]);
    double d = a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      double f = a[r][col];
      for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[c * 4 + r] = static_cast<float>(a[r][c + 4]);
  return true;
}

// Relative max-abs difference of a vs b.
inline double Diff(const float* a, const float* b) {
  double mx = 1e-6, d = 0;
  for (int i = 0; i < 16; ++i) {
    mx = std::max(mx, static_cast<double>(std::fabs(b[i])));
    d = std::max(d, static_cast<double>(std::fabs(a[i] - b[i])));
  }
  return d / mx;
}

using PFN_GetProgramiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_GetActiveUniform = void(APIENTRY*)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, char*);
using PFN_GetUniformLocation = GLint(APIENTRY*)(GLuint, const char*);

// location -> name for one linked program. Needs a current GL context.
inline std::map<GLint, std::string> UniformNames(GLint prog) {
  std::map<GLint, std::string> out;
  static auto getProgramiv = reinterpret_cast<PFN_GetProgramiv>(wglGetProcAddress("glGetProgramiv"));
  static auto getActive = reinterpret_cast<PFN_GetActiveUniform>(wglGetProcAddress("glGetActiveUniform"));
  static auto getLoc = reinterpret_cast<PFN_GetUniformLocation>(wglGetProcAddress("glGetUniformLocation"));
  if (!getProgramiv || !getActive || !getLoc || prog <= 0) return out;
  GLint n = 0;
  getProgramiv(static_cast<GLuint>(prog), 0x8B86 /* ACTIVE_UNIFORMS */, &n);
  for (GLint i = 0; i < n && i < 1024; ++i) {
    char name[256];
    GLsizei len = 0;
    GLint size = 0;
    GLenum type = 0;
    getActive(static_cast<GLuint>(prog), static_cast<GLuint>(i), sizeof(name), &len, &size, &type, name);
    if (len <= 0) continue;
    GLint loc = getLoc(static_cast<GLuint>(prog), name);
    if (loc >= 0) out[loc] = name;
  }
  return out;
}

}  // namespace glu

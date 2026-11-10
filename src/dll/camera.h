#pragma once
// Phase 3: drive Zwift's camera from the headset (immersive mode).
//
// Zwift feeds its camera to shaders as plain per-program uniforms (phase-3 recon):
//   g_W2V  world->view V        g_PROJ  projection P (infinite far, near 10)
//   g_WVP  P*V*Model (CPU-side)  g_L2W   model->world (= V^-1 for camera-attached objects)
//   reflectedViewMatrix  water-reflection camera
// Each frame we pick one eye (alternate-eye rendering), take its pose E (LOCAL space, metres ->
// Zwift cm) and FOV, and patch uploads in flight:
//   V  -> H*V            with H = E^-1 (head/eye transform in Zwift's view space)
//   P  -> P'             the eye's off-axis projection, same near/infinite-far form as Zwift's
//   WVP-> P'*H*P^-1*WVP  (= P'*(H*V)*Model, without knowing Model); orthographic UI left alone
//   L2W ~ V^-1 -> L2W*H^-1, reflection view -> H*M
// The captured frame is tagged with that eye's pose/FOV so the XR thread can submit a proper
// projection layer (the runtime then reprojects for head motion since render time).
#include <windows.h>

#include <GL/gl.h>
#include <openxr/openxr.h>

struct CamOptions {
  bool startImmersive = true;
  float worldScale = 1.0f;
  float parallax = 1.0f;          // eye-separation scale (Ctrl+Shift+PageUp/PageDown +-0.25)
  bool recentrePosition = true;   // eye origin = head position captured at start / F3 / runtime recentre
  int coherentMode = 0;           // COHERENT (both eyes from one frame): 0 never, 1 while Zwift's camera moves,
                                  // 2 always (ini only, no hotkey)
  bool autoScreenInMenus = true;  // virtual screen in Zwift's menus, immersive while riding (zwift_state.h)
};
void CamConfigure(const CamOptions& o);

// The eye origin in LOCAL space, metres: your head position at the last re-home (start, Ctrl+Shift+F1, runtime
// recentre) with recentre_position on, else the runtime's origin. HUD cards are placed relative to it.
void CamHome(float out[3]);

struct FrameInfo;
// Facts about the frame being captured (call after CamCurrentEye, before CamOnSwap). False when not overriding.
bool CamCurrentFrameInfo(FrameInfo* out);

// Water reflections ([render] reflections, Ctrl+Shift+F3). When off, every draw while Zwift's
// water-reflection camera slice is bound is skipped (a whole second scene render saved). The draw hooks
// read this flag; CamOnBindRange sets it per B_Scene bind.
void CamSetReflections(bool on);
extern bool g_camSkipReflDraws;
// Fog colour of the frame's reflection slice: used for the reflection map's colour clear while its pass
// is skipped, so water blends toward haze instead of black.
extern float g_camReflClear[4];
extern bool g_camHaveReflClear;

// Turns the override off for good (no head tracking, no draw skips). Plain stores only, so it is safe
// to call from the swap hook's exception handler.
void CamDisable();

// Zwift's window aspect (w/h). Its main camera projection has exactly this aspect; projections
// with any other aspect belong to other cameras (previews, probes) and are never touched.
void CamSetWindowAspect(float aspect);

// Render thread, from the SwapBuffers hook AFTER the frame was captured: polls the
// Ctrl+Shift+F10 toggle and latches the eye pose/FOV for the NEXT Zwift frame.
void CamOnSwap();

// Eye the current Zwift frame is being rendered for (-1 = no override this frame, 0/1 = that eye,
// 2 = COHERENT: one image for both eyes), with its pose and the frustum actually RENDERED (symmetric in
// STEREO/COHERENT, Zwift's own if its scale field wasn't found) - not the runtime's asymmetric eye FOV.
int CamCurrentEye(XrPosef* pose, XrFovf* fov);
inline bool CamActive() { return CamCurrentEye(nullptr, nullptr) >= 0; }

// From the glUniformMatrix4fv / glProgramUniformMatrix4fv hooks (single matrix). Returns true and
// writes the replacement (column-major, upload with transpose = GL_FALSE) into `out` when the
// uniform is one of Zwift's camera matrices.
bool CamOverrideUniform(GLint prog, GLint loc, const GLfloat* v, bool transpose, float* out);

// From the glBindBufferRange / glBindBuffersRange hooks, BEFORE the original call. Zwift's world
// shaders don't use the uniforms above: they read the camera from a persistently mapped,
// write-combined ring buffer that Zwift fills with memcpy (invisible to GL hooks), then binds:
//   SSBO binding 5 'B_Scene'  : View { mat4 m @+32; vec4 viewPos; vec4 dir; vec4 clip }
//                               Projection { vec4 scale; vec4 bias; mat4 m @+176; mat4 inv @+240 }
//   UBO  binding 1 'B_Object' : model M @+0, WVP = P*V*M @+64                (per draw)
// The world is CAMERA-RELATIVE: Transform = mat3(view.m) * (p - viewPos), Project = p.xyzz*scale + bias
// (diagonal only). So the eye position goes into viewPos, the head rotation into mat3(view.m), and the
// frustum into scale - which forces a SYMMETRIC per-eye frustum; CamCurrentEye reports that rendered
// frustum so the XR thread submits exactly what was drawn. viewPos/dir/scale offsets are found by
// value once per session. At bind time Zwift has finished writing the slice and the GPU hasn't read
// it, so the camera is rewritten in place in the mapped memory (once per slice per frame).
void CamOnBindRange(GLenum target, GLuint index, GLuint buffer, ptrdiff_t offset, ptrdiff_t size);

#pragma once
#include <windows.h>

// Phase-2 recon: press Ctrl+Shift+F12 in Zwift to record one frame of its GL rendering
// (render targets + their attachments/sizes/formats, draw counts, clears, blits, copies)
// to %LOCALAPPDATA%\zwift-vr\frame-trace-HHMMSS.txt. Used to find the scene depth buffer.
//
// Call from the SwapBuffers hook, on Zwift's render thread, BEFORE our own capture blit
// (so our GL calls are never part of a trace).
void GlTraceOnSwap();

// Call from the SwapBuffers hook AFTER our capture blit: starts a trace the hotkey armed,
// so the traced frame contains only Zwift's own GL calls.
void GlTraceArm();

// Silent one-frame probe (no files): finds the R32F colour target of Zwift's busiest render
// pass, which phase-2 recon showed is linear view depth in cm (T4). Result after the next frame.
void GlTraceRequestSceneProbe();
bool GlTraceProbeBusy();
unsigned GlTraceSceneLinearDepth();  // GL texture name, 0 = not found (yet)

// [render] shadows = 0: skip every draw into shadow-map framebuffers (sun cascades + headlight
// spotlight), so shadow maps stay cleared = no shadows, ~3100 fewer driver draw calls per frame.
void GlTraceSetShadows(bool on);
// Draws skipped since the last call (render thread); *shadowFbos = shadow FBOs currently identified.
unsigned GlTraceTakeShadowDrawsSkipped(unsigned* shadowFbos);

// [hud] window_alpha: true from the world snapshot until the swap - every blend's ALPHA factors become "over"
// (ONE, ONE_MINUS_SRC_ALPHA) so the window's alpha is the HUD's true coverage; colour factors stay Zwift's.
// false puts back exactly the blend Zwift last asked for. Render thread, Zwift's context current.
void GlTraceHudBlend(bool on);

// Deferred-resolve ray rewrite: legacy = use Zwift's own CPU rays again (Ctrl+Shift+F4 A/B).
void GlTraceSetLegacyRays(bool legacy);
// Since the last call: draws skipped in the water-reflection pass, resolve draws through the rewritten
// program, and whether the rewrite is active at all.
void GlTraceTakeRenderStats(unsigned* reflDrawsSkipped, unsigned* resolveDraws, bool* raysRewritten);

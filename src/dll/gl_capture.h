#pragma once
#include <windows.h>

// Called from the SwapBuffers hooks on Zwift's render thread, with Zwift's GL context
// current, right before the frame is presented. Copies the finished backbuffer into the
// shared D3D11 texture. Never throws; does nothing until the XR thread has a device.
void GlCaptureFrame(HDC hdc);

// Set once at startup, before the hooks are enabled.
//   cropAspect: crop each frame to this width/height ratio around the centre; 0 = whole window.
//   wantDepth:  also copy Zwift's linear scene depth (found by the silent scene probe) each frame.
void GlCaptureConfigure(float cropAspect, bool wantDepth);

// [hud]: split Zwift's HUD off the eye images in immersive frames. Zwift draws its finished scene into the
// window (FBO 0) with `compositeDraws` draws, then its HUD on top (~210 draws). Just before draw number
// compositeDraws+1 the window is snapshotted ("world"); at swap the eyes get that snapshot and the XR thread
// gets the finished frame too, to cut the HUD out of. Set once at startup.
// useAlpha: after the snapshot, zero the window's alpha so the HUD leaves its coverage there (clean panels).
void GlCaptureConfigureHud(bool on, int compositeDraws, bool useAlpha);

// From the GL draw hooks, BEFORE the draw, when the bound draw framebuffer is the window (FBO 0).
void GlCaptureOnWindowDraw();

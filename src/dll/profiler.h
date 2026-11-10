#pragma once
// In-process sampling profiler for Zwift's render thread (diagnosing the immersive-mode frame-rate
// drop: one Zwift thread pegged at ~96% of a core while the GPU idles at ~15%).
//
// Ctrl+Shift+F8 (polled from the SwapBuffers hook): a sampler thread suspends the render thread
// ~1000x/s for 4 s, records its instruction pointer plus a short unwound call stack, and logs
//   - % of samples by module of the current instruction (ZwiftApp / nvoglv64 / opengl32 / ours / OS)
//   - % by "who's responsible": first stack frame in ZwiftApp.exe or zwift_vr.dll
//   - top RVAs inside zwift_vr.dll (resolve with build\zwift_vr.map)
void ProfilerOnSwap();  // render thread, every SwapBuffers

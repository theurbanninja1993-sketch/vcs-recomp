// The main camera, published for anything that needs to know where the camera
// was this frame. Written by the game override hooks (see vcsPublishCamera) and
// read by whoever wants it; the renderer no longer does, now that the motion
// blur that used it has been taken back out, so it lives on its own rather than
// inside the GPU renderer.
//
// It is kept because it is the verified part: the offsets were confirmed against
// a real camera with PS2X_CAMERA_LOG=1, and ps2CameraHomography in the header is
// checked numerically against hand-computed cases. See docs/vcs-handoff.md item
// 13 for what was tried and why it was withdrawn.
#include "runtime/ps2_camera.h"

Ps2CameraState g_ps2Camera;
std::atomic<uint64_t> g_ps2CameraFrame{0};

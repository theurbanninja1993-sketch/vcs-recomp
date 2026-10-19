#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

// The main camera, published once per emulated frame for the reprojection pass
// (a modern motion blur, and later frame interpolation above 60 fps).
//
// Written by the game_overrides camera hooks, which is the only place the port
// knows the camera: orientation at +0x78/+0x7C, position at +0x20, and the view
// window (tan(halfFov) wide) from RwCameraSetViewWindow. Read by the renderer at
// present time, which runs on another thread, so a reader may see a value one
// frame old; for a blur that is invisible and not worth a lock.
struct Ps2CameraState
{
    // The camera's orientation basis and position, from TheCamera.Cams[0]
    // (a fixed address, 0x6F4540) - the same vectors the port rotates for
    // vehicle mouse look, so they are the ones it can vouch for. The first
    // attempt read +0x78/+0x7C off the RenderWare camera, which turned out to
    // be a change detector (values like 2000) rather than alpha/beta, and a
    // +0x20 that is not a world position; the camera log caught both.
    float front[3] = {}; // look direction, unit length
    float up[3] = {};    // up vector, unit length
    float pos[3] = {};   // world position (CCam Source)
    float viewX = 0.0f, viewY = 0.0f; // tan(halfFov) wide, aspect-scaled tall
    bool valid = false;
};

extern Ps2CameraState g_ps2Camera;
// The presented frame the state was captured on, so the renderer can tell a
// fresh camera from one that has not moved since.
extern std::atomic<uint64_t> g_ps2CameraFrame;

// A 3x3 matrix in GL's column-major order (so it can go straight to mat3),
// mapping a screen coordinate in the current frame to the screen coordinate the
// same content had in the previous frame.
struct Ps2CameraHomography
{
    // Column-major: m[0..2] is the first column.
    float m[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    bool usable = false; // false = leave the frame alone (identity)
};

// Screen coordinate convention. u and v are normalized [0,1] with u to the
// right. v points *down* the picture: the draw shader maps GS y straight to GL
// clip y with no flip, so the target texture's row 0 is GS y 0, which is the top
// of the picture. That is the opposite of the camera's y-up basis, so the
// intrinsics below carry the sign.
//
//   ray (camera frame, z = depth along the view axis):
//     x = (u - 0.5) * 2 * viewX
//     y = (0.5 - v) * 2 * viewY
//
// depth is the assumed distance along the view axis of everything on screen,
// in the same units as pos. The port cannot read real depth: the GS receives
// vertices the game already projected, so the Z values are the game's own
// projection and cannot be inverted without its matrices. Translation is
// therefore reprojected against a single depth plane. A wrong depth scales the
// translation part of the warp - it does not corrupt it - and rotation, which is
// exact, is unaffected.
inline Ps2CameraHomography ps2CameraHomography(const Ps2CameraState &prev, const Ps2CameraState &cur, float depth)
{
    Ps2CameraHomography out;
    if (!prev.valid || !cur.valid || !(depth > 0.0f) || !(cur.viewX > 0.0f) || !(cur.viewY > 0.0f) ||
        !(prev.viewX > 0.0f) || !(prev.viewY > 0.0f))
        return out;

    // Camera basis as rows (right, up, forward), so camera coords are
    // R * (world - pos) and the third row is the look direction. right is
    // front x up; up is rebuilt from right x front so the basis is exactly
    // orthogonal even though the published vectors are only near-perpendicular.
    const auto basis = [](const Ps2CameraState &c, float R[9]) -> bool
    {
        const float f[3] = {c.front[0], c.front[1], c.front[2]};
        const float u[3] = {c.up[0], c.up[1], c.up[2]};
        float r[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
        const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (!(rl > 1e-4f) || !(fl > 1e-4f))
            return false; // degenerate (up parallel to front): the unbuilt camera
        for (int i = 0; i < 3; ++i)
        {
            r[i] /= rl;
            R[6 + i] = f[i] / fl;
        }
        // up = right x front
        R[3] = r[1] * R[8] - r[2] * R[7];
        R[4] = r[2] * R[6] - r[0] * R[8];
        R[5] = r[0] * R[7] - r[1] * R[6];
        R[0] = r[0];
        R[1] = r[1];
        R[2] = r[2];
        return true;
    };

    float Rp[9], Rc[9];
    if (!basis(prev, Rp) || !basis(cur, Rc))
        return out;

    // M = Rp * Rc^T maps a current-camera vector into the previous camera frame.
    // delta = Rp * (cur.pos - prev.pos) is the camera translation in that frame.
    float M[9], delta[3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            M[i * 3 + j] = Rp[i * 3] * Rc[j * 3] + Rp[i * 3 + 1] * Rc[j * 3 + 1] + Rp[i * 3 + 2] * Rc[j * 3 + 2];
    const float d[3] = {cur.pos[0] - prev.pos[0], cur.pos[1] - prev.pos[1], cur.pos[2] - prev.pos[2]};
    for (int i = 0; i < 3; ++i)
        delta[i] = Rp[i * 3] * d[0] + Rp[i * 3 + 1] * d[1] + Rp[i * 3 + 2] * d[2];

    // A current-camera ray r (z = 1) lands on the depth plane at depth * r, so in
    // the previous camera it is  depth * M * r + delta. The delta term is a
    // constant per frame, which as a matrix is delta * e3^T (it picks r.z = 1).
    float A[9];
    for (int i = 0; i < 9; ++i)
        A[i] = depth * M[i];
    A[2] += delta[0];
    A[5] += delta[1];
    A[8] += delta[2];

    // K maps screen (u, v, 1) to a camera ray; Kp^-1 projects a camera point
    // back to screen. The y signs are the picture's v-down convention.
    const float kc[9] = {2.0f * cur.viewX, 0.0f, -cur.viewX, 0.0f, -2.0f * cur.viewY, cur.viewY, 0.0f, 0.0f, 1.0f};
    const float kpi[9] = {1.0f / (2.0f * prev.viewX), 0.0f, 0.5f, 0.0f, -1.0f / (2.0f * prev.viewY), 0.5f, 0.0f, 0.0f, 1.0f};

    // H = Kp^-1 * A * Kc
    float AK[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            AK[i * 3 + j] = A[i * 3] * kc[j] + A[i * 3 + 1] * kc[3 + j] + A[i * 3 + 2] * kc[6 + j];
    float H[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            H[i * 3 + j] = kpi[i * 3] * AK[j] + kpi[i * 3 + 1] * AK[3 + j] + kpi[i * 3 + 2] * AK[6 + j];

    // GL wants column-major.
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out.m[j * 3 + i] = H[i * 3 + j];
    out.usable = true;
    return out;
}

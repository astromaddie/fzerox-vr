/* Standalone unit-test harness for port/vr/gdx_vr_math.cpp.
 *
 * The decisive check rebuilds the game's own combined camera matrix with the formulas of
 * Matrix_SetLookAt / Matrix_SetFrustrum (decomp/src/sys/math.c) and
 * Camera_CalculateProjectionViewMtx (decomp/src/game/camera.c), copied below as a reference, and
 * requires the VR path to produce the same matrix for an untracked eye given the equivalent field
 * of view. That rules out transposed matrices, wrong handedness, flipped axes, reversed depth and
 * multiplication-order mistakes before anything reaches a headset. The reference uses exact tan()
 * where the game uses its 4096-entry table; that difference is far below the tolerance.
 *
 * No OpenXR, no window, no game data.
 */
#include <cmath>
#include <cstdio>

#include "../vr/gdx_vr_math.h"

using namespace gdx_vr;

static int gChecks = 0;
static int gFails = 0;

static void check(const char* what, bool ok) {
    gChecks++;
    if (!ok) {
        gFails++;
        std::printf("    [x] %s\n", what);
    }
}

static void checkNear(const char* what, double got, double expected, double tolerance) {
    gChecks++;
    if (!(std::fabs(got - expected) <= tolerance)) {
        gFails++;
        std::printf("    [x] %s: got %.6f, expected %.6f (+-%.1e)\n", what, got, expected, tolerance);
    }
}

static const float kPi = 3.14159265358979f;

// ---- Reference: the game's matrix construction, transcribed -----------------------------------

static void RefLookAt(float m[4][4], Vec3 eye, Vec3 at, Vec3 up) {
    float lx = eye.x - at.x, ly = eye.y - at.y, lz = eye.z - at.z;
    float f = 1.0f / std::sqrt(lx * lx + ly * ly + lz * lz);
    m[0][2] = lx * f;
    m[1][2] = ly * f;
    m[2][2] = lz * f;
    m[3][2] = -eye.x * m[0][2] - eye.y * m[1][2] - eye.z * m[2][2];
    float nx = up.y * lz - up.z * ly, ny = up.z * lx - up.x * lz, nz = up.x * ly - up.y * lx;
    f = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
    m[0][0] = nx * f;
    m[1][0] = ny * f;
    m[2][0] = nz * f;
    m[3][0] = -eye.x * m[0][0] - eye.y * m[1][0] - eye.z * m[2][0];
    float ux = ly * nz - lz * ny, uy = lz * nx - lx * nz, uz = lx * ny - ly * nx;
    f = 1.0f / std::sqrt(ux * ux + uy * uy + uz * uz);
    m[0][1] = ux * f;
    m[1][1] = uy * f;
    m[2][1] = uz * f;
    m[3][1] = -eye.x * m[0][1] - eye.y * m[1][1] - eye.z * m[2][1];
    m[0][3] = m[1][3] = m[2][3] = 0.0f;
    m[3][3] = 1.0f;
}

// Matrix_SetFrustrum: fovAngle in degrees spans the horizontal extent; vertical is scaled by
// fovScaleY / fovScaleX; frustum centres shift the window.
static void RefFrustrum(float m[4][4], float fovAngle, float nearZ, float farZ, float fovScaleX, float centerX,
                        float fovScaleY, float centerY) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            m[i][j] = 0.0f;
        }
    }
    float radius = std::tan(fovAngle * kPi / 360.0f) * nearZ;
    centerX *= (2.0f * radius) / fovScaleX;
    float right = centerX + radius, left = centerX - radius;
    radius *= fovScaleY / fovScaleX;
    centerY *= (2.0f * radius) / fovScaleY;
    float top = centerY + radius, bottom = centerY - radius;
    m[0][0] = 2.0f * nearZ * (1.0f / (right - left));
    m[2][0] = (right + left) * (1.0f / (right - left));
    m[1][1] = 2.0f * nearZ * (1.0f / (top - bottom));
    m[2][1] = (top + bottom) * (1.0f / (top - bottom));
    m[3][2] = 2.0f * nearZ * farZ * (1.0f / (nearZ - farZ));
    m[2][2] = (nearZ + farZ) * (1.0f / (nearZ - farZ));
}

// Camera_CalculateProjectionViewMtx, verbatim structure.
static void RefProjView(float pv[4][4], float p[4][4], float v[4][4]) {
    for (int i = 0; i < 4; i++) {
        pv[i][0] = (p[0][0] * v[i][0]) + (p[2][0] * v[i][2]);
        pv[i][1] = (p[1][1] * v[i][1]) + (p[2][1] * v[i][2]);
        pv[i][2] = p[2][2] * v[i][2];
        pv[i][3] = -v[i][2];
    }
    pv[3][2] += p[3][2];
}

// The XR fov equivalent to a game frustum (exact, from the same extents).
static Fov FovFromGame(float fovAngle, float fovScaleX, float centerX, float fovScaleY, float centerY) {
    const float t = std::tan(fovAngle * kPi / 360.0f); // half-width at unit distance
    const float cx = centerX * (2.0f * t) / fovScaleX;
    const float ty = t * fovScaleY / fovScaleX;
    const float cy = centerY * (2.0f * ty) / fovScaleY;
    return { std::atan(cx - t), std::atan(cx + t), std::atan(cy + ty), std::atan(cy - ty) };
}

static const Pose kIdentityPose = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };

static void ClipOf(const float m[4][4], Vec3 p, float out[4]) {
    for (int j = 0; j < 4; j++) {
        out[j] = p.x * m[0][j] + p.y * m[1][j] + p.z * m[2][j] + m[3][j];
    }
}

static Quat AxisAngle(Vec3 axis, float radians) {
    const float s = std::sin(radians * 0.5f);
    return { axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5f) };
}

// ---- Cases ----------------------------------------------------------------------------------

static void CaseMatchesGameCamera(const char* label, Vec3 eye, Vec3 at, Vec3 up, float fovAngle, float centerX,
                                  float centerY) {
    char what[160];
    const float nearZ = 10.0f, farZ = 30000.0f, sx = 160.0f, sy = 120.0f;
    float view[4][4], proj[4][4], ref[4][4];
    RefLookAt(view, eye, at, up);
    RefFrustrum(proj, fovAngle, nearZ, farZ, sx, centerX, sy, centerY);
    RefProjView(ref, proj, view);

    Anchor anchor;
    std::snprintf(what, sizeof(what), "%s: anchor built", label);
    check(what, AnchorFromLookAt(eye, at, up, &anchor));

    float v2[4][4], p2[4][4], ours[4][4];
    EyeViewMatrix(anchor, kIdentityPose, 1.0f, v2);
    FovProjectionMatrix(FovFromGame(fovAngle, sx, centerX, sy, centerY), nearZ, farZ, p2);
    MatMul(v2, p2, ours);

    double worst = 0.0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            const double scale = std::fmax(1.0, std::fabs(ref[i][j]));
            const double err = std::fabs(ours[i][j] - ref[i][j]) / scale;
            worst = std::fmax(worst, err);
        }
    }
    std::snprintf(what, sizeof(what), "%s: matches the game's projection-view matrix", label);
    checkNear(what, worst, 0.0, 2e-4);
}

static void CaseIpd() {
    Anchor a;
    AnchorFromLookAt({ 100.0f, 50.0f, -30.0f }, { 100.0f, 50.0f, -1030.0f }, { 0.0f, 1.0f, 0.0f }, &a);
    const float upm = 40.0f; // world units per metre
    const Pose left = { { 0, 0, 0, 1 }, { -0.0315f, 0.0f, 0.0f } };
    const Pose right = { { 0, 0, 0, 1 }, { 0.0315f, 0.0f, 0.0f } };
    float vl[4][4], vr[4][4];
    EyeViewMatrix(a, left, upm, vl);
    EyeViewMatrix(a, right, upm, vr);
    // Eye world position from a view matrix: pos = -(row3 * R^T) with R's columns the basis.
    auto eyePos = [](const float v[4][4]) {
        Vec3 p;
        p.x = -(v[3][0] * v[0][0] + v[3][1] * v[0][1] + v[3][2] * v[0][2]);
        p.y = -(v[3][0] * v[1][0] + v[3][1] * v[1][1] + v[3][2] * v[1][2]);
        p.z = -(v[3][0] * v[2][0] + v[3][1] * v[2][1] + v[3][2] * v[2][2]);
        return p;
    };
    const Vec3 pl = eyePos(vl), pr = eyePos(vr);
    checkNear("IPD: eye separation is the tracked offset in world units",
              std::sqrt((pr.x - pl.x) * (pr.x - pl.x) + (pr.y - pl.y) * (pr.y - pl.y) + (pr.z - pl.z) * (pr.z - pl.z)),
              0.063 * upm, 1e-3);
    checkNear("IPD: midpoint stays on the anchor (x)", (pl.x + pr.x) * 0.5, 100.0, 1e-3);
    checkNear("IPD: midpoint stays on the anchor (z)", (pl.z + pr.z) * 0.5, -30.0, 1e-3);
    // Anchor looks down world -z with up +y, so its right is world +x: the left eye must be at -x.
    check("IPD: left eye is on the anchor's left", pl.x < pr.x);
}

static void CaseHeadRotationMapsOntoAnchor() {
    // Anchor looking down world +x (at is +x of eye), up +z: right = up x back.
    Anchor a;
    AnchorFromLookAt({ 0, 0, 0 }, { 10, 0, 0 }, { 0, 0, 1 }, &a);
    // XR: +90 degrees about +y turns the -z forward toward -x, i.e. the viewer looks left.
    const Pose yawLeft = { AxisAngle({ 0, 1, 0 }, kPi * 0.5f), { 0, 0, 0 } };
    float v[4][4];
    EyeViewMatrix(a, yawLeft, 1.0f, v);
    // Column 2 of V is the eye's back vector in world space; forward = -back.
    const Vec3 forward = { -v[0][2], -v[1][2], -v[2][2] };
    checkNear("yaw left: forward = -anchor.right (x)", forward.x, -a.right.x, 1e-5);
    checkNear("yaw left: forward = -anchor.right (y)", forward.y, -a.right.y, 1e-5);
    checkNear("yaw left: forward = -anchor.right (z)", forward.z, -a.right.z, 1e-5);

    const Pose pitchUp = { AxisAngle({ 1, 0, 0 }, kPi / 6.0f), { 0, 0, 0 } };
    EyeViewMatrix(a, pitchUp, 1.0f, v);
    const Vec3 f2 = { -v[0][2], -v[1][2], -v[2][2] };
    const double upComponent = f2.x * a.up.x + f2.y * a.up.y + f2.z * a.up.z;
    checkNear("pitch up 30 deg: forward gains sin(30) of anchor up", upComponent, 0.5, 1e-5);

    // A full turn around: looking behind the anchor.
    const Pose behind = { AxisAngle({ 0, 1, 0 }, kPi), { 0, 0, 0 } };
    EyeViewMatrix(a, behind, 1.0f, v);
    const Vec3 f3 = { -v[0][2], -v[1][2], -v[2][2] };
    checkNear("yaw 180: looks along anchor back", f3.x * a.back.x + f3.y * a.back.y + f3.z * a.back.z, 1.0, 1e-5);
}

static void CaseRightHanded() {
    Anchor a;
    AnchorFromLookAt({ 5, -3, 2 }, { 900, 120, -400 }, { 0.1f, 1.0f, 0.2f }, &a);
    Vec3 axis = { 0.3f, 0.8f, -0.5f };
    const float n = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    axis = { axis.x / n, axis.y / n, axis.z / n };
    const Pose p = { AxisAngle(axis, 1.1f), { 0.1f, -0.05f, 0.2f } };
    float v[4][4];
    EyeViewMatrix(a, p, 30.0f, v);
    const double det = v[0][0] * (v[1][1] * v[2][2] - v[2][1] * v[1][2]) -
                       v[1][0] * (v[0][1] * v[2][2] - v[2][1] * v[0][2]) +
                       v[2][0] * (v[0][1] * v[1][2] - v[1][1] * v[0][2]);
    checkNear("view rotation is a proper rotation (det +1)", det, 1.0, 1e-5);
}

static void CaseAsymmetricEdges() {
    // Steam Frame left eye, measured (docs/VR_ARCHITECTURE.md M0-A).
    const Fov fov = { -1.028f, 0.885f, 0.864f, -1.048f };
    Anchor a;
    AnchorFromLookAt({ 0, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, &a);
    float pv[4][4];
    EyeProjView(a, kIdentityPose, fov, 1.0f, 1.0f, 1000.0f, pv);
    const float d = 50.0f; // a point 50 units ahead
    float c[4];
    ClipOf(pv, { d * std::tan(fov.angleRight), 0.0f, -d }, c);
    checkNear("asym fov: right edge -> NDC x = +1", c[0] / c[3], 1.0, 1e-4);
    ClipOf(pv, { d * std::tan(fov.angleLeft), 0.0f, -d }, c);
    checkNear("asym fov: left edge -> NDC x = -1", c[0] / c[3], -1.0, 1e-4);
    ClipOf(pv, { 0.0f, d * std::tan(fov.angleUp), -d }, c);
    checkNear("asym fov: top edge -> NDC y = +1", c[1] / c[3], 1.0, 1e-4);
    ClipOf(pv, { 0.0f, d * std::tan(fov.angleDown), -d }, c);
    checkNear("asym fov: bottom edge -> NDC y = -1", c[1] / c[3], -1.0, 1e-4);
    ClipOf(pv, { 0.0f, 0.0f, -1.0f }, c);
    checkNear("depth: near plane -> NDC z = -1", c[2] / c[3], -1.0, 1e-4);
    ClipOf(pv, { 0.0f, 0.0f, -1000.0f }, c);
    checkNear("depth: far plane -> NDC z = +1", c[2] / c[3], 1.0, 1e-3);
    check("depth: points ahead have w > 0", c[3] > 0.0f);
}

static void CaseFixedPointFit() {
    // Far from the world origin, as on a real course: translation terms exceed s15.16 range.
    Anchor a;
    // Looking back toward the origin: eye . back ~ |eye| ~ 34600, so the depth translation term
    // (m[3][2], m[3][3]) passes the s15.16 range.
    AnchorFromLookAt({ 21000.0f, 9000.0f, -26000.0f }, { 20370.0f, 8730.0f, -25220.0f }, { 0, 1, 0 }, &a);
    const Fov fov = { -1.028f, 0.885f, 0.864f, -1.048f };
    float view[4][4], proj[4][4], raw[4][4], fitted[4][4];
    EyeViewMatrix(a, kIdentityPose, 1.0f, view);
    FovProjectionMatrix(fov, 10.0f, 30000.0f, proj);
    MatMul(view, proj, raw);
    const float scale = EyeProjView(a, kIdentityPose, fov, 1.0f, 10.0f, 30000.0f, fitted);
    float largestRaw = 0.0f, largestFit = 0.0f;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            largestRaw = std::fmax(largestRaw, std::fabs(raw[i][j]));
            largestFit = std::fmax(largestFit, std::fabs(fitted[i][j]));
        }
    }
    check("fit: this camera really overflows s15.16 unscaled", largestRaw > 32768.0f);
    check("fit: scaled matrix is inside the limit", largestFit <= 30000.0f + 1e-2f);
    check("fit: a scale below 1 was applied", scale < 1.0f);
    const Vec3 pts[3] = { { 20500.0f, 8800.0f, -25400.0f }, { 20010.0f, 8600.0f, -24800.0f }, { 18000.0f, 7600.0f, -22000.0f } };
    double worst = 0.0;
    for (const Vec3& p : pts) {
        float cr[4], cf[4];
        ClipOf(raw, p, cr);
        ClipOf(fitted, p, cf);
        for (int k = 0; k < 3; k++) {
            worst = std::fmax(worst, std::fabs(cr[k] / cr[3] - cf[k] / cf[3]));
        }
        check("fit: w keeps its sign", (cr[3] > 0.0f) == (cf[3] > 0.0f));
    }
    checkNear("fit: NDC unchanged by the scale", worst, 0.0, 1e-4);
}

static void CaseDegenerate() {
    Anchor a = {};
    check("degenerate: eye == at refused", !AnchorFromLookAt({ 1, 2, 3 }, { 1, 2, 3 }, { 0, 1, 0 }, &a));
    check("degenerate: up parallel to view refused", !AnchorFromLookAt({ 0, 0, 0 }, { 0, 5, 0 }, { 0, 1, 0 }, &a));
}

int main() {
    std::printf("gdx_vr_math_tests\n");
    CaseMatchesGameCamera("straight ahead", { 0, 0, 0 }, { 0, 0, -100 }, { 0, 1, 0 }, 70.0f, 0.0f, 0.0f);
    CaseMatchesGameCamera("course camera", { 1200.0f, 340.0f, -5600.0f }, { 1500.0f, 300.0f, -5200.0f },
                          { 0.05f, 0.99f, -0.02f }, 65.0f, 0.0f, 0.0f);
    CaseMatchesGameCamera("off-centre frustum", { -800.0f, 90.0f, 2400.0f }, { -790.0f, 85.0f, 2300.0f },
                          { 0.0f, 1.0f, 0.0f }, 80.0f, 12.0f, -7.0f);
    CaseMatchesGameCamera("banked in a pipe", { 300.0f, -200.0f, 50.0f }, { 380.0f, -150.0f, -40.0f },
                          { 0.7f, 0.3f, 0.6f }, 60.0f, 0.0f, 3.0f);
    CaseIpd();
    CaseHeadRotationMapsOntoAnchor();
    CaseRightHanded();
    CaseAsymmetricEdges();
    CaseFixedPointFit();
    CaseDegenerate();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}

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

static void CasePointer() {
    // A panel 2 m ahead (-z), facing the viewer, 2.4 x 1.35 m, unrotated.
    const Pose panel = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -2.0f } };
    float u = -1.0f, v = -1.0f, d = -1.0f;
    check("straight ahead hits the centre", RayHitsPanel({ 0, 0, 0 }, { 0, 0, -1 }, panel, 2.4f, 1.35f, &u, &v, &d));
    checkNear("centre u", u, 0.5, 1e-6);
    checkNear("centre v", v, 0.5, 1e-6);
    checkNear("centre distance", d, 2.0, 1e-6);
    // Aim at the top-right quarter point (x = +0.6, y = +0.3375): u 0.75, v 0.25 (v runs downward).
    Vec3 dir = { 0.6f, 0.3375f, -2.0f };
    const float n = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    dir = { dir.x / n, dir.y / n, dir.z / n };
    check("off-centre hit", RayHitsPanel({ 0, 0, 0 }, dir, panel, 2.4f, 1.35f, &u, &v, &d));
    checkNear("right is u > 0.5", u, 0.75, 1e-5);
    checkNear("up is v < 0.5", v, 0.25, 1e-5);
    check("pointing away misses", !RayHitsPanel({ 0, 0, 0 }, { 0, 0, 1 }, panel, 2.4f, 1.35f, &u, &v, &d));
    check("off the edge misses", !RayHitsPanel({ 0, 0, 0 }, { 0.8f, 0.0f, -0.6f }, panel, 2.4f, 1.35f, &u, &v, &d));
    check("from behind misses", !RayHitsPanel({ 0, 0, -3 }, { 0, 0, 1 }, panel, 2.4f, 1.35f, &u, &v, &d));
    // Yawed 90 degrees left (+y): the panel now sits at -x facing +x.
    const float h = std::sqrt(0.5f);
    const Pose yawed = { { 0.0f, h, 0.0f, h }, { -2.0f, 0.0f, 0.0f } };
    check("yawed panel hit", RayHitsPanel({ 0, 0, 0 }, { -1, 0, 0 }, yawed, 2.4f, 1.35f, &u, &v, &d));
    checkNear("yawed centre u", u, 0.5, 1e-5);

    Pose beam;
    float len = 0.0f;
    check("beam pose", BeamPose({ 0.2f, -0.3f, 0.0f }, { 0.2f, -0.3f, -2.0f }, { 0, 0, 0.5f }, &beam, &len));
    checkNear("beam length", len, 2.0, 1e-5);
    checkNear("beam centre z", beam.position.z, -1.0, 1e-5);
    const Vec3 by = Rotate(beam.orientation, { 0, 1, 0 });
    checkNear("beam +y runs along the ray", by.z, -1.0, 1e-4);
    const Vec3 bz = Rotate(beam.orientation, { 0, 0, 1 });
    check("beam faces the viewer", bz.y > 0.5f); // viewer is above the beam (0 vs -0.3)
    check("degenerate beam", !BeamPose({ 1, 1, 1 }, { 1, 1, 1 }, { 0, 0, 0 }, &beam, &len));
}

static void CaseNoRoll() {
    // Yaw 30 degrees left, pitch 20 down, roll 25: the roll goes, the view direction stays.
    const Quat yaw = AxisAngle({ 0, 1, 0 }, 30.0f * kPi / 180.0f);
    const Quat pitch = AxisAngle({ 1, 0, 0 }, -20.0f * kPi / 180.0f);
    const Quat roll = AxisAngle({ 0, 0, 1 }, 25.0f * kPi / 180.0f);
    const auto mul = [](const Quat& a, const Quat& b) {
        return Quat{ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    };
    const Pose head = { mul(mul(yaw, pitch), roll), { 0.1f, 1.6f, -0.2f } };
    const Pose level = NoRollPose(head);
    const Vec3 f0 = Rotate(head.orientation, { 0, 0, -1 });
    const Vec3 f1 = Rotate(level.orientation, { 0, 0, -1 });
    checkNear("no-roll keeps the view x", f1.x, f0.x, 1e-5);
    checkNear("no-roll keeps the view y", f1.y, f0.y, 1e-5);
    checkNear("no-roll keeps the view z", f1.z, f0.z, 1e-5);
    checkNear("no-roll x axis is level", Rotate(level.orientation, { 1, 0, 0 }).y, 0.0, 1e-5);
    check("no-roll up points up", Rotate(level.orientation, { 0, 1, 0 }).y > 0.9f);
    const Pose expect = { mul(yaw, pitch), head.position };
    const Vec3 x0 = Rotate(expect.orientation, { 1, 0, 0 });
    const Vec3 x1 = Rotate(level.orientation, { 1, 0, 0 });
    checkNear("no-roll equals yaw then pitch", x0.x * x1.x + x0.y * x1.y + x0.z * x1.z, 1.0, 1e-5);
    checkNear("no-roll keeps the position", level.position.y, 1.6, 1e-6);
    // Straight down: the level direction comes from the head's own x axis.
    const Pose down = NoRollPose({ mul(yaw, AxisAngle({ 1, 0, 0 }, -kPi / 2)), { 0, 0, 0 } });
    checkNear("looking down keeps the yaw", Rotate(down.orientation, { 1, 0, 0 }).x, std::cos(30.0f * kPi / 180.0f), 1e-4);
}

static void CaseWorldToTracking() {
    // Anchor: eye at (100, 50, 20) looking down world -z, up +y; 10 world units per metre.
    Anchor a;
    check("anchor", AnchorFromLookAt({ 100, 50, 20 }, { 100, 50, 10 }, { 0, 1, 0 }, &a));
    const Quat id = { 0, 0, 0, 1 };
    const Vec3 ox = { 1, 0, 0 }, oy = { 0, 1, 0 }, oz = { 0, 0, 1 };
    // A point 10 units straight ahead of the anchor is 1 m ahead in tracking space (-z).
    Pose p = WorldToTracking(a, 10.0f, id, { 0, 0, 0 }, { 100, 50, 10 }, ox, oy, oz);
    checkNear("ahead x", p.position.x, 0.0, 1e-5);
    checkNear("ahead z", p.position.z, -1.0, 1e-5);
    checkNear("world-aligned axes stay identity", p.orientation.w, 1.0, 1e-5);
    // Recentred 90 deg left (+y) about (2, 1.6, 3): ahead becomes -x.
    const float h = std::sqrt(0.5f);
    p = WorldToTracking(a, 10.0f, { 0, h, 0, h }, { 2, 1.6f, 3 }, { 100, 50, 10 }, ox, oy, oz);
    checkNear("recentred ahead x", p.position.x, 2.0 - 1.0, 1e-5);
    checkNear("recentred height", p.position.y, 1.6, 1e-5);
    checkNear("recentred z", p.position.z, 3.0, 1e-5);
    const Vec3 fx = Rotate(p.orientation, { 1, 0, 0 });
    checkNear("world +x turns with the recentre (to -z)", fx.z, -1.0, 1e-5);
    // Round trip with the eye placement: an eye at tracking offset e (recentre identity) sits at
    // world origin + ToWorld(anchor, e) * unitsPerMeter (EyeViewMatrix); mapping that world point
    // back gives e.
    const Vec3 e = { 0.1f, -0.2f, 0.3f };
    const Vec3 w = AnchorToWorld(a, e);
    p = WorldToTracking(a, 10.0f, id, { 0, 0, 0 }, { 100 + w.x * 10, 50 + w.y * 10, 20 + w.z * 10 }, ox, oy, oz);
    checkNear("round trip x", p.position.x, 0.1, 1e-5);
    checkNear("round trip y", p.position.y, -0.2, 1e-5);
    checkNear("round trip z", p.position.z, 0.3, 1e-5);
    const Quat q = QuatFromBasis({ 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 }); // 90 deg about +y
    const Vec3 r = Rotate(q, { 1, 0, 0 });
    checkNear("QuatFromBasis maps x to its first column", r.z, -1.0, 1e-5);
}

// Cabin cockpit precision. The machine's model matrix (s15.16 in the pool, as the game builds it),
// the eye derived from that matrix at the seat, and the eye's origin-relative projection, combined
// as the interpreter does with the camera-relative eye projection on (Interpreter::ComputeMP: the
// model matrix translated by -origin in double, then MP = model * pv and v * MP in float). Cabin
// points must land within 0.01 degrees of a double-precision reference anywhere within +-15000
// units of the course origin. The old path -- the absolute projection*view cut to s15.16 -- is run
// alongside to show the case can tell the two apart.
static float Fix1616(float x) {
    return (float)(std::trunc((double)x * 65536.0) / 65536.0); // guMtxF2L, then guMtxL2F
}

static double AngleErrorDeg(const float clip[4], double refX, double refY, const float proj[4][4]) {
    const double dx = clip[0] / clip[3] - refX;
    const double dy = clip[1] / clip[3] - refY;
    // Tangent-plane distance bounds the angle from above.
    return (std::fabs(dx) / proj[0][0] + std::fabs(dy) / proj[1][1]) * 180.0 / 3.14159265358979;
}

static void CaseCabinPrecision() {
    unsigned seed = 12345u;
    const auto rnd = [&seed](float lo, float hi) {
        seed = seed * 1664525u + 1013904223u;
        return lo + (hi - lo) * (float)((seed >> 8) & 0xFFFFFF) / (float)0xFFFFFF;
    };
    const float seat[3] = { 0.0f, 104.0f, 51.0f }; // Blue Falcon's measured seat, model units
    // Dash centre, a sill and the two mirror-end lamps of the same cabin, model units.
    const float pts[4][3] = { { 0, 85, 96 }, { 45, 95, 80 }, { 25, 110, 131 }, { -25, 110, 131 } };
    const Fov fov = { -0.95f, 0.85f, 0.9f, -0.95f };
    Pose head = kIdentityPose;
    head.position = { 0.032f, 0.05f, -0.02f };
    const float nearZ = 2.0f, farZ = 32768.0f;
    float proj[4][4];
    FovProjectionMatrix(fov, nearZ, farZ, proj);
    double worstNew = 0.0, worstOld = 0.0;
    for (int trial = 0; trial < 4000; trial++) {
        const float pos[3] = { rnd(-15000, 15000), rnd(-15000, 15000), rnd(-15000, 15000) };
        const auto unit = [](Vec3 v, bool* ok) {
            const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            *ok = l > 0.1f;
            return *ok ? Vec3{ v.x / l, v.y / l, v.z / l } : v;
        };
        bool ok = false;
        const Vec3 up = unit({ rnd(-1, 1), rnd(-1, 1), rnd(-1, 1) }, &ok);
        if (!ok) {
            continue;
        }
        Vec3 f = { rnd(-1, 1), rnd(-1, 1), rnd(-1, 1) };
        const float along = f.x * up.x + f.y * up.y + f.z * up.z;
        const Vec3 fwd = unit({ f.x - along * up.x, f.y - along * up.y, f.z - along * up.z }, &ok);
        if (!ok) {
            continue;
        }
        const Vec3 side = { up.y * fwd.z - up.z * fwd.y, up.z * fwd.x - up.x * fwd.z, up.x * fwd.y - up.y * fwd.x };
        // The pool's model matrix: rows 0.1 * (model +x, up, forward), translation the position.
        float m[4][4] = { { side.x * 0.1f, side.y * 0.1f, side.z * 0.1f, 0 },
                          { up.x * 0.1f, up.y * 0.1f, up.z * 0.1f, 0 },
                          { fwd.x * 0.1f, fwd.y * 0.1f, fwd.z * 0.1f, 0 },
                          { pos[0], pos[1], pos[2], 1 } };
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                m[i][j] = Fix1616(m[i][j]);
            }
        }
        // The cabin eye: that matrix at the seat, in double; the view looks far along the forward.
        double eyeD[3];
        ModelPointExact(m, seat, eyeD);
        const Vec3 eye = { (float)eyeD[0], (float)eyeD[1], (float)eyeD[2] };
        Anchor a;
        if (!AnchorFromLookAt(eye, { eye.x + fwd.x * 4096.0f, eye.y + fwd.y * 4096.0f, eye.z + fwd.z * 4096.0f }, up,
                              &a)) {
            continue;
        }
        float rel[4][4], abs[4][4];
        EyeProjViewRelative(a, head, fov, 1.0f, nearZ, farZ, rel);
        EyeProjView(a, head, fov, 1.0f, nearZ, farZ, abs);
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                abs[i][j] = Fix1616(abs[i][j]);
            }
        }
        // Interpreter, camera-relative: the model matrix less the origin in double, then float.
        float mRel[4][4];
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 3; j++) {
                mRel[i][j] = (float)((double)m[i][j] - (double)m[i][3] * eyeD[j]);
            }
            mRel[i][3] = m[i][3];
        }
        float mpNew[4][4], mpOld[4][4];
        MatMul(mRel, rel, mpNew);
        MatMul(m, abs, mpOld);
        // Reference, in double: the eye's world position and axes, the point, the same frustum.
        const Vec3 off = AnchorToWorld(a, head.position);
        const double ex = eyeD[0] + off.x, ey = eyeD[1] + off.y, ez = eyeD[2] + off.z;
        for (const auto& c : pts) {
            double w[3];
            ModelPointExact(m, c, w);
            const double d[3] = { w[0] - ex, w[1] - ey, w[2] - ez };
            const double vx = d[0] * a.right.x + d[1] * a.right.y + d[2] * a.right.z;
            const double vy = d[0] * a.up.x + d[1] * a.up.y + d[2] * a.up.z;
            const double vz = d[0] * a.back.x + d[1] * a.back.y + d[2] * a.back.z;
            const double refX = (vx * proj[0][0] + vz * proj[2][0]) / -vz;
            const double refY = (vy * proj[1][1] + vz * proj[2][1]) / -vz;
            float cn[4], co[4];
            for (int j = 0; j < 4; j++) {
                cn[j] = c[0] * mpNew[0][j] + c[1] * mpNew[1][j] + c[2] * mpNew[2][j] + mpNew[3][j];
                co[j] = c[0] * mpOld[0][j] + c[1] * mpOld[1][j] + c[2] * mpOld[2][j] + mpOld[3][j];
            }
            worstNew = std::fmax(worstNew, AngleErrorDeg(cn, refX, refY, proj));
            worstOld = std::fmax(worstOld, AngleErrorDeg(co, refX, refY, proj));
        }
    }
    std::printf("    cabin points, world +-15000: worst %.5f deg camera-relative, %.3f deg s15.16\n", worstNew,
                worstOld);
    check("cabin: camera-relative eye projection within 0.01 degrees", worstNew < 0.01);
    check("cabin: the s15.16 projection*view really was off by more", worstOld > 0.1);
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
    CasePointer();
    CaseNoRoll();
    CaseWorldToTracking();
    CaseCabinPrecision();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}

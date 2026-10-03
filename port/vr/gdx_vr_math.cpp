// port/vr/gdx_vr_math.cpp — see gdx_vr_math.h.

#include "gdx_vr_math.h"

#include <cmath>

namespace gdx_vr {

namespace {

Vec3 Add(const Vec3& a, const Vec3& b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

Vec3 Scale(const Vec3& a, float s) {
    return { a.x * s, a.y * s, a.z * s };
}

float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

bool Normalize(Vec3* v) {
    const float len2 = Dot(*v, *v);
    if (!(len2 > 0.0f)) {
        return false;
    }
    const float inv = 1.0f / std::sqrt(len2);
    *v = Scale(*v, inv);
    return true;
}

} // namespace

Vec3 AnchorToWorld(const Anchor& a, const Vec3& local) {
    return Add(Add(Scale(a.right, local.x), Scale(a.up, local.y)), Scale(a.back, local.z));
}

namespace {
Vec3 ToWorld(const Anchor& a, const Vec3& local) {
    return AnchorToWorld(a, local);
}
} // namespace

Vec3 Rotate(const Quat& q, const Vec3& v) {
    // v' = v + 2w(u x v) + 2(u x (u x v)), u = q.xyz
    const Vec3 u = { q.x, q.y, q.z };
    const Vec3 t = Scale(Cross(u, v), 2.0f);
    return Add(Add(v, Scale(t, q.w)), Cross(u, t));
}

bool RayHitsPanel(const Vec3& origin, const Vec3& dir, const Pose& panel, float width, float height, float* u,
                  float* v, float* distance) {
    const Vec3 normal = Rotate(panel.orientation, { 0.0f, 0.0f, 1.0f });
    const float facing = Dot(dir, normal);
    if (facing >= -1e-4f) {
        return false; // parallel, or arriving from behind the panel
    }
    const Vec3 toCentre = { panel.position.x - origin.x, panel.position.y - origin.y, panel.position.z - origin.z };
    const float t = Dot(toCentre, normal) / facing;
    if (t <= 0.0f) {
        return false; // the panel is behind the ray
    }
    const Vec3 hit = { origin.x + dir.x * t - panel.position.x, origin.y + dir.y * t - panel.position.y,
                       origin.z + dir.z * t - panel.position.z };
    const float px = Dot(hit, Rotate(panel.orientation, { 1.0f, 0.0f, 0.0f }));
    const float py = Dot(hit, Rotate(panel.orientation, { 0.0f, 1.0f, 0.0f }));
    const float hu = px / width + 0.5f;
    const float hv = 0.5f - py / height;
    if (hu < 0.0f || hu > 1.0f || hv < 0.0f || hv > 1.0f) {
        return false;
    }
    *u = hu;
    *v = hv;
    *distance = t;
    return true;
}

bool BeamPose(const Vec3& from, const Vec3& to, const Vec3& viewer, Pose* out, float* length) {
    Vec3 y = { to.x - from.x, to.y - from.y, to.z - from.z };
    const float len = std::sqrt(Dot(y, y));
    if (!Normalize(&y)) {
        return false;
    }
    const Vec3 centre = Add(from, Scale(y, len * 0.5f));
    // Face the viewer: the component of (viewer - centre) across the beam. Looking straight down
    // the beam there is none; any perpendicular will do then.
    Vec3 z = { viewer.x - centre.x, viewer.y - centre.y, viewer.z - centre.z };
    z = Add(z, Scale(y, -Dot(z, y)));
    if (!Normalize(&z)) {
        z = std::fabs(y.y) < 0.9f ? Cross(y, { 0.0f, 1.0f, 0.0f }) : Cross(y, { 1.0f, 0.0f, 0.0f });
        Normalize(&z);
    }
    const Vec3 x = Cross(y, z);
    // Rotation matrix with columns x, y, z -> quaternion.
    const float m00 = x.x, m01 = y.x, m02 = z.x;
    const float m10 = x.y, m11 = y.y, m12 = z.y;
    const float m20 = x.z, m21 = y.z, m22 = z.z;
    Quat q;
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = { (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s };
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = { 0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s };
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = { (m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s };
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = { (m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s };
    }
    out->orientation = q;
    out->position = centre;
    *length = len;
    return true;
}

bool AnchorFromLookAt(const Vec3& eye, const Vec3& at, const Vec3& up, Anchor* out) {
    Vec3 back = { eye.x - at.x, eye.y - at.y, eye.z - at.z };
    if (!Normalize(&back)) {
        return false;
    }
    Vec3 right = Cross(up, back);
    if (!Normalize(&right)) {
        return false;
    }
    Vec3 trueUp = Cross(back, right);
    if (!Normalize(&trueUp)) {
        return false;
    }
    out->origin = eye;
    out->right = right;
    out->up = trueUp;
    out->back = back;
    return true;
}

void EyeViewMatrix(const Anchor& anchor, const Pose& eye, float unitsPerMeter, float out[4][4]) {
    const Vec3 pos = Add(anchor.origin, Scale(ToWorld(anchor, eye.position), unitsPerMeter));
    const Vec3 right = ToWorld(anchor, Rotate(eye.orientation, { 1.0f, 0.0f, 0.0f }));
    const Vec3 up = ToWorld(anchor, Rotate(eye.orientation, { 0.0f, 1.0f, 0.0f }));
    const Vec3 back = ToWorld(anchor, Rotate(eye.orientation, { 0.0f, 0.0f, 1.0f }));

    out[0][0] = right.x;
    out[1][0] = right.y;
    out[2][0] = right.z;
    out[0][1] = up.x;
    out[1][1] = up.y;
    out[2][1] = up.z;
    out[0][2] = back.x;
    out[1][2] = back.y;
    out[2][2] = back.z;
    out[0][3] = 0.0f;
    out[1][3] = 0.0f;
    out[2][3] = 0.0f;
    out[3][0] = -Dot(pos, right);
    out[3][1] = -Dot(pos, up);
    out[3][2] = -Dot(pos, back);
    out[3][3] = 1.0f;
}

void FovProjectionMatrix(const Fov& fov, float nearZ, float farZ, float out[4][4]) {
    const float left = nearZ * std::tan(fov.angleLeft);
    const float right = nearZ * std::tan(fov.angleRight);
    const float top = nearZ * std::tan(fov.angleUp);
    const float bottom = nearZ * std::tan(fov.angleDown);

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out[i][j] = 0.0f;
        }
    }
    out[0][0] = 2.0f * nearZ / (right - left);
    out[2][0] = (right + left) / (right - left);
    out[1][1] = 2.0f * nearZ / (top - bottom);
    out[2][1] = (top + bottom) / (top - bottom);
    out[2][2] = (nearZ + farZ) / (nearZ - farZ);
    out[3][2] = 2.0f * nearZ * farZ / (nearZ - farZ);
    out[2][3] = -1.0f;
}

void MatMul(const float a[4][4], const float b[4][4], float out[4][4]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[i][k] * b[k][j];
            }
            out[i][j] = sum;
        }
    }
}

float FitToFixedPoint(float m[4][4], float limit) {
    float largest = 0.0f;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            const float v = std::fabs(m[i][j]);
            if (v > largest) {
                largest = v;
            }
        }
    }
    if (!(largest > limit)) {
        return 1.0f;
    }
    const float scale = limit / largest;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            m[i][j] *= scale;
        }
    }
    return scale;
}

float EyeProjView(const Anchor& anchor, const Pose& eye, const Fov& fov, float unitsPerMeter, float nearZ, float farZ,
                  float out[4][4]) {
    float view[4][4];
    float proj[4][4];
    EyeViewMatrix(anchor, eye, unitsPerMeter, view);
    FovProjectionMatrix(fov, nearZ, farZ, proj);
    MatMul(view, proj, out);
    return FitToFixedPoint(out, 30000.0f);
}

} // namespace gdx_vr

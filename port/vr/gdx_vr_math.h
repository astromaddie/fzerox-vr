// port/vr/gdx_vr_math.h — per-eye projection-view matrices for stereo rendering.
//
// Plain POD types only (no OpenXR headers) so the maths is unit-tested without a runtime.
//
// Conventions, matching the game so its combined camera matrix can be replaced one-for-one:
//   * Row vectors: clip = world * PV, PV = V * P (decomp/src/game/camera.c
//     Camera_CalculateProjectionViewMtx).
//   * View space is right-handed, x right, y up, camera looking down -z. V's rotation columns are
//     (right, up, back) where back = normalize(eye - at) (decomp/src/sys/math.c Matrix_SetLookAt),
//     and V's row 3 is -eye dotted with each column.
//   * Projection is the glFrustum form Matrix_SetFrustrum writes: m[2][3] = -1, so w = -z_view,
//     and NDC z = -1 at the near plane.
//   * OpenXR's LOCAL/VIEW spaces share that handedness (+x right, +y up, +z back), so an XR pose
//     maps onto a camera basis by taking XR axis components as coefficients of (right, up, back).

#ifndef GDX_VR_MATH_H
#define GDX_VR_MATH_H

namespace gdx_vr {

struct Vec3 {
    float x, y, z;
};

struct Quat {
    float x, y, z, w;
};

// XrPosef equivalent.
struct Pose {
    Quat orientation;
    Vec3 position;
};

// XrFovf equivalent: tangent-space half angles in radians; left and down are normally negative.
struct Fov {
    float angleLeft, angleRight, angleUp, angleDown;
};

// A world-space reference frame the viewer's tracked head is attached to (the chase camera, or a
// point on the machine). Basis vectors are orthonormal; back points from the scene toward the
// viewer.
struct Anchor {
    Vec3 origin;
    Vec3 right, up, back;
};

// Build an anchor from the game camera's look-at inputs, with the same cross products as
// Matrix_SetLookAt. Returns false (anchor untouched) on degenerate input: eye == at, or up
// parallel to the view direction.
bool AnchorFromLookAt(const Vec3& eye, const Vec3& at, const Vec3& up, Anchor* out);

// Row-vector view matrix for an eye posed (in metres, OpenXR conventions) relative to `anchor`.
// `unitsPerMeter` converts tracked translation into world units.
void EyeViewMatrix(const Anchor& anchor, const Pose& eye, float unitsPerMeter, float out[4][4]);

// glFrustum-form projection for an asymmetric XR field of view.
void FovProjectionMatrix(const Fov& fov, float nearZ, float farZ, float out[4][4]);

// out = a * b (row-vector convention: apply a, then b). out may not alias a or b.
void MatMul(const float a[4][4], const float b[4][4], float out[4][4]);

// The game's combined matrix is stored as s15.16 fixed point, so every element must stay inside
// +-32768 (the Expansion Kit build widens the fov when a translation term nears that limit,
// camera.c). A uniform scale of a projection-view matrix leaves every projected point unchanged:
// x/w, y/w and z/w are invariant and clip tests compare against w. This scales `m` down so its
// largest element is at most `limit`, and returns the factor applied (1 when already inside).
float FitToFixedPoint(float m[4][4], float limit);

// Convenience: anchor + eye pose + fov -> fitted projection-view matrix for that eye.
float EyeProjView(const Anchor& anchor, const Pose& eye, const Fov& fov, float unitsPerMeter, float nearZ, float farZ,
                  float out[4][4]);

// The same eye's projection*view for positions RELATIVE TO THE ANCHOR ORIGIN: no world translation,
// so nothing in it is large and it is used in float, never fitted or cut to fixed point. The
// interpreter pairs it with the origin (Interpreter::SetVrEyeProjection), subtracting the origin
// from each model matrix in double; an s15.16 world-space projection*view rounds to ~0.1 units at
// a few thousand units from the course origin, degrees of jitter on a cockpit a few units away.
void EyeProjViewRelative(const Anchor& anchor, const Pose& eye, const Fov& fov, float unitsPerMeter, float nearZ,
                         float farZ, float out[4][4]);

// A model-space point through a row-vector model matrix (p * m), in double: m's translation is a
// world position whose float sum with a small offset would round to ~1e-3 units at 15000.
void ModelPointExact(const float m[4][4], const float p[3], double out[3]);

// Rotate v by unit quaternion q.
Vec3 Rotate(const Quat& q, const Vec3& v);

// A vector given in the anchor's local (XR) axes, expressed in world space.
Vec3 AnchorToWorld(const Anchor& anchor, const Vec3& local);

// Rotation whose columns are the orthonormal axes x, y, z (right-handed).
Quat QuatFromBasis(const Vec3& x, const Vec3& y, const Vec3& z);

// A pose fixed in the game world (position and orthonormal axes, world units), expressed in the
// headset's raw LOCAL tracking space: the inverse of how the eyes are placed. `anchor` is the frame
// the tracked head was attached to (BuildActiveEyeProjView), `unitsPerMeter` the world scale, and
// the recentre is a yaw (recentreYaw) about the point (recentreOrigin: recentre x, height anchor,
// recentre z) -- the frame Recentred() removes from raw eye poses.
Pose WorldToTracking(const Anchor& anchor, float unitsPerMeter, const Quat& recentreYaw, const Vec3& recentreOrigin,
                     const Vec3& worldPos, const Vec3& worldX, const Vec3& worldY, const Vec3& worldZ);

// Menu pointer: where a ray (unit `dir`) meets a flat panel posed like an XrCompositionLayerQuad
// (centred on panel.position, facing its local +z, `width` x `height`). Returns true only for a hit
// on the panel's front face inside its rectangle; u runs 0..1 left to right and v 0..1 top to
// bottom (image order), and *distance is the ray length to the hit.
bool RayHitsPanel(const Vec3& origin, const Vec3& dir, const Pose& panel, float width, float height, float* u,
                  float* v, float* distance);

// Pose for a thin quad drawn as a beam from `from` to `to`: centred between them, its local +y
// along the beam and its face (+z) turned toward `viewer` as far as the beam allows. *length is the
// beam length (the quad's height). Returns false when from == to.
bool BeamPose(const Vec3& from, const Vec3& to, const Vec3& viewer, Pose* out, float* length);

// The head pose with its roll taken out: same position and view direction, with the x axis held
// level, so a panel placed in front of it follows head yaw and pitch but never tilts with the head
// (standard.md section 4). Looking straight up or down, the level direction comes from the head's
// own x axis instead.
Pose NoRollPose(const Pose& head);

} // namespace gdx_vr

#endif // GDX_VR_MATH_H

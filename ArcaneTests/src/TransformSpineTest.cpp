// Task 3 (F1): the transform spine in three dimensions. Transform now carries a
// vec3 position, a quaternion rotation and a vec3 scale, and ToMatrix/
// WorldTransform speak mat4. These are the properties that say what that must
// mean -- and, more importantly, what must NOT change: every scene in the tree
// is 2D, so the Z-axis-quaternion case has to reproduce the retired
// float-radians path exactly.
//
// Deliberately pure: no Registry, no host, no device. The hierarchy case below
// composes matrices by hand rather than driving TransformPropagationSystem --
// TransformPropagationTest.cpp already owns the system, and pinning `parent *
// local` here keeps the ORDER pinned independently of the walk that applies it
// (Task 4 replaces the walk, not the order).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/Components.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

using Catch::Approx;

namespace
{
    constexpr float kPi = 3.14159265358979323846f;

    // Element-wise mat4 comparison. Catch2 has no matrix matcher and glm's ==
    // is exact, which would make a legitimate 1-ulp difference in a cos/sin
    // product read as a failure with no diagnostic about WHICH element moved.
    void CheckMat4(const glm::mat4& actual, const glm::mat4& expected, float eps = 1e-6f)
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                CHECK(actual[c][r] == Approx(expected[c][r]).margin(eps));
    }

    // The mat3 Transform::ToMatrix produced BEFORE this task, copied verbatim
    // from Components.hpp as it stood at 5a3cb5be. This is the ONLY definition
    // of "the old 2D rotation" that survives the type change, so the
    // Z-axis-quaternion property below is checked against it rather than
    // against a re-derivation that could drift the same way the code did.
    glm::mat3 Legacy2DMatrix(glm::vec2 position, float rotation, glm::vec2 scale)
    {
        const float c = std::cos(rotation);
        const float s = std::sin(rotation);
        glm::mat3 m(1.0f);
        m[0] = glm::vec3(c * scale.x,  s * scale.x, 0.0f);
        m[1] = glm::vec3(-s * scale.y, c * scale.y, 0.0f);
        m[2] = glm::vec3(position.x,   position.y,  1.0f);
        return m;
    }

    // A 2D pose expressed in the 3D types: Z=0, rotation about +Z, unit Z scale.
    Arcane::Transform Planar(glm::vec2 position, float rotation, glm::vec2 scale)
    {
        Arcane::Transform t;
        t.position = glm::vec3(position, 0.0f);
        t.rotation = glm::angleAxis(rotation, glm::vec3(0.0f, 0.0f, 1.0f));
        t.scale    = glm::vec3(scale, 1.0f);
        return t;
    }
}

TEST_CASE("Transform: the default pose is the identity mat4", "[scene][transform]")
{
    const Arcane::Transform t;
    CHECK(t.position == glm::vec3(0.0f));
    CHECK(t.scale    == glm::vec3(1.0f));
    // Identity quaternion is (w=1, x=y=z=0) -- glm::quat's ctor takes w FIRST.
    CHECK(t.rotation.w == Approx(1.0f));
    CHECK(t.rotation.x == Approx(0.0f));
    CHECK(t.rotation.y == Approx(0.0f));
    CHECK(t.rotation.z == Approx(0.0f));
    CheckMat4(t.ToMatrix(), glm::mat4(1.0f));

    // WorldTransform is the same identity, so an entity that reaches
    // propagation before its first walk renders where it was authored rather
    // than collapsing to the origin.
    CHECK(Arcane::WorldTransform{}.matrix == glm::mat4(1.0f));
}

TEST_CASE("Transform: ToMatrix composes translate * rotate * scale, in that order",
          "[scene][transform]")
{
    Arcane::Transform t;
    t.position = glm::vec3(3.0f, -4.0f, 5.0f);
    t.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    t.scale    = glm::vec3(2.0f, 3.0f, 4.0f);

    const glm::mat4 expected =
        glm::translate(glm::mat4(1.0f), t.position) *
        glm::mat4_cast(t.rotation) *
        glm::scale(glm::mat4(1.0f), t.scale);
    CheckMat4(t.ToMatrix(), expected, 1e-5f);

    // The order is observable, not just structural: T*R*S turns the SCALED
    // local axes and then shifts, so the local +X axis (2 m long after scale)
    // lands 2 m along world +Y from the position. S*R*T or R*T*S would not.
    const glm::vec4 xAxisTip = t.ToMatrix() * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    CHECK(xAxisTip.x == Approx(3.0f).margin(1e-5));
    CHECK(xAxisTip.y == Approx(-2.0f).margin(1e-5));
    CHECK(xAxisTip.z == Approx(5.0f).margin(1e-5));

    // Translation lives in COLUMN 3 now (it was column 2 in the mat3). Every
    // reader that reached for matrix[2] had to move; this pins where it went.
    CHECK(t.ToMatrix()[3] == glm::vec4(3.0f, -4.0f, 5.0f, 1.0f));
}

TEST_CASE("Transform: a Z-axis quaternion reproduces the retired 2D rotation matrix",
          "[scene][transform]")
{
    // THE 2D-SURVIVAL PROPERTY. Every scene in the tree is planar, so a pose
    // authored as (vec2, radians, vec2) and re-expressed as (vec3, quat about
    // +Z, vec3) must produce the SAME basis and the SAME translation -- the
    // mat4 is the old mat3 with an untouched Z row/column threaded through it.
    const float angles[] = { 0.0f, 0.3f, kPi * 0.5f, 2.0f, -1.25f, kPi };
    const glm::vec2 position(-1.5f, 4.0f);
    const glm::vec2 scale(2.0f, 0.5f);

    for (float rot : angles)
    {
        const glm::mat3 legacy = Legacy2DMatrix(position, rot, scale);
        const glm::mat4 now    = Planar(position, rot, scale).ToMatrix();

        // Basis: the 2x2 block of columns 0/1, rows 0/1.
        CHECK(now[0][0] == Approx(legacy[0][0]).margin(1e-6));
        CHECK(now[0][1] == Approx(legacy[0][1]).margin(1e-6));
        CHECK(now[1][0] == Approx(legacy[1][0]).margin(1e-6));
        CHECK(now[1][1] == Approx(legacy[1][1]).margin(1e-6));
        // Translation: mat3 column 2 -> mat4 column 3.
        CHECK(now[3][0] == Approx(legacy[2][0]).margin(1e-6));
        CHECK(now[3][1] == Approx(legacy[2][1]).margin(1e-6));
        // Out of plane: an unrotated, unscaled, unshifted Z.
        CHECK(now[0][2] == Approx(0.0f).margin(1e-6));
        CHECK(now[1][2] == Approx(0.0f).margin(1e-6));
        CHECK(now[2][2] == Approx(1.0f).margin(1e-6));
        CHECK(now[3][2] == Approx(0.0f).margin(1e-6));

        // The angle the renderers read back out of the world matrix
        // (atan2 of basis column 0) is the angle that went in.
        CHECK(std::atan2(now[0][1], now[0][0]) == Approx(std::atan2(legacy[0][1], legacy[0][0])).margin(1e-5));
    }
}

TEST_CASE("Transform: RotationZ and RotationAboutZ are inverses on the planar branch",
          "[scene][transform]")
{
    // The planar bridge had no test of its own, and FOUR seams depend on it for
    // their SIGN convention -- the physics write-back and author reconcile,
    // sprite interpolation, and the gizmo write-back. A flipped sign in BOTH
    // directions would cancel out in a round trip, so RotationAboutZ is pinned
    // against an INDEPENDENT construction (what the rotation does to +X) as
    // well as against its inverse.
    const float angles[] = { 0.0f, 0.3f, kPi * 0.5f, -1.25f, 2.5f };
    for (float rot : angles)
    {
        const glm::quat q = Arcane::RotationAboutZ(rot);
        const glm::vec3 turned = q * glm::vec3(1.0f, 0.0f, 0.0f);
        CHECK(turned.x == Approx(std::cos(rot)).margin(1e-5));   // a sign flip fails here
        CHECK(turned.y == Approx(std::sin(rot)).margin(1e-5));
        CHECK(turned.z == Approx(0.0f).margin(1e-6));
        CHECK(Arcane::RotationZ(q) == Approx(rot).margin(1e-5));
    }

    // RotationZ answers in (-pi, pi] -- it is an atan2. An authored angle
    // OUTSIDE that range comes back WRAPPED, not preserved: 5 rad reads as
    // 5 - 2pi. That is correct (they are the same orientation, and a quaternion
    // cannot tell them apart at all), but it IS a real difference from the
    // retired float rotation, which stored whatever winding it was handed.
    // Anything that needs to count turns must not route through here.
    const float wound = 5.0f;
    CHECK(Arcane::RotationZ(Arcane::RotationAboutZ(wound))
          == Approx(wound - 2.0f * kPi).margin(1e-5));
    CHECK_FALSE(Arcane::RotationZ(Arcane::RotationAboutZ(wound))
                == Approx(wound).margin(1e-3));

    // The identity reads as EXACTLY zero, not a denormal: this angle goes
    // straight to the batcher, whose rotation-0 fast path is an exact compare
    // (Batcher2D::QuadCorners), so a 1e-18 here would silently cost every
    // unrotated sprite its byte-identical path.
    CHECK(Arcane::RotationZ(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) == 0.0f);
}

TEST_CASE("Transform: hierarchy composition is parent * local", "[scene][transform]")
{
    // Parent: at (10, 0, 0), turned +90 degrees about +Z, scaled 2x uniformly.
    Arcane::Transform parent;
    parent.position = glm::vec3(10.0f, 0.0f, 0.0f);
    parent.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    parent.scale    = glm::vec3(2.0f);

    // Child: 3 m along the parent's local +X, no rotation, no scale.
    Arcane::Transform local;
    local.position = glm::vec3(3.0f, 0.0f, 0.0f);

    const glm::mat4 world = parent.ToMatrix() * local.ToMatrix();

    // Hand-computed: the parent scales the offset to 6 m, turns it onto +Y,
    // then shifts by the parent position -> (10, 6, 0).
    glm::mat4 expected(0.0f);
    expected[0] = glm::vec4(0.0f, 2.0f, 0.0f, 0.0f);    // parent +X -> world +Y, length 2
    expected[1] = glm::vec4(-2.0f, 0.0f, 0.0f, 0.0f);   // parent +Y -> world -X, length 2
    expected[2] = glm::vec4(0.0f, 0.0f, 2.0f, 0.0f);    // Z untouched by a Z-axis turn
    expected[3] = glm::vec4(10.0f, 6.0f, 0.0f, 1.0f);
    CheckMat4(world, expected, 1e-5f);

    // The reverse product is a DIFFERENT matrix -- the property is the order,
    // not merely that a product exists.
    CHECK((local.ToMatrix() * parent.ToMatrix())[3] != world[3]);
}

TEST_CASE("PreviousTransform: the render blend slerps on the shortest arc across +-180 degrees",
          "[scene][transform][interp]")
{
    // PreviousTransform's contract (Components.hpp) has always been that it is
    // stored DECOMPOSED so rotation interpolates on the shortest arc rather
    // than by lerping matrix components. In 3D that is slerp: 170 deg -> -170
    // deg is a +20 deg step through 180, never a -340 deg unwind through 0.
    Arcane::PreviousTransform prev;
    prev.position = glm::vec3(0.0f, 0.0f, 0.0f);
    prev.rotation = glm::angleAxis(glm::radians(170.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    Arcane::PreviousTransform cur;
    cur.position = glm::vec3(10.0f, 0.0f, 0.0f);
    cur.rotation = glm::angleAxis(glm::radians(-170.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    const Arcane::PreviousTransform mid = Arcane::LerpPose(prev, cur, 0.5f);

    CHECK(mid.position.x == Approx(5.0f));
    CHECK(mid.position.y == Approx(0.0f).margin(1e-6));
    CHECK(mid.position.z == Approx(0.0f).margin(1e-6));

    // Compare by what the rotation DOES: the midpoint is 180 deg, so +X maps to
    // -X. Checking the quaternion components directly would be hostage to the
    // double cover (q and -q are the same rotation).
    const glm::vec3 turned = mid.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    CHECK(turned.x == Approx(-1.0f).margin(1e-5));
    CHECK(turned.y == Approx(0.0f).margin(1e-5));
    CHECK(turned.z == Approx(0.0f).margin(1e-5));

    // A component-wise lerp of the two quaternions -- the thing this must NOT
    // be -- collapses toward the identity here (their W's cancel), which is the
    // 0 deg long-way-round answer. Pinning that it differs keeps a future
    // "simplification" to glm::mix from passing silently.
    const glm::quat naive = glm::quat(
        0.5f * (prev.rotation.w + cur.rotation.w), 0.5f * (prev.rotation.x + cur.rotation.x),
        0.5f * (prev.rotation.y + cur.rotation.y), 0.5f * (prev.rotation.z + cur.rotation.z));
    const glm::vec3 naiveTurned = glm::normalize(naive) * glm::vec3(1.0f, 0.0f, 0.0f);
    CHECK(naiveTurned.x == Approx(1.0f).margin(1e-5));   // 0 deg: the WRONG arc

    // Endpoints are exact.
    const Arcane::PreviousTransform atZero = Arcane::LerpPose(prev, cur, 0.0f);
    const glm::vec3 zeroTurned = atZero.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    CHECK(zeroTurned.x == Approx(std::cos(glm::radians(170.0f))).margin(1e-5));
    CHECK(zeroTurned.y == Approx(std::sin(glm::radians(170.0f))).margin(1e-5));
    const Arcane::PreviousTransform atOne = Arcane::LerpPose(prev, cur, 1.0f);
    CHECK(atOne.position.x == Approx(10.0f));
}

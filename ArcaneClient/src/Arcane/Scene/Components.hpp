#pragma once

// Scene components: plain reflected data. Every engine component is
// ASTRA_REFLECT-annotated from day one (design rule) so the editor/JSON/server
// path stays open at ~zero cost via the Astra 3.2 visitFields seam. Header-only;
// reflect blocks at namespace scope register once per module (MetaRegistry is
// idempotent), and the simulation Registry is owned by the host module.

#include <Arcane/Guid.hpp>

#include <Astra/Reflection/Reflection.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>          // quat, mat4_cast, slerp

#include <cmath>
#include <cstdint>
#include <string>

namespace Arcane
{
    // Task 3 (F1): the transform spine is 3D. position/scale are vec3 and
    // rotation is a QUATERNION -- not Euler angles, which have no canonical
    // order and gimbal-lock, and not a matrix, which cannot be interpolated on
    // the shortest arc (see PreviousTransform below). The editor still AUTHORS
    // it as EULER ANGLES: the Inspector's FieldKind::Quat draws a three-axis
    // Euler view over the quaternion and honours the AngleFormat attribute on
    // the reflect row (see InspectorFields.cpp). The UNIT is whatever that
    // attribute says, and for this field it says RADIANS -- see the reflect
    // block below, which carries AngleFormat::Radians deliberately so the
    // widening did not silently flip the row from the radians the retired float
    // showed. (This paragraph said "Euler degrees" and was wrong about its own
    // reflect block; the reflect block is the half that was right.)
    //
    // Every scene in the tree today is planar and stays planar: a 2D pose is
    // this type with position.z == 0, a rotation about +Z only, and scale.z ==
    // 1, which reproduces the retired mat3 path exactly (pinned by
    // TransformSpineTest.cpp's "Z-axis quaternion reproduces the retired 2D
    // rotation matrix" case).
    struct Transform
    {
        glm::vec3 position{0.0f, 0.0f, 0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};   // identity; glm's ctor is (w, x, y, z)
        glm::vec3 scale{1.0f, 1.0f, 1.0f};

        // TRS in column-major homogeneous mat4: translate * rotate * scale, so
        // the local axes are scaled, then turned, then shifted (translation in
        // COLUMN 3 -- it was column 2 in the mat3 this replaced, which is the
        // one index every reader of a world matrix had to move).
        //
        // Built column-wise rather than as three glm::translate/scale products:
        // this is called once per entity per propagation pass, and the product
        // form spends two full 4x4 multiplies re-deriving a result whose shape
        // is known. Equivalence is pinned against the product form in
        // TransformSpineTest.cpp.
        glm::mat4 ToMatrix() const
        {
            glm::mat4 m = glm::mat4_cast(rotation);
            m[0] *= scale.x;
            m[1] *= scale.y;
            m[2] *= scale.z;
            m[3] = glm::vec4(position, 1.0f);
            return m;
        }
    };

    struct WorldTransform
    {
        glm::mat4 matrix{1.0f};             // computed by TransformPropagationSystem; never authored
    };

    // The planar bridge (Task 3, F1). Three subsystems are deliberately still
    // 2D -- the physics simulation (Manifold2D is a 2D solver), the sprite
    // submission path, and the transform gizmo -- and each of them has to turn a
    // quaternion into the one angle it understands, and back. These two exist so
    // that conversion has ONE definition instead of a hand-rolled copy at each
    // seam that could drift in sign or convention.
    //
    // RotationZ reads the image of local +X, which is exactly how every consumer
    // of a WORLD matrix reads its angle (atan2 of basis column 0). That makes
    // the two agree for a planar pose, and degrade identically for one that is
    // not: an out-of-plane rotation projects onto the XY plane rather than
    // producing a different answer per call site.
    [[nodiscard]] inline float RotationZ(const glm::quat& q) noexcept
    {
        const glm::vec3 forward = q * glm::vec3(1.0f, 0.0f, 0.0f);
        return std::atan2(forward.y, forward.x);
    }

    [[nodiscard]] inline glm::quat RotationAboutZ(float radians) noexcept
    {
        return glm::angleAxis(radians, glm::vec3(0.0f, 0.0f, 1.0f));
    }

    // PreviousTransform (Epic 04.2): an entity's LOCAL pose at the previous fixed
    // step, captured by PhysicsSystem write-back before it overwrites Transform.
    // RenderSubmissionSystem draws at lerp(previous -> current, alpha) for smooth
    // slow-mo. Decomposed (position + rotation) so rotation interpolates on the
    // shortest arc, NOT by lerping matrix components. Purely derived render state:
    // an entity opts into interpolation by carrying it; absent -> the sprite snaps
    // to the latest step (unchanged).
    struct PreviousTransform
    {
        glm::vec3 position{0.0f, 0.0f, 0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    // The render-interpolation blend: prev -> cur at alpha. Rotation is SLERP,
    // which is the 3D expression of the shortest-arc contract above -- glm::slerp
    // negates one input when their dot is negative, so a 170 deg -> -170 deg step
    // takes the +20 deg arc through 180 rather than unwinding -340 deg through 0,
    // exactly as the retired float path's AngleLerp did. A component-wise
    // glm::mix of the two quaternions would take the wrong arc (pinned in
    // TransformSpineTest.cpp), and lerping the two POSE MATRICES would shear
    // through the middle of the turn instead of rotating.
    //
    // A free function rather than a method: it is a pure blend of two poses, and
    // the ONE consumer (RenderSubmissionSystem) blends a PreviousTransform
    // against a world pose it just decomposed, not against another component.
    //
    // The position half is spelled a + (b - a) * t, NOT glm::mix (which is
    // a * (1 - t) + b * t): that is the exact expression Arcane::Lerp
    // (SceneResources.hpp) used here before, and the two differ in the last ulp.
    // Keeping the arithmetic identical is what makes a planar sprite land on the
    // same sub-pixel it did before the widening.
    [[nodiscard]] inline PreviousTransform LerpPose(const PreviousTransform& prev,
                                                    const PreviousTransform& cur,
                                                    float alpha) noexcept
    {
        return PreviousTransform{ prev.position + (cur.position - prev.position) * alpha,
                                  glm::slerp(prev.rotation, cur.rotation, alpha) };
    }

    // The primitive a SpriteRenderer draws. Lets the ONE canonical 2D submission
    // path (RenderSubmissionSystem) render filled circles + capsules, not just
    // rectangles -- so a body's sprite can MATCH its collider shape (a circle
    // fixture -> a disc), and a multi-fixture body can be drawn as one child
    // sprite per fixture of the right shape. Rect is the default (unchanged).
    enum class SpriteShape : uint8_t
    {
        Rect    = 0,   // axis quad, rotated by the WorldTransform; the default
        Circle  = 1,   // filled disc; diameter == size.x, rotation-invariant
        Capsule = 2,   // horizontal capsule (central rect + two end discs)
    };

    struct SpriteRenderer
    {
        // The .arcsprite asset drawn: it supplies the texture, the UV sub-rect,
        // the world size in meters (sourceSize / ppu) and the pivot. Nil (the
        // default) or unresolved -> a 1x1 m untextured tint quad. There is NO
        // size field: an entity is sized by its Transform scale, matching
        // Unity/Unreal (neither puts a size on the sprite renderer), so one
        // asset can be shared by many entities at different scales.
        Guid        sprite{};
        glm::vec4   tint{1.0f, 1.0f, 1.0f, 1.0f};
        int32_t     sortingLayer = 0;
        int32_t     orderInLayer = 0;
        SpriteShape shape = SpriteShape::Rect; // primitive drawn (Rect/Circle/Capsule)
        // Optional sprite material (Slice 8): the Guid of a SAVED .arcmat asset.
        // Nil (the default) = the plain sprite pipeline, byte-identical to the
        // pre-material path. Resolved to a Batcher2D material id through the
        // SpriteMaterialTable resource at submission; unresolved Guids draw as
        // plain sprites until their compile lands. Rect shape only.
        Guid        material{};
    };

    // The scene's post-processing stack (post arc): the Guid of a SAVED
    // fullscreen .arcmat whose pass DAG runs between the linear canvas and the
    // tonemap (the material IS the stack; kSceneInput wires read the scene
    // color). The host sweeps for it each frame -- the FIRST entity with a
    // valid material wins, nil or unresolved means no chain (today's path,
    // byte-identical). One scene, one chain; per-camera stacks are a non-goal.
    struct PostProcess
    {
        Guid material{};
    };

    // The scene's viewpoint. The entity's WorldTransform supplies the POSITION --
    // there is deliberately none stored here, the same call SpriteRenderer makes
    // about size, and the same one Unity and Unreal make (a Camera component /
    // ACameraActor is placed BY its transform; it does not carry one).
    //
    // Before this existed the camera was host state driven through
    // Runtime::SetCamera -- a Sandbox-era artifact from when the physics showcase
    // owned its own camera class. That made the editor viewport and a standalone
    // runtime get their view from different places, so a scene could look correct
    // in the editor and render nothing in the game. A camera IS scene data.
    //
    // orthographicSize is the half-HEIGHT of the visible world in METERS -- the
    // resolution-INDEPENDENT unit (Unity's Camera.orthographicSize). Each host
    // derives zoom = viewportHalfHeight / orthographicSize per frame, so one scene
    // frames identically in a 720p runtime window and in a docked editor viewport
    // of any size. Storing a pixels-per-meter zoom instead would make every scene
    // wrong at any other window size.
    //
    // One scene, one active camera: the FIRST active one wins and >1 warns once,
    // exactly as PostProcess above resolves. `active` lets a scene keep alternates
    // (a cutscene angle, a debug wide shot) without deleting them.
    //
    // Task 5 (Phase 4): `projection` selects which of the two lens models below
    // applies. Orthographic is the default so every scene authored before this
    // field existed (every serialized Camera predating it) loads unchanged --
    // the 2D path (ActiveSceneCamera, SceneCamera.hpp) is gated on this default
    // and stays byte-identical. `fovYDegrees`/`nearZ`/`farZ` are Perspective-only
    // and, per the engine's MKS decision, `nearZ`/`farZ` are METERS (not an
    // arbitrary clip-space unit).
    enum class CameraProjection : std::uint8_t
    {
        Orthographic = 0,
        Perspective  = 1,
    };

    struct Camera
    {
        float orthographicSize = 5.0f;   // half-height of the view, in meters
        bool  active = true;
        CameraProjection projection = CameraProjection::Orthographic;
        float fovYDegrees = 60.0f;       // vertical field of view, degrees (Perspective only)
        float nearZ = 0.1f;              // meters (MKS); Perspective only
        float farZ  = 1000.0f;           // meters (MKS); Perspective only
    };

    // The editor-facing identity (Outliner arc): a STABLE Guid + display name.
    // Policy: the EDITOR adds this AT ENTITY CREATION ONLY (Edit::CreateEntity,
    // and scene load); runtime spawns are never forced to carry strings. A
    // rename is an EDIT of an existing one and never mints it -- an entity
    // without one has no durable identity to rename, so Edit::RenameEntity
    // refuses instead of adding. An entity without one displays as
    // "Entity <id>". The Guid is generated when the component is added and is
    // the durable cross-save identity -- entity ids are not.
    // Renamed from EntityInfo 2026-07-27: it IS the entity's durable identity,
    // the ECS equivalent of Unreal's intrinsic ActorLabel/ActorGuid, and the
    // surrounding comments already called it that ("identity is never minted
    // by rename", "structure-locked identity").
    struct Identity
    {
        Guid        id{};
        std::string name;
        // Non-trivially-copyable: the binary path (Play snapshots, scene
        // SaveBinary) serializes through this member instead of the POD
        // memcpy overload (Astra's HasSerializeMethod seam).
        template<typename Archive> void Serialize(Archive& ar) { ar(id); ar(name); }
    };

    // Marker ("tag component"): render submission skips entities carrying it
    // (the Outliner eye). Serialized like any component, so hidden stays
    // hidden in game; the eye applies it to an entity AND its descendants.
    struct Hidden {};
}

// Reflection blocks at namespace scope (NOT anonymous). WorldTransform::matrix is
// derived state -> Serializable(false) so name-keyed JSON skips it (recomputed on
// load; binary trivially-copyable path still round-trips it harmlessly).
namespace Arcane
{
    // NO Category on these three. The Inspector draws uncategorised fields
    // directly under the component header and each named category under a
    // collapsible sub-header, so a component whose every field shares one
    // category renders as "Transform > Transform > fields" -- a click and an
    // indent that separate nothing from anything. Categories earn their keep
    // where a component has several groups; SpriteRenderer below is that case.
    ASTRA_REFLECT_TYPE(Transform)
        ASTRA_REFLECT_FIELD(Transform, position)
            ASTRA_REFLECT_ATTR(Tooltip, "World position in meters (MKS units). The renderer applies no Y-flip, so +Y moves an entity DOWN on screen, not up.")
        // AngleFormat survives the widening to a quaternion unchanged, and
        // KEEPING it is what preserves the authoring experience. It was inert
        // on the float field this replaces (nothing read it -- InspectorMeta.hpp),
        // so the Inspector showed the stored radians raw. FieldKind::Quat DOES
        // read it (InspectorView.cpp's Quat row), and Radians is the value that
        // makes its Euler drags go on showing radians; dropping the attribute
        // would silently flip the Transform rotation row to degrees, which is a
        // UI change this task has no business making.
        ASTRA_REFLECT_FIELD(Transform, rotation)
            ASTRA_REFLECT_ATTR(AngleFormat, Astra::AngleFormat::Unit::Radians)
            ASTRA_REFLECT_ATTR(Tooltip, "Orientation, stored as a quaternion and edited as Euler angles about X/Y/Z. A 2D scene turns about Z only.")
        ASTRA_REFLECT_FIELD(Transform, scale)
    ASTRA_END_REFLECT_TYPE()

    // Hidden as well as Serializable(false): these are runtime-derived caches
    // (recomputed every frame / every fixed step), and the Inspector used to
    // advertise them as "unsupported" rows -- noise about state nobody authors.
    // Unity and UE both hide derived caches outright.
    ASTRA_REFLECT_TYPE(WorldTransform)
        ASTRA_REFLECT_FIELD(WorldTransform, matrix)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(Hidden)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(PreviousTransform)
        ASTRA_REFLECT_FIELD(PreviousTransform, position)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(Hidden)
        ASTRA_REFLECT_FIELD(PreviousTransform, rotation)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(AngleFormat, Astra::AngleFormat::Unit::Radians)
            ASTRA_REFLECT_ATTR(Hidden)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_ENUM(SpriteShape)
        ASTRA_REFLECT_ENUM_VALUE(SpriteShape, Rect)
        ASTRA_REFLECT_ENUM_VALUE(SpriteShape, Circle)
        ASTRA_REFLECT_ENUM_VALUE(SpriteShape, Capsule)
    ASTRA_END_REFLECT_ENUM()

    // Guid reflects as a nested struct (hi/lo scalars) so the reflection->JSON
    // path round-trips component asset references (SpriteRenderer::material).
    // Registered here, NOT in Core -- Core stays Astra-free.
    ASTRA_REFLECT_TYPE(Guid)
        ASTRA_REFLECT_FIELD(Guid, hi)
        ASTRA_REFLECT_FIELD(Guid, lo)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(SpriteRenderer)
        ASTRA_REFLECT_FIELD(SpriteRenderer, sprite)
            ASTRA_REFLECT_ATTR(Category, "Appearance")
            ASTRA_REFLECT_ATTR(Tooltip, "The .arcsprite asset this renderer draws. Nil renders an untextured 1x1 m tint quad scaled by the Transform.")
        ASTRA_REFLECT_FIELD(SpriteRenderer, tint)
            ASTRA_REFLECT_ATTR(Category, "Appearance")
        // Both are int32_t here but reach the batcher through
        // static_cast<uint16_t> (RenderSystems.hpp:104-105), which WRAPS rather
        // than clamps -- an authored -1 would sort as 65535, in front of
        // everything. The Range is the cast's own domain, and the Inspector
        // clamps to it on every path that writes these fields (drag, Ctrl+click
        // entry, and the multi-selection box), so an out-of-range value has to
        // come from somewhere other than the property panel.
        ASTRA_REFLECT_FIELD(SpriteRenderer, sortingLayer)
            ASTRA_REFLECT_ATTR(Category, "Sorting")
            ASTRA_REFLECT_ATTR(Range, 0.0, 65535.0, 1.0)
        ASTRA_REFLECT_FIELD(SpriteRenderer, orderInLayer)
            ASTRA_REFLECT_ATTR(Category, "Sorting")
            ASTRA_REFLECT_ATTR(Range, 0.0, 65535.0, 1.0)
        ASTRA_REFLECT_FIELD(SpriteRenderer, shape)
            ASTRA_REFLECT_ATTR(Category, "Shape")
        ASTRA_REFLECT_FIELD(SpriteRenderer, material)
            ASTRA_REFLECT_ATTR(Category, "Appearance")
    ASTRA_END_REFLECT_TYPE()

    // One field, so no Category -- see Transform above.
    ASTRA_REFLECT_TYPE(PostProcess)
        ASTRA_REFLECT_FIELD(PostProcess, material)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_ENUM(CameraProjection)
        ASTRA_REFLECT_ENUM_VALUE(CameraProjection, Orthographic)
        ASTRA_REFLECT_ENUM_VALUE(CameraProjection, Perspective)
    ASTRA_END_REFLECT_ENUM()

    // The Range floor on orthographicSize is deliberately above zero: a zero or
    // negative half-height has no view to derive, and ActiveSceneCamera treats
    // it as "no camera" rather than dividing by it. Field order matches the
    // struct's own (every other reflect block in this file does the same).
    // Categorized now that the component has two lens models worth of fields
    // (Task 5, Phase 4) -- `active`/`projection` are uncategorized (apply to
    // either lens), and each model's own fields group together so the
    // Inspector doesn't interleave a Perspective-only field between two
    // Orthographic-only ones.
    ASTRA_REFLECT_TYPE(Camera)
        ASTRA_REFLECT_FIELD(Camera, orthographicSize)
            ASTRA_REFLECT_ATTR(Category, "Orthographic")
            ASTRA_REFLECT_ATTR(Tooltip, "Half-height of the visible world, in meters. Resolution-independent: each host derives its zoom from the viewport height, so the framing holds at any window size.")
            ASTRA_REFLECT_ATTR(Range, 0.01, 10000.0, 0.1)
        ASTRA_REFLECT_FIELD(Camera, active)
            ASTRA_REFLECT_ATTR(Tooltip, "Only active cameras are considered; the first one found renders the scene.")
        ASTRA_REFLECT_FIELD(Camera, projection)
            ASTRA_REFLECT_ATTR(Tooltip, "Orthographic (the default) preserves every 2D scene. Perspective is the 3D lens (Phase 4's mesh slice).")
        ASTRA_REFLECT_FIELD(Camera, fovYDegrees)
            ASTRA_REFLECT_ATTR(Category, "Perspective")
            ASTRA_REFLECT_ATTR(Tooltip, "Vertical field of view, in degrees.")
            ASTRA_REFLECT_ATTR(Range, 1.0, 179.0, 1.0)
        ASTRA_REFLECT_FIELD(Camera, nearZ)
            ASTRA_REFLECT_ATTR(Category, "Perspective")
            ASTRA_REFLECT_ATTR(Tooltip, "Near clip plane distance, in meters (MKS).")
            ASTRA_REFLECT_ATTR(Range, 0.001, 10000.0, 0.01)
        ASTRA_REFLECT_FIELD(Camera, farZ)
            ASTRA_REFLECT_ATTR(Category, "Perspective")
            ASTRA_REFLECT_ATTR(Tooltip, "Far clip plane distance, in meters (MKS).")
            ASTRA_REFLECT_ATTR(Range, 0.01, 1000000.0, 1.0)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(Identity)
        ASTRA_REFLECT_FIELD(Identity, id)
            ASTRA_REFLECT_ATTR(ReadOnly)
        ASTRA_REFLECT_FIELD(Identity, name)
            ASTRA_REFLECT_ATTR(Tooltip, "The Outliner display name for this entity. If empty, the Outliner shows \"Entity <id>\" instead.")
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(Hidden)
    ASTRA_END_REFLECT_TYPE()
}

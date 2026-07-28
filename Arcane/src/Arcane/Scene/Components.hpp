#pragma once

// Scene components: plain reflected data. Every engine component is
// ASTRA_REFLECT-annotated from day one (design rule) so the editor/JSON/server
// path stays open at ~zero cost via the Astra 3.2 visitFields seam. Header-only;
// reflect blocks at namespace scope register once per module (MetaRegistry is
// idempotent), and the simulation Registry is owned by the host module.

#include <Arcane/Guid.hpp>

#include <Astra/Reflection/Reflection.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <string>

namespace Arcane
{
    struct Transform
    {
        glm::vec2 position{0.0f, 0.0f};
        float     rotation = 0.0f;          // radians
        glm::vec2 scale{1.0f, 1.0f};

        // 2D TRS in column-major homogeneous mat3 (translation in column 2).
        glm::mat3 ToMatrix() const
        {
            const float c = std::cos(rotation);
            const float s = std::sin(rotation);
            glm::mat3 m(1.0f);
            m[0] = glm::vec3(c * scale.x,  s * scale.x, 0.0f);
            m[1] = glm::vec3(-s * scale.y, c * scale.y, 0.0f);
            m[2] = glm::vec3(position.x,   position.y,  1.0f);
            return m;
        }
    };

    struct WorldTransform
    {
        glm::mat3 matrix{1.0f};             // computed by TransformPropagationSystem; never authored
    };

    // PreviousTransform (Epic 04.2): an entity's LOCAL pose at the previous fixed
    // step, captured by PhysicsSystem write-back before it overwrites Transform.
    // RenderSubmissionSystem draws at lerp(previous -> current, alpha) for smooth
    // slow-mo. Decomposed (position + angle) so rotation interpolates on the shortest
    // arc, NOT by lerping matrix components. Purely derived render state: an entity
    // opts into interpolation by carrying it; absent -> the sprite snaps to the latest
    // step (unchanged).
    struct PreviousTransform
    {
        glm::vec2 position{0.0f, 0.0f};
        float     rotation = 0.0f;          // radians
    };

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
        ASTRA_REFLECT_FIELD(Transform, rotation)
            ASTRA_REFLECT_ATTR(AngleFormat, Astra::AngleFormat::Unit::Radians)
            ASTRA_REFLECT_ATTR(Tooltip, "Rotation in radians.")
        ASTRA_REFLECT_FIELD(Transform, scale)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(WorldTransform)
        ASTRA_REFLECT_FIELD(WorldTransform, matrix)
            ASTRA_REFLECT_ATTR(Serializable, false)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(PreviousTransform)
        ASTRA_REFLECT_FIELD(PreviousTransform, position)
            ASTRA_REFLECT_ATTR(Serializable, false)
        ASTRA_REFLECT_FIELD(PreviousTransform, rotation)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(AngleFormat, Astra::AngleFormat::Unit::Radians)
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

    ASTRA_REFLECT_TYPE(Identity)
        ASTRA_REFLECT_FIELD(Identity, id)
            ASTRA_REFLECT_ATTR(ReadOnly)
        ASTRA_REFLECT_FIELD(Identity, name)
            ASTRA_REFLECT_ATTR(Tooltip, "The Outliner display name for this entity. If empty, the Outliner shows \"Entity <id>\" instead.")
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(Hidden)
    ASTRA_END_REFLECT_TYPE()
}

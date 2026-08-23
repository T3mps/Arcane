#pragma once

// Registry resources (singletons) for the scene slice. RenderContext2D,
// SpriteTable and SpriteMaterialTable are set by the host each frame; SceneRoot
// marks the subtree that IS the scene.

#include <Arcane/Guid.hpp>
#include <Arcane/Render/MeshBuilder.hpp>   // MeshData / MeshBounds -- MeshEntry's fields

#include <Astra/Entity/Entity.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    struct SceneRoot { Astra::Entity entity; };

    // ---- render interpolation (Epic 04.2) -----------------------------------
    // Blend a previous fixed-step pose toward the current one by RunLoop alpha so
    // slow-mo renders smoothly instead of snapping. Rotation MUST use AngleLerp
    // (shortest arc), not a matrix-component lerp.
    //
    // These two serve the PHYSICS-side InterpPose below, which stays a 2D pose
    // (glm::vec2 + a scalar angle) because Manifold2D is a 2D solver. The
    // COMPONENT-side pose (Arcane::PreviousTransform) went 3D in Task 3 (F1)
    // and blends through Arcane::LerpPose (Components.hpp), whose rotation half
    // is glm::slerp -- the same shortest-arc guarantee, one dimension up.
    [[nodiscard]] inline float Lerp(float a, float b, float t) noexcept
    {
        return a + (b - a) * t;
    }

    // Shortest-arc angle interpolation (radians): wrap the delta into (-pi, pi]
    // before blending, so 350deg->10deg travels +20deg through 0, not -340deg.
    [[nodiscard]] inline float AngleLerp(float a, float b, float t) noexcept
    {
        constexpr float kPi  = 3.14159265358979323846f;
        constexpr float kTau = 2.0f * kPi;
        float d = std::fmod(b - a, kTau);
        if (d < -kPi)      d += kTau;
        else if (d >  kPi) d -= kTau;
        return a + d * t;
    }

    // One body's previous fixed-step pose. `generation` mirrors the body handle's
    // generation so a recycled SoA slot (stale prev) is rejected by the consumer.
    struct InterpPose
    {
        glm::vec2     position{0.0f, 0.0f};
        float         angle      = 0.0f;   // radians
        std::uint32_t generation = 0;      // 0 == dead slot (never matches a live handle)
    };

    // Per-body previous-pose buffer, indexed by PhysicsWorld body SLOT index (the
    // same space DrawPhysicsDebug iterates). Populated by PhysicsSystem before each
    // world.Step(); read by DrawPhysicsDebug. Transient runtime state (Registry::Save
    // excludes resources; the no-op Serialize satisfies Astra's HasSerializeMethod so
    // the vector member does not hit the trivially-copyable path).
    struct PhysicsInterpBuffer
    {
        std::vector<InterpPose> prev;
        bool                    captured = false;   // false until the first capture

        template<typename Archive>
        void Serialize(Archive& /*ar*/) {}
    };

    struct RenderContext2D
    {
        // Camera transform applied by RenderSubmissionSystem AND DrawPhysicsDebug so
        // sprites + the physics-debug overlay pan/zoom together. CANONICAL form
        // (matches Sandbox::Camera::WorldToScreen): screen = world * zoom + offset.
        // Defaults (offset (0,0), zoom 1) are the identity transform.
        class Batcher2D* batcher = nullptr;   // set by the host between Begin and Drain
        glm::vec2        cameraOffset{0.0f, 0.0f};  // screen-space translation (canvas px)
        float            zoom = 1.0f;               // world->screen scale (1 == 1:1)
        float            alpha = 0.0f;              // RunLoop::Alpha() in [0,1); host-set each frame
    };

    // One .arcsprite asset, resolved for submission: the source texture's
    // asset Guid, plus ComputeSpriteGeom's output (SpriteAsset.hpp) -- UVs for
    // the pixel sub-rect and the world size in meters -- and the asset's
    // normalized pivot. Precomputed host-side so RenderSubmissionSystem never
    // touches the Assets facade or re-derives rect math per frame.
    struct SpriteEntry
    {
        glm::vec2 uvMin{0.0f, 0.0f};
        glm::vec2 uvMax{1.0f, 1.0f};
        glm::vec2 sizeMeters{1.0f, 1.0f};
        glm::vec2 pivot{0.5f, 0.5f};
        // The SOURCE TEXTURE's asset Guid (.arcsprite's `texture` field), and
        // the record's ONLY texture identity. Device-free by construction:
        // RenderSubmissionSystem hands it to Batcher2D::QuadTextured, it
        // travels into the drained span, and the NRI recorder resolves it
        // through NriTextureCache. Nil when the sprite declares no texture.
        Guid textureId{};
    };

    // Sprite-asset resolution: SpriteRenderer::sprite (an .arcsprite Guid) ->
    // the resolved record above. Same shape and lifetime rules as
    // SpriteMaterialTable below: the map is OWNED by the host (transient
    // pointer resource, set each frame, never serialized). Unresolved (nil /
    // absent / texture still loading) -> null, and submission falls back to a
    // 1x1 m tint quad, so a sprite always draws.
    struct SpriteTable
    {
        const std::unordered_map<Guid, SpriteEntry>* sprites = nullptr;

        const SpriteEntry* Resolve(const Guid& g) const
        {
            if (!sprites || !g.IsValid()) return nullptr;
            auto it = sprites->find(g);
            return it != sprites->end() ? &it->second : nullptr;
        }
    };

    // Sprite-material resolution (Slice 8): material asset Guid -> the id a
    // compiled material was registered under in the frame's Batcher2D. The map
    // is OWNED by the host's SpriteMaterialCache (transient pointer resource,
    // like RenderContext2D::batcher -- set each frame, never serialized).
    // Unresolved (absent / still compiling / failed) -> 0, the plain sprite
    // built-in, so a sprite always draws.
    struct SpriteMaterialTable
    {
        const std::unordered_map<Guid, uint16_t>* materials = nullptr;

        uint16_t Resolve(const Guid& g) const
        {
            if (!materials || !g.IsValid()) return 0;
            auto it = materials->find(g);
            return it != materials->end() ? it->second : 0;
        }
    };

    // One resolved .arcmesh (F2a, Task 4): OWNED geometry, generated once by
    // MeshCache::Request from BuildMeshData, plus its local-space bounds
    // (ComputeMeshBounds). MeshInstance::mesh (Render/Nri/nodes/MeshNode.hpp)
    // BORROWS a raw pointer into `data` for exactly the duration of one
    // RenderFrame call -- safe against a table rehash (std::unordered_map
    // never relocates an existing element's storage, only its iterators) but
    // NOT against an erase, which is why MeshCache::Invalidate/Clear are the
    // only things that may ever remove an entry mid-frame.
    //
    // `material` (F2a, Task 5) is a COPY of the loaded .arcmesh's own
    // `MeshAssetData::material` -- the mesh's default material Guid, the
    // second link in MeshSubmissionSystem's `materialOverride` -> mesh
    // default -> white chain. It rides along here because
    // MeshSubmissionSystem is host-published-resource-only by design (it
    // reads MeshTable/MeshMaterialTable and never touches a cache pointer,
    // matching RenderSubmissionSystem's rule of never touching the Assets
    // facade). It is also the ONLY part of the loaded MeshAssetData that
    // survives resolution at all: MeshCache keeps no copy of the asset (see
    // MeshCache.hpp's "WHAT IT DOES NOT KEEP"), so anything needing the
    // rest of it -- name, source, topology -- re-reads the .arcmesh, which
    // is what MeshDocument does.
    struct MeshEntry
    {
        MeshData   data;
        MeshBounds bounds;
        Guid       material{};
    };

    // .arcmesh Guid -> the resolved record above. Same shape and lifetime
    // rules as SpriteTable above: the map is OWNED by the host's MeshCache
    // (transient pointer resource, set each frame, never serialized).
    // Unresolved (nil / absent / failed to load or validate) -> null, and
    // MeshSubmissionSystem (Task 5) skips the entity entirely -- unlike a
    // sprite's 1x1 m untextured placeholder, there is no meaningful
    // placeholder mesh, so a broken reference draws nothing rather than the
    // wrong shape.
    struct MeshTable
    {
        const std::unordered_map<Guid, MeshEntry>* meshes = nullptr;

        const MeshEntry* Resolve(const Guid& g) const
        {
            if (!meshes || !g.IsValid()) return nullptr;
            auto it = meshes->find(g);
            return it != meshes->end() ? &it->second : nullptr;
        }
    };

    // One resolved mesh material (F2a, Task 4): CONSTANTS ONLY. Neither
    // MeshCache nor MeshMaterialCache may touch MaterialSource or
    // ShaderCompiler (that would open a second compile-drain site alongside
    // SceneRenderResolver's one, Host/SceneRenderResolver.hpp:22-28), so
    // there is no compiled pipeline riding along here -- just the value
    // MeshInstance::baseColor (Render/Nri/nodes/MeshNode.hpp) copies into its
    // per-instance root constant. Default (1,1,1,1) is exactly what a nil
    // material at the end of the materialOverride -> mesh-default chain
    // resolves to directly, with no lookup at all.
    struct ResolvedMeshMaterial
    {
        glm::vec4 baseColor{1.0f};
    };

    // .arcmat Guid -> the resolved record above. Same shape and lifetime
    // rules as MeshTable/SpriteTable.
    struct MeshMaterialTable
    {
        const std::unordered_map<Guid, ResolvedMeshMaterial>* materials = nullptr;

        const ResolvedMeshMaterial* Resolve(const Guid& g) const
        {
            if (!materials || !g.IsValid()) return nullptr;
            auto it = materials->find(g);
            return it != materials->end() ? &it->second : nullptr;
        }
    };
}

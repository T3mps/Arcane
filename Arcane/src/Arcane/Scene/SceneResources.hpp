#pragma once

// Registry resources (singletons) for the scene slice. RenderContext2D and
// TextureTable are set by the host each frame; SceneRoot marks the subtree that
// IS the scene.

#include <Arcane/Guid.hpp>

#include <Astra/Entity/Entity.hpp>

#include <nvrhi/nvrhi.h>

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
        class Batcher2D* batcher = nullptr;   // set by the host between Begin/End
        glm::vec2        cameraOffset{0.0f, 0.0f};  // screen-space translation (canvas px)
        float            zoom = 1.0f;               // world->screen scale (1 == 1:1)
        float            alpha = 0.0f;              // RunLoop::Alpha() in [0,1); host-set each frame
    };

    struct TextureTable
    {
        // textureId 0 is reserved for "untextured". Full Assets integration deferred.
        std::unordered_map<uint32_t, nvrhi::ITexture*> textures;

        nvrhi::ITexture* Resolve(uint32_t id) const
        {
            if (id == 0) return nullptr;
            auto it = textures.find(id);
            return it != textures.end() ? it->second : nullptr;
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
}

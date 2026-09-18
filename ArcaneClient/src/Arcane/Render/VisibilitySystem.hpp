#pragma once

// The CPU coarse visibility stage (F3, spec s4): one VisibleSet per view,
// linear over every WorldBounds. A registry RESOURCE (SceneVisibility)
// because RenderSubmissionSystem is an Astra::System and must read it; the
// host builds views[0] between the fixed and render schedulers
// (Sim/SystemSchedulers.hpp) from the viewport's ViewTransform.
//
// NO RESOURCE => NOTHING IS CULLED. Every consumer (the sprite sweep,
// CollectPickables, the GPU batch builder) reads MainVisibleSet() and treats
// nullptr as "draw everything" -- device-less hosts and every pre-F3 test keep
// their behaviour.
//
// THE SLACK: WorldBounds is the fixed-step pose; the sprite sweep renders a
// pose interpolated toward it (PhysicsInterpBuffer), so a fast sprite at the
// screen edge can sit a fraction of one step outside its box. The frustum is
// widened by kVisibilitySlack metres before every test -- conservative, one
// constant. Plan 2's GPU cull uses the SAME widened planes (VisibleSet::frustum).
//
// HEADER-ONLY, device-free (the CollectMeshInstances idiom): ArcaneTests
// drives it under ~[gpu]. No spatial structure -- the trigger is a measured
// BuildVisibleSet above 0.5 ms (spec s4).
#include <Arcane/Math/Aabb.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/Frustum.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace Arcane
{
    inline constexpr float kVisibilitySlack = 0.25f;

    struct VisibleEntry
    {
        Astra::Entity entity{};
        Aabb          box;
        float         nearDepth = 0.0f;   // view-space distance to the box's nearest point along -Z, clamped >= 0
    };

    struct VisibleSet
    {
        ViewTransform              view;
        Frustum                    frustum;   // already Widened(kVisibilitySlack)
        std::vector<VisibleEntry>  entries;
        std::vector<std::uint64_t> members;   // bitset by entity index

        [[nodiscard]] bool Contains(Astra::Entity e) const noexcept
        {
            const std::uint32_t idx = static_cast<std::uint32_t>(e.GetID());
            const std::size_t word = idx / 64u;
            return word < members.size() && ((members[word] >> (idx % 64u)) & 1ull);
        }

        void Clear()
        {
            entries.clear();
            std::fill(members.begin(), members.end(), 0ull);
        }

        void Insert(Astra::Entity e, const Aabb& box, float nearDepth)
        {
            const std::uint32_t idx = static_cast<std::uint32_t>(e.GetID());
            const std::size_t word = idx / 64u;
            if (word >= members.size())
                members.resize(word + 1, 0ull);
            members[word] |= (1ull << (idx % 64u));
            entries.push_back(VisibleEntry{ e, box, nearDepth });
        }
    };

    struct SceneVisibility
    {
        std::vector<VisibleSet> views;   // index 0 = the main view; shadow views / previews append (later arcs)

        // Transient, never serialized (Registry::Save excludes resources
        // entirely regardless); the no-op Serialize keeps this off Astra's
        // reflected/trivially-copyable auto-serialization path, the same
        // reason BoundsSystemState (BoundsSystem.hpp) carries one.
        template<typename Archive> void Serialize(Archive& /*ar*/) {}
    };

    [[nodiscard]] inline const VisibleSet* MainVisibleSet(const Astra::Registry& reg) noexcept
    {
        const SceneVisibility* sv = reg.GetResource<SceneVisibility>();
        return (sv && !sv->views.empty()) ? &sv->views[0] : nullptr;
    }

    // View-space depth of the box's nearest point: the eight corners through
    // `view`, the largest z (nearest, since the camera looks down -Z),
    // negated and clamped at 0 (a box that straddles the eye is "at" it).
    [[nodiscard]] inline float NearViewDepth(const Aabb& box, const glm::mat4& view) noexcept
    {
        float nearest = -std::numeric_limits<float>::infinity();
        for (int i = 0; i < 8; ++i)
        {
            const glm::vec3 c((i & 1) ? box.max.x : box.min.x,
                              (i & 2) ? box.max.y : box.min.y,
                              (i & 4) ? box.max.z : box.min.z);
            nearest = std::max(nearest, (view * glm::vec4(c, 1.0f)).z);
        }
        return std::max(0.0f, -nearest);
    }

    inline void BuildVisibleSet(Astra::Registry& reg, const ViewTransform& view, VisibleSet& out)
    {
        out.Clear();
        out.view    = view;
        out.frustum = Frustum::From(view).Widened(kVisibilitySlack);
        reg.CreateView<const WorldBounds, Astra::Not<Hidden>>().ForEach(
            [&](Astra::Entity e, const WorldBounds& wb)
            {
                if (out.frustum.Contains(wb.box))
                    out.Insert(e, wb.box, NearViewDepth(wb.box, view.view));
            });
    }
}

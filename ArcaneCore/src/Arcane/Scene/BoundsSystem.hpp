#pragma once

// BoundsSystem -- writes WorldBounds for every drawable (F3, spec s2.3).
// fixedUpdate, After<TransformPropagationSystem> (it reads the composed
// WorldTransform); the editor's EditModeSchedule runs the same pair.
//
// DIRTY-DRIVEN, the TransformSystems idiom: the rows visited are the union of
// Changed<WorldTransform> / Changed<MeshRenderer> / Changed<SpriteRenderer>
// since the last run (WorldTransform and MeshRenderer are change-tracked --
// exact per entity; SpriteRenderer is chunk-coarse, which only recomputes a
// few boxes), every drawable with no WorldBounds yet, and the removal
// reconciliation. A static scene does no work.
//
// Hidden is NOT consulted: it is a draw decision (the sweeps skip it), not a
// bounds one -- the editor frames what exists, and a hidden entity unhidden
// next frame must already have its box.
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/SpriteGeometry.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <glm/glm.hpp>

#include <optional>
#include <utility>
#include <vector>

namespace Arcane
{
    // A sprite is a zero-thickness quad; a zero-extent axis is a degenerate
    // frustum input, so the box gets a millimetre of Z -- and ONLY Z (spec
    // 2026-09-18-f3-visibility-and-gpu-scene-design s2.3: "Z widened by
    // kSpriteDepthEpsilon"). X/Y stay the exact SpriteWorldQuad extents: the
    // editor's framing reads this box, and a uniform Widened() moved the
    // boot-framed camera sub-pixel and broke both editor golden lanes (R-E).
    inline constexpr float kSpriteDepthEpsilon = 0.001f;

    // The last-run tick, a registry resource like TransformOrder (both hosts'
    // schedulers own a system instance; the registry is the shared lifetime).
    struct BoundsSystemState
    {
        Astra::Tick lastRun = 0;
        std::vector<Astra::Entity> scratch;

        // Transient derived state; Registry::Save excludes resources entirely.
        // The no-op Serialize keeps the vector member off Astra's trivially-
        // copyable path (TransformOrder carries one for the same reason).
        template<typename Archive> void Serialize(Archive& /*ar*/) {}
    };

    struct BoundsSystem
        : Astra::SystemTraits<Astra::Reads<WorldTransform, MeshRenderer, SpriteRenderer, Hidden>,
                              Astra::Writes<WorldBounds>,
                              Astra::After<TransformPropagationSystem>>
    {
        // The one rule for a drawable's world box; nullopt = not drawable.
        //
        // The box covers EVERYTHING the entity paints. The sprite pass draws any
        // <WorldTransform, SpriteRenderer> row and the mesh pass any
        // <WorldTransform, MeshRenderer> row (neither excludes the other), so an
        // entity carrying both renderers draws both, and its box is the UNION of
        // the mesh box and the sprite box (review round 1). "Mesh beats sprite"
        // is the PICK rule -- which drawable to report for a hit -- not a
        // coverage rule; a visible set or GPU row culled on a mesh-only box
        // would drop the sprite half wrongly. An unresolved mesh contributes
        // nothing (it draws nothing), so mesh-only-and-unresolved is still
        // nullopt and mesh-unresolved-plus-sprite is the sprite box alone.
        [[nodiscard]] static std::optional<Aabb> WorldBoxFor(const Astra::Registry& reg, Astra::Entity e,
                                                             const WorldTransform& world,
                                                             const MeshTable* meshes, const SpriteTable* sprites)
        {
            std::optional<Aabb> box;
            if (const MeshRenderer* mr = reg.GetComponent<MeshRenderer>(e))
            {
                const MeshEntry* entry = meshes ? meshes->Resolve(mr->mesh) : nullptr;
                if (entry)
                    box = entry->bounds.Transformed(world.matrix);
            }
            if (const SpriteRenderer* sr = reg.GetComponent<SpriteRenderer>(e))
            {
                const SpriteEntry* entry =
                    (sr->shape == SpriteShape::Rect && sprites) ? sprites->Resolve(sr->sprite) : nullptr;
                const SpriteQuad q = SpriteWorldQuad(world.matrix,
                                                     entry ? entry->sizeMeters : glm::vec2(1.0f),
                                                     entry ? entry->pivot      : glm::vec2(0.5f));
                Aabb sprite = Aabb::FromPoints(q.corners);
                sprite.min.z -= kSpriteDepthEpsilon;   // Z only (spec s2.3); X/Y are the exact corners
                sprite.max.z += kSpriteDepthEpsilon;
                box = box ? box->Union(sprite) : sprite;
            }
            return box;
        }

        void operator()(Astra::Registry& reg)
        {
            BoundsSystemState* state = reg.GetResource<BoundsSystemState>();
            if (!state)
                state = reg.EmplaceResource<BoundsSystemState>();
            if (!state) return;
            const MeshTable*   meshes  = reg.GetResource<MeshTable>();
            const SpriteTable* sprites = reg.GetResource<SpriteTable>();

            std::vector<Astra::Entity>& todo = state->scratch;
            todo.clear();

            // 1. Changed since last run (the three producers), 2. drawables with
            //    no box yet (first sight, or a registry just loaded).
            const Astra::Tick since = state->lastRun;
            reg.CreateView<const WorldTransform, Astra::Changed<WorldTransform>>().Since(since)
                .ForEach([&](Astra::Entity e, const WorldTransform&) { todo.push_back(e); });
            reg.CreateView<const WorldTransform, const MeshRenderer, Astra::Changed<MeshRenderer>>().Since(since)
                .ForEach([&](Astra::Entity e, const WorldTransform&, const MeshRenderer&) { todo.push_back(e); });
            reg.CreateView<const WorldTransform, const SpriteRenderer, Astra::Changed<SpriteRenderer>>().Since(since)
                .ForEach([&](Astra::Entity e, const WorldTransform&, const SpriteRenderer&) { todo.push_back(e); });
            reg.CreateView<const WorldTransform, const MeshRenderer, Astra::Not<WorldBounds>>()
                .ForEach([&](Astra::Entity e, const WorldTransform&, const MeshRenderer&) { todo.push_back(e); });
            reg.CreateView<const WorldTransform, const SpriteRenderer, Astra::Not<WorldBounds>>()
                .ForEach([&](Astra::Entity e, const WorldTransform&, const SpriteRenderer&) { todo.push_back(e); });

            // 3. Removal reconciliation: a box whose entity no longer draws.
            reg.CreateView<const WorldBounds, Astra::Not<MeshRenderer>, Astra::Not<SpriteRenderer>>()
                .ForEach([&](Astra::Entity e, const WorldBounds&) { todo.push_back(e); });

            // `todo` may hold an entity twice (moved AND first-seen); the loop is
            // idempotent, so no dedup.
            for (Astra::Entity e : todo)
            {
                const WorldTransform* world = std::as_const(reg).GetComponent<WorldTransform>(e);
                const std::optional<Aabb> box =
                    world ? WorldBoxFor(std::as_const(reg), e, *world, meshes, sprites) : std::nullopt;
                if (!box)
                {
                    if (reg.HasComponent<WorldBounds>(e))
                        reg.RemoveComponent<WorldBounds>(e);
                    continue;
                }
                if (WorldBounds* wb = reg.GetComponent<WorldBounds>(e))   // non-const: stamps the change
                    wb->box = *box;
                else
                    reg.AddComponent<WorldBounds>(e, WorldBounds{ *box });
            }

            state->lastRun = reg.CurrentTick();
            reg.AdvanceTick();
        }

        static constexpr bool RequiresExclusive = true;   // it advances the tick (TransformPropagationSystem's contract)
    };
}

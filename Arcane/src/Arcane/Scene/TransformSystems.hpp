#pragma once

// TransformPropagationSystem: BFS world-matrix propagation over the scene root's
// subtree. Correctness relies on BFS pre-order (parent before child) guaranteed
// by Astra's RelationshipGraph TraversalCache. The root is computed first (no
// parent); every descendant reads its already-computed parent's world matrix.

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <vector>

namespace Arcane
{
    struct TransformPropagationSystem
        : Astra::SystemTraits<Astra::Reads<Transform>, Astra::Writes<WorldTransform>>
    {
        void operator()(Astra::Registry& reg)
        {
            const SceneRoot* sceneRoot = reg.GetResource<SceneRoot>();
            if (!sceneRoot) return;
            const Astra::Entity root = sceneRoot->entity;

            // WorldTransform is derived, never authored: an entity that reaches
            // this subtree with a Transform but no WorldTransform yet (a node
            // Edit::CreateEntity just created, or a SceneRoot minted by
            // SceneAsset::CreateEmpty, or one loaded from a pre-fix .arcscene)
            // must get one here, or it can never satisfy RenderSubmissionSystem's
            // view no matter what components get added to it afterward.
            //
            // Collected into a vector and added only AFTER this walk finishes:
            // Registry::AddComponent moves the entity to a different archetype,
            // which would invalidate the Transform/WorldTransform pointers this
            // same walk is handing out for OTHER entities if done inline. Astra's
            // ForEachDescendant only snapshots the entity-id list itself (see
            // Relations::ForEachDescendant in Astra), not per-entity component
            // storage, so mutating structure mid-callback is exactly the hazard
            // it does not protect against.
            std::vector<Astra::Entity> needsWorld;
            if (reg.GetComponent<Transform>(root) && !reg.GetComponent<WorldTransform>(root))
                needsWorld.push_back(root);
            reg.GetRelations(root).ForEachDescendant(
                [&](Astra::Entity e, size_t /*depth*/)
                {
                    if (reg.GetComponent<Transform>(e) && !reg.GetComponent<WorldTransform>(e))
                        needsWorld.push_back(e);
                });
            for (Astra::Entity e : needsWorld)
                reg.AddComponent<WorldTransform>(e, WorldTransform{});

            // Root: world = local (no parent).
            if (auto* rootLocal = reg.GetComponent<Transform>(root))
                if (auto* rootWorld = reg.GetComponent<WorldTransform>(root))
                    rootWorld->matrix = rootLocal->ToMatrix();

            reg.GetRelations(root).ForEachDescendant(
                [&](Astra::Entity e, size_t /*depth*/)
                {
                    auto* local = reg.GetComponent<Transform>(e);
                    auto* world = reg.GetComponent<WorldTransform>(e);
                    if (!local || !world) return;   // skip non-spatial nodes

                    const Astra::Entity parent = reg.GetParent(e);
                    const WorldTransform* parentWorld = reg.GetComponent<WorldTransform>(parent);
                    const glm::mat3 parentMat = parentWorld ? parentWorld->matrix : glm::mat3(1.0f);
                    world->matrix = parentMat * local->ToMatrix();
                });
        }
    };
}

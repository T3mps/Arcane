#pragma once

// TransformPropagationSystem: BFS world-matrix propagation over the scene root's
// subtree. Correctness relies on BFS pre-order (parent before child) guaranteed
// by Astra's RelationshipGraph TraversalCache. The root is computed first (no
// parent); every descendant reads its already-computed parent's world matrix.

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

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

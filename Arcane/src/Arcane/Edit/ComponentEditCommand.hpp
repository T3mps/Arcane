#pragma once

// Generic reflection-based component edit command: a whole-component before/
// after byte snapshot (via the Astra descriptor serialize seam). Undo/Redo
// re-resolve the LIVE component by (Entity, descriptor->hash) so it survives
// archetype moves and no-ops if the entity/component is gone. Restoring a
// LocalTransform reflects to the physics body via SPEC #1's polling reconcile.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Edit/Command.hpp>

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std::function/vector/string members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API ComponentEditCommand final : public ICommand
    {
    public:
        // `resolve` returns the CURRENT live registry each call -- the registry
        // object itself can be swapped out from under a long-lived command (e.g.
        // Runtime::RestoreRegistry/ResetRegistry on Play/Stop/hot-reload), so a
        // cached Registry& would dangle. Callers (CommandStack) pass a lambda
        // that reads through their own indirection (e.g. Runtime::Registry()).
        ComponentEditCommand(std::function<Astra::Registry&()> resolve, Astra::Entity entity,
                             const Astra::ComponentDescriptor* descriptor,
                             std::vector<std::byte> before,
                             std::vector<std::byte> after,
                             std::string label);

        void Undo() override;   // deserialize `before` into the live component
        void Redo() override;   // deserialize `after`
        const char* Label() const override { return m_label.c_str(); }

        // Serialize the live component for (registry, entity, descriptor) to a
        // blob. Empty if the entity/component is not present.
        static std::vector<std::byte> Snapshot(Astra::Registry& registry,
                                               Astra::Entity entity,
                                               const Astra::ComponentDescriptor* descriptor);

    private:
        void Restore(const std::vector<std::byte>& blob);

        std::function<Astra::Registry&()>  m_resolve;
        Astra::Entity                      m_entity;
        const Astra::ComponentDescriptor*  m_descriptor;
        std::vector<std::byte>             m_before;
        std::vector<std::byte>             m_after;
        std::string                        m_label;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

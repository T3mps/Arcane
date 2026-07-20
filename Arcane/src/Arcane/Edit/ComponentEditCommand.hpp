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
#include <string>
#include <vector>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane
{
    class ARCANE_API ComponentEditCommand final : public ICommand
    {
    public:
        ComponentEditCommand(Astra::Registry& registry, Astra::Entity entity,
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

        Astra::Registry&                   m_registry;
        Astra::Entity                      m_entity;
        const Astra::ComponentDescriptor*  m_descriptor;
        std::vector<std::byte>             m_before;
        std::vector<std::byte>             m_after;
        std::string                        m_label;
    };
}

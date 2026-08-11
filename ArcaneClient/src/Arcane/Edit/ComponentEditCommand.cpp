#include <Arcane/Edit/ComponentEditCommand.hpp>

#include <Astra/Component/Component.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <span>
#include <utility>

namespace Arcane
{
    ComponentEditCommand::ComponentEditCommand(std::function<Astra::Registry&()> resolve, Astra::Entity entity,
                                               const Astra::ComponentDescriptor* descriptor,
                                               std::vector<std::byte> before,
                                               std::vector<std::byte> after,
                                               std::string label)
        : m_resolve(std::move(resolve)), m_entity(entity), m_descriptor(descriptor),
          m_before(std::move(before)), m_after(std::move(after)), m_label(std::move(label))
    {
    }

    std::vector<std::byte> ComponentEditCommand::Snapshot(Astra::Registry& registry,
                                                          Astra::Entity entity,
                                                          const Astra::ComponentDescriptor* descriptor)
    {
        std::vector<std::byte> blob;
        if (!descriptor || !descriptor->serialize)
            return blob;
        void* instance = registry.GetComponentByHash(entity, descriptor->hash);
        if (!instance)
            return blob;
        {
            Astra::BinaryWriter writer(blob, /*reserveSize*/ 256);
            descriptor->serialize(writer, instance);
        } // writer dtor flushes into `blob`
        return blob;
    }

    void ComponentEditCommand::Restore(const std::vector<std::byte>& blob)
    {
        if (!m_descriptor || !m_descriptor->deserialize || blob.empty())
            return;
        void* instance = m_resolve().GetComponentByHash(m_entity, m_descriptor->hash);
        if (!instance)
            return;   // entity/component gone -> safe no-op
        Astra::BinaryReader reader{std::span<const std::byte>(blob)};
        m_descriptor->deserialize(reader, instance);
    }

    void ComponentEditCommand::Undo() { Restore(m_before); }
    void ComponentEditCommand::Redo() { Restore(m_after); }
}

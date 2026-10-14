#pragma once

// Settings arc S6-12: a good ProtocolLoader::Load layers the file's settings
// onto net.* at the Project rung, tagged "protocol.json". DropProtocolLayer()
// removes those records (the whole Project rung of each affected net.* cvar)
// so no other case in the random-order run sees one file's net.* values;
// ProtocolLayerReset does it when a case ends.

#include <Arcane/Config/CVarRegistry.hpp>

namespace Arcane::Test
{
    inline void DropProtocolLayer()
    {
        CVarRegistry& reg = CVarRegistry::Get();
        bool cleared = false;
        for (const CVarListEntry& entry : reg.List())
        {
            if (!entry.name.starts_with("net.")) continue;
            const auto e = reg.Explain(entry.name);
            if (!e) continue;
            for (const CVarHistoryRecord& r : e->history)
                if (r.by == SetBy::Project && r.module == "protocol.json")
                {
                    cleared |= reg.ClearRung(reg.Find(entry.name), SetBy::Project);
                    break;
                }
        }
        if (cleared) reg.PublishImmediate();
    }

    struct ProtocolLayerReset
    {
        ProtocolLayerReset() = default;
        ProtocolLayerReset(const ProtocolLayerReset&) = delete;
        ProtocolLayerReset& operator=(const ProtocolLayerReset&) = delete;
        ~ProtocolLayerReset() { DropProtocolLayer(); }
    };
}

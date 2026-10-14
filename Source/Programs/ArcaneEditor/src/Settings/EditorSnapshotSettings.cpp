#include "Settings/EditorSnapshotSettings.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarRegistry.hpp>

ARC_SETTINGS(Arcane::Editor::AstraSnapshotSettings);

namespace Arcane::Editor
{
    void ApplySnapshotSettings(const AstraSnapshotSettings& s)
    {
        Runtime::SetSnapshotSaveConfig(ToAstraSaveConfig(s));
    }

    namespace
    {
        void OnSnapshotCompressionPublished(CVarHandle, void*)
        {
            ApplySnapshotSettings(Settings<AstraSnapshotSettings>());   // the block is swapped in before callbacks run
        }

        // ARC_SETTINGS above registered astra.snapshot.compression in this TU
        // (ordered), so it exists here. Every later rung (EditorUser, project,
        // --set, the settings window) reaches Core through a Publish that
        // changed the value. Dist refuses the Dev cvar: no callback, and the
        // snapshot keeps Core's default (Astra's SaveConfig{}), as before.
        const bool s_snapshotCallback = [] {
            CVarRegistry& reg = CVarRegistry::Get();
            const CVarHandle h = reg.Find("astra.snapshot.compression");
            if (!h.IsStale())
                reg.AddCallback(h, &OnSnapshotCompressionPublished, nullptr);
            return true;
        }();
    }
}

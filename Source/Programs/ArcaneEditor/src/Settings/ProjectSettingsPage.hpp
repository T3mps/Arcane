#pragma once

// The Project Settings "Project" page (settings arc S3-12, spec s6.1, s6.6,
// s16.7): the .arcproj identity (read-only), the boot scene, and the
// gameplay input asset -- the selector the old Project Settings window
// carried (EditorPanels.cpp:2975-3036 on main). It edits the project FILE,
// not cvars, and says so in its header. EditorApp applies the requests.

#include <Arcane/Guid.hpp>

#include <optional>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class PropertyGrid;
    struct AssetRefServices;

    struct ProjectSettingsRequests
    {
        Guid selection;                  // the picked gameplay input asset (with select)
        bool select = false;
        bool clear  = false;             // clear the gameplay input asset
        bool open   = false;             // open the gameplay input asset's document
        bool create = false;             // Assets > Create > Input Actions...
        std::optional<Guid> bootScene;   // set the boot scene (Nil clears)
    };

    void DrawProjectIdentityPage(const Arcane::Project* project, PropertyGrid& grid,
                                 const AssetRefServices* assetRefs, ProjectSettingsRequests& requests);
}

#pragma once

// The editor's title text, composed in ONE place (node page phase s6.4). The
// OS window title and the toolbar strip's right-side status both format the
// same TitleParts, which EditorApp::CurrentTitleParts() alone assembles.
// Header-only and pure (std only; no Project or SceneSession includes):
// BuildInfo and the backend name are passed in, so ArcaneTests links nothing
// to test it. An empty `project` is a real, reachable session state.

#include <string>
#include <string_view>

namespace Arcane::Editor
{
    struct TitleParts
    {
        std::string project;          // Manifest().name; empty = no project open
        std::string scene;            // SceneSession::DisplayName(): "Untitled" or the file stem
        bool        sceneDirty = false;
    };

    // "<project> - <scene>* - <buildInfo> <<backend>>". An empty part drops
    // out with its separator; the * is File -> Save's unsaved marker and only
    // follows a scene. BuildInfo is the DLL's own "<version> [Debug|Release|Dist]"
    // (Engine.cpp), so the title reports the engine actually loaded.
    [[nodiscard]] inline std::string FormatOsTitle(const TitleParts& parts, std::string_view buildInfo,
                                                   std::string_view backend)
    {
        std::string title = parts.project;
        if (!parts.scene.empty())
        {
            if (!title.empty()) title += " - ";
            title += parts.scene;
            if (parts.sceneDirty) title += "*";
        }
        if (!title.empty()) title += " - ";
        title += buildInfo;
        if (!backend.empty())
        {
            title += " <";
            title += backend;
            title += ">";
        }
        return title;
    }

    // The strip's plain text (also its tooltip's first line): "<project> > <scene>",
    // plus " *" when dirty; "No project" alone when `project` is empty.
    [[nodiscard]] inline std::string FormatStripStatus(const TitleParts& parts)
    {
        if (parts.project.empty()) return "No project";
        std::string s = parts.project + " > " + parts.scene;
        if (parts.sceneDirty) s += " *";
        return s;
    }
}

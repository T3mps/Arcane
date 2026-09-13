#pragma once

// ClassTemplates: the PURE text half of Assets -> Create -> C++ Class (the
// third step of the editor<->IDE surface; Unreal's GameProjectUtils::
// AddCodeToProject in miniature). Given a template kind, a class name and the
// project's name, produce the header (and, for the kinds that have one, the
// source) as strings. Nothing here touches a filesystem, ImGui, or the
// engine -- EditorApp::MintCppClass writes the files, registers them under
// source://, regenerates the solution and opens the .cpp in Visual Studio.
//
// The three kinds and what they emit:
//   Component  -- a reflected struct (ASTRA_REFLECT_TYPE/FIELD, the shape
//                 Components.hpp and HotReloadShared.hpp use) in the header,
//                 and a .cpp carrying the ONE ARCANE_COMPONENT(ns::T) line
//                 (Arcane/Plugin/GameComponents.hpp), so the type is live
//                 after one Rebuild Game Module with no edit to Init.
//   System     -- a header-only Astra::SystemTraits functor (engine systems
//                 are header-only too) with the paste-ready AddSystem line
//                 for GamePlugin_Init in its comment: systems stay EXPLICIT
//                 because their scheduler order is a design act.
//   PlainClass -- a class in the project namespace with its own .cpp.
//
// Templates are embedded here rather than shipped as data files (UE's
// Engine/Content/Editor/Templates/*.template): three short texts, testable
// as strings, and nothing yet asks to edit them outside the engine.

#include <optional>
#include <string>
#include <string_view>

namespace Arcane::Editor::ClassTemplates
{
    enum class Kind : int { Component = 0, System, PlainClass, Count };

    [[nodiscard]] const char* KindLabel(Kind kind);   // "Component" / "System" / "Plain class"

    // nullopt when `name` is a usable C++ identifier for a type; otherwise the
    // reason, in the words the dialog shows. Refuses: empty, a leading digit,
    // any character outside [A-Za-z0-9_], and C++ keywords. Qualification
    // ("A::B") is refused too -- the project namespace is applied by Render.
    [[nodiscard]] std::optional<std::string> ValidateClassName(std::string_view name);

    // The manifest name is free text ("My Game"); the namespace it becomes must
    // be an identifier: every other character -> '_', a leading digit gets a
    // '_' prefix, and an empty result falls back to "Game".
    [[nodiscard]] std::string NamespaceForProject(std::string_view projectName);

    struct Rendered
    {
        std::string headerName;   // "<Class>.hpp"
        std::string header;
        std::string sourceName;   // "<Class>.cpp", or empty for a header-only kind
        std::string source;       // empty for a header-only kind
    };

    // Render `kind` for `className` (already validated) in `projectName`'s
    // namespace. Every text ends in a newline and carries no template token.
    [[nodiscard]] Rendered Render(Kind kind, std::string_view className, std::string_view projectName);
}

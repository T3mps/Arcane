#pragma once

// Which Problems rows route, and how (node-page phase s8.2). Pure: the host
// passes facts as callbacks, so the panel stays free of EditorApp and the
// rules are unit-tested with fakes. A row that cannot route is drawn as plain
// text, never a hover highlight that does nothing.

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Guid.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace Arcane::Editor
{
    struct RouteFacts
    {
        std::function<bool(std::uint64_t)>                               entityAlive;
        std::function<std::optional<std::filesystem::path>(const Guid&)> resolveAsset;
        std::function<bool(const std::filesystem::path&)>                hasDocumentFactory;
        std::function<bool(const std::filesystem::path&)>                isDirectory;
        std::function<bool(const std::filesystem::path&)>                exists;
    };

    enum class RouteAction : std::uint8_t { None, SelectEntity, OpenDocument, RevealAsset,
                                            OpenGraphNode, ShowInExplorer, OpenAsText };

    // Entity: SelectEntity when alive. Asset: None when unresolved, OpenDocument
    // with a factory, else RevealAsset. GraphNode: OpenGraphNode when the owner
    // resolves. File: None when missing; ShowInExplorer for a directory or a
    // .dll/.exe; OpenDocument with a factory; else OpenAsText. A missing
    // callback reads as "no".
    [[nodiscard]] RouteAction ClassifyLocator(const DiagLocator& l, const RouteFacts& f);
    [[nodiscard]] inline bool IsRoutable(const DiagLocator& l, const RouteFacts& f)
    { return ClassifyLocator(l, f) != RouteAction::None; }

    // The path behind a File / Asset / GraphNode (owner) row; nullopt otherwise or unresolved.
    [[nodiscard]] std::optional<std::filesystem::path> LocatorPath(const DiagLocator& l, const RouteFacts& f);
}

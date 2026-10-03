#include <Panels/LocatorRoute.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace Arcane::Editor
{
    namespace
    {
        std::optional<std::filesystem::path> Resolve(const RouteFacts& f, const Guid& g)
        {
            return f.resolveAsset ? f.resolveAsset(g) : std::nullopt;
        }
        bool Call(const std::function<bool(const std::filesystem::path&)>& fn, const std::filesystem::path& p)
        {
            return fn && fn(p);
        }
    }

    RouteAction ClassifyLocator(const DiagLocator& l, const RouteFacts& f)
    {
        using Kind = DiagLocator::Kind;
        switch (l.kind)
        {
            case Kind::Entity:
                return f.entityAlive && f.entityAlive(l.entity) ? RouteAction::SelectEntity : RouteAction::None;
            case Kind::Asset:
            {
                const auto path = Resolve(f, l.asset);
                if (!path) return RouteAction::None;
                return Call(f.hasDocumentFactory, *path) ? RouteAction::OpenDocument : RouteAction::RevealAsset;
            }
            case Kind::GraphNode:
                return Resolve(f, l.ownerAsset) ? RouteAction::OpenGraphNode : RouteAction::None;
            case Kind::File:
            {
                if (l.file.empty()) return RouteAction::None;
                const std::filesystem::path p(l.file);
                if (!Call(f.exists, p)) return RouteAction::None;
                if (Call(f.isDirectory, p)) return RouteAction::ShowInExplorer;
                std::string ext = p.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".dll" || ext == ".exe") return RouteAction::ShowInExplorer;
                return Call(f.hasDocumentFactory, p) ? RouteAction::OpenDocument : RouteAction::OpenAsText;
            }
            case Kind::None:
                break;
        }
        return RouteAction::None;
    }

    std::optional<std::filesystem::path> LocatorPath(const DiagLocator& l, const RouteFacts& f)
    {
        switch (l.kind)
        {
            case DiagLocator::Kind::File:      if (!l.file.empty()) return std::filesystem::path(l.file); break;
            case DiagLocator::Kind::Asset:     return Resolve(f, l.asset);
            case DiagLocator::Kind::GraphNode: return Resolve(f, l.ownerAsset);
            default: break;
        }
        return std::nullopt;
    }
}

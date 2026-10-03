// LocatorRoute (node-page phase s8.2): which Problems rows route, and how --
// pure, with fake facts.
#include <catch2/catch_test_macros.hpp>
#include <Panels/LocatorRoute.hpp>

using namespace Arcane::Editor;
using Arcane::DiagLocator;

namespace
{
    const Arcane::Guid kMat   = *Arcane::Guid::FromString("7e5a0012-0012-4012-8012-000000000012");
    const Arcane::Guid kTex   = *Arcane::Guid::FromString("7e5a0012-0012-4012-8012-000000000013");
    const Arcane::Guid kGone  = *Arcane::Guid::FromString("7e5a0012-0012-4012-8012-000000000014");

    RouteFacts Facts()
    {
        RouteFacts f;
        f.entityAlive = [](std::uint64_t id) { return id == 7; };
        f.resolveAsset = [](const Arcane::Guid& g) -> std::optional<std::filesystem::path>
        {
            if (g == kMat) return std::filesystem::path("D:/p/Content/a.arcmat");
            if (g == kTex) return std::filesystem::path("D:/p/Content/t.png");
            return std::nullopt;
        };
        f.hasDocumentFactory = [](const std::filesystem::path& p) { return p.extension() == ".arcmat" || p.extension() == ".arcinput"; };
        f.isDirectory = [](const std::filesystem::path& p) { return p == std::filesystem::path("D:/p/Plugins"); };
        f.exists = [](const std::filesystem::path& p) { return p.generic_string().find("missing") == std::string::npos; };
        return f;
    }
}

TEST_CASE("ClassifyLocator: every kind and action", "[editor][diagnostics]")
{
    const RouteFacts f = Facts();
    CHECK(ClassifyLocator(DiagLocator{}, f) == RouteAction::None);
    CHECK(ClassifyLocator(DiagLocator::Entity(7), f) == RouteAction::SelectEntity);
    CHECK(ClassifyLocator(DiagLocator::Entity(8), f) == RouteAction::None);
    CHECK(ClassifyLocator(DiagLocator::Asset(kMat), f) == RouteAction::OpenDocument);
    CHECK(ClassifyLocator(DiagLocator::Asset(kTex), f) == RouteAction::RevealAsset);       // no editor: reveal
    CHECK(ClassifyLocator(DiagLocator::Asset(kGone), f) == RouteAction::None);             // assets.unresolved never routes
    CHECK(ClassifyLocator(DiagLocator::GraphNode(kMat, 3), f) == RouteAction::OpenGraphNode);
    CHECK(ClassifyLocator(DiagLocator::GraphNode(kGone, 3), f) == RouteAction::None);
    CHECK(ClassifyLocator(DiagLocator::File("D:/p/missing.arcmat"), f) == RouteAction::None);
    CHECK(ClassifyLocator(DiagLocator::File("D:/p/Plugins"), f) == RouteAction::ShowInExplorer);
    CHECK(ClassifyLocator(DiagLocator::File("D:/p/Binaries/Game.DLL"), f) == RouteAction::ShowInExplorer);
    CHECK(ClassifyLocator(DiagLocator::File("D:/p/bin/Host.exe"), f) == RouteAction::ShowInExplorer);
    CHECK(ClassifyLocator(DiagLocator::File("D:/p/Content/Player.arcinput", 4), f) == RouteAction::OpenDocument);
    CHECK(ClassifyLocator(DiagLocator::File("D:/p/project.arcproj"), f) == RouteAction::OpenAsText);
    CHECK(IsRoutable(DiagLocator::Entity(7), f));
    CHECK_FALSE(IsRoutable(DiagLocator::Asset(kGone), f));
    CHECK(ClassifyLocator(DiagLocator::Entity(7), RouteFacts{}) == RouteAction::None);     // missing facts never throw
}

TEST_CASE("LocatorPath names the file behind File, Asset and GraphNode rows", "[editor][diagnostics]")
{
    const RouteFacts f = Facts();
    CHECK(LocatorPath(DiagLocator::File("D:/p/x.txt"), f) == std::filesystem::path("D:/p/x.txt"));
    CHECK(LocatorPath(DiagLocator::Asset(kTex), f) == std::filesystem::path("D:/p/Content/t.png"));
    CHECK(LocatorPath(DiagLocator::GraphNode(kMat, 1), f) == std::filesystem::path("D:/p/Content/a.arcmat"));
    CHECK_FALSE(LocatorPath(DiagLocator::Entity(7), f).has_value());
    CHECK_FALSE(LocatorPath(DiagLocator::Asset(kGone), f).has_value());
}

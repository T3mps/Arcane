// RuntimeLaunch: the pure candidate-list/argv logic behind the editor's
// "play in a separate window" button (Task 6 wires SpawnDetached into the
// toolbar; this task only ships and tests ExeCandidates/BuildArgs/
// SpawnDetached). ExeCandidates and BuildArgs take no OS action, so this file
// drives them directly, headlessly -- same precedent as
// EditorComponentCatalogTest/EditGestureTest. SpawnDetached is desk-verify
// only: no test here creates a process (CreateProcessW is out of scope for
// CPU-only coverage, per the task brief's "no spawn test" rule).

#include "Project/RuntimeLaunch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/HostConfig.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace Arcane::Editor::RuntimeLaunch;

namespace
{
    namespace fs = std::filesystem;

    // BuildArgs returns wide argv tokens for CreateProcessW; HostConfig::Parse
    // takes narrow argv (main()'s own argv). Every token BuildArgs ever
    // produces in these tests is plain ASCII (a filesystem path built from
    // ASCII fixtures, a lowercase hex guid, or a backend literal), so a
    // byte-for-byte narrow is exact -- this is a TEST-ONLY shortcut, not a
    // general UTF-16 to UTF-8 conversion.
    std::string Narrow(const std::wstring& w)
    {
        std::string s;
        s.reserve(w.size());
        for (wchar_t c : w)
            s.push_back(static_cast<char>(c));
        return s;
    }

    Arcane::HostConfig::ParseOutcome ParseArgs(const std::vector<std::wstring>& wideArgs)
    {
        std::vector<std::string> narrowArgs;
        narrowArgs.reserve(wideArgs.size());
        for (const std::wstring& w : wideArgs)
            narrowArgs.push_back(Narrow(w));

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("ArcaneRuntime"));
        for (auto& a : narrowArgs)
            argv.push_back(const_cast<char*>(a.c_str()));
        return Arcane::HostConfig::Parse(static_cast<int>(argv.size()), argv.data());
    }
}

TEST_CASE("ExeCandidates: packaged layout first, dev bin layout second", "[editor]")
{
    const fs::path dir = fs::path("C:/Somewhere/ArcaneEditor");
    const std::vector<fs::path> candidates = ExeCandidates(dir);

    REQUIRE(candidates.size() == 2);
    // Packaged layout: ArcaneRuntime.exe sits BESIDE ArcaneEditor.exe.
    CHECK(candidates[0] == dir / "ArcaneRuntime.exe");
    // Dev bin layout: premake's bin/<cfg>-<os>-<arch>-md/<Project>/ puts
    // every project in its own sibling directory.
    CHECK(candidates[1] == dir / ".." / "ArcaneRuntime" / "ArcaneRuntime.exe");
}

TEST_CASE("BuildArgs omits --scene for a nil guid", "[editor]")
{
    const fs::path root = fs::path("C:/Project/Root");
    const std::vector<std::wstring> args =
        BuildArgs(root, Arcane::Guid::Nil(), Arcane::GraphicsBackend::D3D12);

    REQUIRE(args.size() == 4);
    CHECK(args[0] == L"--project");
    CHECK(args[1] == root.wstring());
    CHECK(args[2] == L"--backend");
    CHECK(args[3] == L"dx12");
}

TEST_CASE("BuildArgs includes --scene for a valid guid", "[editor]")
{
    const auto scene = Arcane::Guid::FromString("a5e0c1de-1111-4222-8333-444455556666");
    REQUIRE(scene.has_value());

    const fs::path root = fs::path("C:/Project/Root");
    const std::vector<std::wstring> args = BuildArgs(root, *scene, Arcane::GraphicsBackend::Vulkan);

    REQUIRE(args.size() == 6);
    CHECK(args[0] == L"--project");
    CHECK(args[1] == root.wstring());
    CHECK(args[2] == L"--scene");
    CHECK(args[3] == L"a5e0c1de-1111-4222-8333-444455556666");
    CHECK(args[4] == L"--backend");
    CHECK(args[5] == L"vulkan");
}

TEST_CASE("BuildArgs backend token matches the enum for both backends", "[editor]")
{
    const fs::path root = fs::path("C:/P");
    const std::vector<std::wstring> d3d =
        BuildArgs(root, Arcane::Guid::Nil(), Arcane::GraphicsBackend::D3D12);
    const std::vector<std::wstring> vk =
        BuildArgs(root, Arcane::Guid::Nil(), Arcane::GraphicsBackend::Vulkan);

    REQUIRE(d3d.size() == 4);
    REQUIRE(vk.size() == 4);
    CHECK(d3d.back() == L"dx12");
    CHECK(vk.back() == L"vulkan");
}

TEST_CASE("BuildArgs round-trips through HostConfig::Parse with a valid scene", "[editor]")
{
    const fs::path root = fs::path("C:/Project/Root");
    const auto scene = Arcane::Guid::FromString("a5e0c1de-1111-4222-8333-444455556666");
    REQUIRE(scene.has_value());

    const std::vector<std::wstring> args = BuildArgs(root, *scene, Arcane::GraphicsBackend::Vulkan);
    const Arcane::HostConfig::ParseOutcome outcome = ParseArgs(args);

    REQUIRE(outcome.config.has_value());
    CHECK(outcome.config->projectPath == root.string());
    CHECK(outcome.config->sceneOverride == scene->ToString());
    CHECK(outcome.config->backend == Arcane::GraphicsBackend::Vulkan);
}

TEST_CASE("BuildArgs round-trips through HostConfig::Parse with a nil scene", "[editor]")
{
    const fs::path root = fs::path("C:/Project/Root");
    const std::vector<std::wstring> args =
        BuildArgs(root, Arcane::Guid::Nil(), Arcane::GraphicsBackend::D3D12);
    const Arcane::HostConfig::ParseOutcome outcome = ParseArgs(args);

    REQUIRE(outcome.config.has_value());
    CHECK(outcome.config->projectPath == root.string());
    // Absent flag -> HostConfig's own default: empty means "follow the
    // manifest's bootScene" (the same contract HostConfigTest pins for
    // --scene).
    CHECK(outcome.config->sceneOverride.empty());
    CHECK(outcome.config->backend == Arcane::GraphicsBackend::D3D12);
}

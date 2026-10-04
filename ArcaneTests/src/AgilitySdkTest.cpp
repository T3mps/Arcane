// The Agility SDK handshake (settings inventory R3): ONE header names the
// version every EXE exports, and it matches the vendored package.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Render/AgilitySdk.hpp>
#include "Helpers/ReferenceProjectDir.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace
{
    std::string ReadText(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
}

#if defined(_WIN32)
TEST_CASE("Agility SDK: the test EXE exports the shared version and path", "[agility]")
{
    const HMODULE exe = GetModuleHandleW(nullptr);
    const auto* version = reinterpret_cast<const unsigned*>(GetProcAddress(exe, "D3D12SDKVersion"));
    const auto* path = reinterpret_cast<const char* const*>(GetProcAddress(exe, "D3D12SDKPath"));
    REQUIRE(version != nullptr);
    REQUIRE(path != nullptr);
    CHECK(*version == Arcane::AgilitySdk::kVersion);
    CHECK(std::string_view(*path) == ".\\D3D12\\");
}
#endif

TEST_CASE("Agility SDK: every EXE uses the macro, and the version matches the vendored package", "[agility]")
{
    const std::filesystem::path root = Arcane::Test::FindReferenceProjectDir().parent_path();
    REQUIRE_FALSE(root.empty());
    for (const char* rel : { "ArcaneRuntime/src/main.cpp", "ArcaneEditor/src/main.cpp", "ArcaneTests/src/test_main.cpp" })
    {
        INFO(rel);
        const std::string text = ReadText(root / rel);
        REQUIRE_FALSE(text.empty());
        CHECK(text.find("ARC_AGILITY_SDK_EXPORTS();") != std::string::npos);
        CHECK(text.find("D3D12SDKVersion = ") == std::string::npos);       // no literal copy left
    }
    const std::string readme = ReadText(root / "ThirdParty" / "AgilitySDK" / "README.md");
    CHECK(readme.find("`1." + std::to_string(Arcane::AgilitySdk::kVersion) + ".") != std::string::npos);   // pinned 1.619.x
}

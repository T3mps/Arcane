// The new-constant guard (settings spec s10.3 form (a), s16.8): a numeric
// constant in engine or editor source is a setting, or carries
// ARC_CONSTANT("why") on the line above it, or is on scripts/constant-allowlist.txt.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/ConstantScan.hpp"

#include <Arcane/Core/Constant.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

using namespace Arcane::Test;

namespace
{
    struct AllowEntry { std::string status, note; int line = 0; };
    using AllowList = std::map<std::pair<std::string, std::string>, AllowEntry>;

    AllowList LoadAllowList()
    {
        AllowList out;
        std::ifstream in(RepoRoot() / "scripts" / "constant-allowlist.txt");
        REQUIRE(in.good());
        std::string line; int n = 0;
        while (std::getline(in, line))
        {
            ++n;
            if (line.empty() || line[0] == '#') continue;
            std::stringstream ss(line);
            std::string file, symbol, status, note;
            std::getline(ss, file, '|'); std::getline(ss, symbol, '|');
            std::getline(ss, status, '|'); std::getline(ss, note);
            out[{ file, symbol }] = AllowEntry{ status, note, n };
        }
        return out;
    }
}

TEST_CASE("constant scanner: recognises numeric constants and skips the rest", "[sweep][guard]")
{
    CHECK(NumericConstantName("constexpr float kFoo = 1.0f;") == "kFoo");
    CHECK(NumericConstantName("    static constexpr std::uint32_t kMax = 8;") == "kMax");
    CHECK(NumericConstantName("inline constexpr ImVec4 kAccent = ImVec4(0.357f, 0.498f, 0.651f, 1.00f);") == "kAccent");
    CHECK(NumericConstantName("constexpr float kCanvasClear[4] = { 0.02f, 0.02f, 0.04f, 1.0f };") == "kCanvasClear");
    CHECK(NumericConstantName("static constexpr glm::vec4 kAxisXColor{ 0.85f, 0.25f, 0.25f, 0.9f };") == "kAxisXColor");
    CHECK(NumericConstantName("static const int kRetries = 1'000;") == "kRetries");
    CHECK(NumericConstantName("extern \"C\" __declspec(dllexport) extern const unsigned D3D12SDKVersion = 619;") == "D3D12SDKVersion");
    CHECK(NumericConstantName("static constexpr const char* kIniType = \"EditorViewport\";").empty());   // a string
    CHECK(NumericConstantName("constexpr bool HasFlag(CVarFlags set, CVarFlags bit) noexcept { return 1; }").empty());  // a function
    CHECK(NumericConstantName("if constexpr (N > 2) { x = 3; }").empty());
    CHECK(NumericConstantName("constexpr float kAlias = kOther;").empty());                             // no literal: derived
    CHECK(NumericConstantName("static const char* kName = \"v2\";").empty());
    CHECK(NumericConstantName("constexpr auto rgb = [](unsigned v) constexpr { return v >> 16; };").empty());  // a lambda
}

TEST_CASE("constant scanner: ARC_CONSTANT on the nearest non-blank line above marks a site", "[sweep][guard]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane-constant-scan-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ArcaneCore" / "src");
    {
        std::ofstream f(root / "ArcaneCore" / "src" / "Probe.hpp");
        f << R"(#pragma once
ARC_CONSTANT("file format")
inline constexpr int kMarked = 6;

ARC_CONSTANT("blank lines between are skipped")

static constexpr float kMarkedAcrossBlank = 0.5f;
// ARC_CONSTANT("a comment is not the marker")
constexpr unsigned kUnmarked = 3u;
constexpr float kMultiLine[2] = {
    1.0f, 2.0f };
)";
    }
    std::map<std::string, ConstantSite> bySymbol;
    for (ConstantSite& s : ScanNumericConstants(root)) bySymbol[s.symbol] = std::move(s);
    fs::remove_all(root, ec);

    REQUIRE(bySymbol.size() == 4);
    CHECK(bySymbol.at("kMarked").marked);
    CHECK(bySymbol.at("kMarkedAcrossBlank").marked);
    CHECK_FALSE(bySymbol.at("kUnmarked").marked);
    CHECK_FALSE(bySymbol.at("kMultiLine").marked);
    CHECK(bySymbol.at("kUnmarked").file == "ArcaneCore/src/Probe.hpp");
    CHECK(bySymbol.at("kUnmarked").line == 9);
    CHECK(bySymbol.at("kMultiLine").line == 10);
}

TEST_CASE("ARC_CONSTANT: the marker expands to nothing", "[sweep][guard]")
{
    ARC_CONSTANT("test: the marker compiles away and leaves the declaration below it intact")
    constexpr int kMarked = 7;
    STATIC_CHECK(kMarked == 7);
}

TEST_CASE("constant guard: every numeric constant is a setting, marked, or allow-listed", "[sweep][guard]")
{
    const AllowList allow = LoadAllowList();
    std::string missing;
    for (const ConstantSite& s : ScanNumericConstants(RepoRoot()))
        if (!s.marked && !allow.contains({ s.file, s.symbol }))
            missing += "  " + s.file + ":" + std::to_string(s.line) + "  " + s.symbol + "\n";
    INFO("Unmarked numeric constants (make it a setting, or put ARC_CONSTANT(\"why\") on the line above):\n" << missing);
    CHECK(missing.empty());
}

TEST_CASE("constant guard: no allow-list entry is stale", "[sweep][guard]")
{
    std::set<std::pair<std::string, std::string>> live;
    for (const ConstantSite& s : ScanNumericConstants(RepoRoot()))
        if (!s.marked) live.insert({ s.file, s.symbol });
    std::string stale;
    for (const auto& [key, e] : LoadAllowList())
        if (!live.contains(key))
            stale += "  constant-allowlist.txt:" + std::to_string(e.line) + "  " + key.first + "|" + key.second + "\n";
    INFO("Entries whose constant is gone or now marked -- delete these lines:\n" << stale);
    CHECK(stale.empty());
}

TEST_CASE("constant guard: seed dump", "[.][sweep-seed]")
{
    const char* temp = std::getenv("TEMP");
    REQUIRE(temp != nullptr);
    std::ofstream out(std::filesystem::path(temp) / "constant-scan.txt");
    for (const ConstantSite& s : ScanNumericConstants(RepoRoot()))
        if (!s.marked) out << s.file << '|' << s.symbol << "|UNLISTED|line " << s.line << '\n';
}

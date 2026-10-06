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
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

TEST_CASE("constant scanner: every numeric declarator of a multi-declarator statement counts", "[sweep][guard]")
{
    using Names = std::vector<std::string>;
    // ImageCompare.cpp and ShaderEditorDocument.cpp carry these exact shapes.
    CHECK(NumericConstantNames("        constexpr double k1 = 0.045, k2 = 0.015;") == Names{ "k1", "k2" });
    CHECK(NumericConstantNames("        constexpr std::uint64_t kPinInBase = 1, kPinOutBase = 501;") == Names{ "kPinInBase", "kPinOutBase" });
    CHECK(NumericConstantName("constexpr double k1 = 0.045, k2 = 0.015;") == "k1");          // the first-name API is unchanged
    // A derived later declarator has no literal of its own; a derived FIRST one does not hide a numeric later one.
    CHECK(NumericConstantNames("constexpr int kA = 1, kB = kA;") == Names{ "kA" });
    CHECK(NumericConstantNames("constexpr int kA = kOther, kB = 2;") == Names{ "kB" });
    // Brace and array declarators after the first.
    CHECK(NumericConstantNames("static constexpr float kX{ 1.0f, 2.0f }, kY[2] = { 3.0f, 4.0f }, kZ{ 5.0f };") == Names{ "kX", "kY", "kZ" });
    // Template-argument commas are not declarator boundaries: no phantom names.
    CHECK(NumericConstantNames("constexpr auto kArr = std::array<int, 3>{ 1, 2, 3 };") == Names{ "kArr" });
    CHECK(NumericConstantNames("constexpr std::array<int, 3> kArr2{ 1, 2, 3 };") == Names{ "kArr2" });
    CHECK(NumericConstantNames("constexpr std::pair<int, float> kPair{ 1, 2.0f };") == Names{ "kPair" });
    CHECK(NumericConstantNames("constexpr auto kPair2 = std::pair<int, float>{ 1, 2.0f };") == Names{ "kPair2" });
    CHECK(NumericConstantNames("constexpr auto kPair3 = std::pair<int, float>{ 1, 2.0f }, kNext = 4;") == Names{ "kPair3", "kNext" });
    // Commas inside (), {} and [] never split; text after the statement's ';' is not part of it.
    CHECK(NumericConstantNames("inline constexpr ImVec4 kAccent = ImVec4(0.357f, 0.498f, 0.651f, 1.00f);") == Names{ "kAccent" });
    CHECK(NumericConstantNames("constexpr int kOnly = kOther; int x = 5, y = 6;").empty());
    // A lambda declarator is skipped; a numeric one beside it still counts.
    CHECK(NumericConstantNames("constexpr auto f = [](int a, int b) { return a + b + 1; }, kAfter = 2;") == Names{ "kAfter" });
    CHECK(NumericConstantNames("constexpr auto rgb = [](unsigned v) constexpr { return v >> 16; };").empty());
}

TEST_CASE("constant scanner: a multi-declarator statement gives one site per numeric declarator", "[sweep][guard]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane-constant-scan-multi-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ArcaneCore" / "src");
    {
        std::ofstream f(root / "ArcaneCore" / "src" / "Multi.cpp");
        f << R"(void F()
{
    constexpr double k1 = 0.045, k2 = 0.015;
    ARC_CONSTANT("one marker covers every declarator")
    constexpr std::uint64_t kPinInBase = 1,
                            kPinOutBase = 501;
    constexpr int kA = 1, kB = kA;
}
)";
    }
    std::map<std::string, ConstantSite> bySymbol;
    for (ConstantSite& s : ScanNumericConstants(root)) bySymbol[s.symbol] = std::move(s);
    fs::remove_all(root, ec);

    REQUIRE(bySymbol.size() == 5);
    CHECK(bySymbol.at("k1").line == 3);
    CHECK(bySymbol.at("k2").line == 3);
    CHECK_FALSE(bySymbol.at("k1").marked);
    CHECK_FALSE(bySymbol.at("k2").marked);
    CHECK(bySymbol.at("kPinInBase").line == 5);
    CHECK(bySymbol.at("kPinOutBase").line == 5);
    CHECK(bySymbol.at("kPinInBase").marked);
    CHECK(bySymbol.at("kPinOutBase").marked);
    CHECK(bySymbol.at("kA").line == 7);
    CHECK_FALSE(bySymbol.contains("kB"));
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

TEST_CASE("constant guard: no allow-listed constants remain under ArcaneCore", "[sweep][guard][markers]")
{
    std::ifstream in(RepoRoot() / "scripts" / "constant-allowlist.txt");
    std::string line, left;
    while (std::getline(in, line))
        if (line.rfind("ArcaneCore/", 0) == 0) left += "  " + line + "\n";
    INFO("Mark these with ARC_CONSTANT(\"why\") and delete the lines:\n" << left);
    CHECK(left.empty());
}

TEST_CASE("constant guard: no allow-listed constants remain under Client, Runtime, Server or CrashReporter", "[sweep][guard][markers]")
{
    constexpr std::string_view kPrefixes[] = { "ArcaneClient/", "ArcaneRuntime/", "ArcaneServer/", "ArcaneCrashReporter/" };
    std::ifstream in(RepoRoot() / "scripts" / "constant-allowlist.txt");
    std::string line, left;
    while (std::getline(in, line))
        for (std::string_view prefix : kPrefixes)
            if (line.starts_with(prefix)) left += "  " + line + "\n";
    INFO("Mark these with ARC_CONSTANT(\"why\") and delete the lines:\n" << left);
    CHECK(left.empty());
}

TEST_CASE("constant guard: no allow-listed constants remain under Editor or AssetPipeline", "[sweep][guard][markers]")
{
    constexpr std::string_view kPrefixes[] = { "ArcaneEditor/", "ArcaneAssetPipeline/" };
    std::ifstream in(RepoRoot() / "scripts" / "constant-allowlist.txt");
    std::string line, left;
    while (std::getline(in, line))
        for (std::string_view prefix : kPrefixes)
            if (line.starts_with(prefix)) left += "  " + line + "\n";
    INFO("Mark these with ARC_CONSTANT(\"why\") and delete the lines:\n" << left);
    CHECK(left.empty());
}

TEST_CASE("constant guard: the D3D12 Agility SDK version is spelled once", "[sweep][guard][markers]")
{
    int literal = 0;
    for (const ConstantSite& s : ScanNumericConstants(RepoRoot()))
        if (s.symbol == "D3D12SDKVersion") ++literal;
    CHECK(literal == 0);   // the three EXE exports read AgilitySdk::kVersion: no numeric literal left
}

TEST_CASE("constant guard: seed dump", "[.][sweep-seed]")
{
    const char* temp = std::getenv("TEMP");
    REQUIRE(temp != nullptr);
    std::ofstream out(std::filesystem::path(temp) / "constant-scan.txt");
    for (const ConstantSite& s : ScanNumericConstants(RepoRoot()))
        if (!s.marked) out << s.file << '|' << s.symbol << "|UNLISTED|line " << s.line << '\n';
}

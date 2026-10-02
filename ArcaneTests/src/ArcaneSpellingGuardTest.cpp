// THE RULE (input-seam spec 2026-10-02 s6.3): GAME-FACING CODE SPELLS
// Arcane:: ONLY. The standalone libraries keep their namespaces underneath;
// Arcane/Ecs.hpp, Arcane/EcsFwd.hpp and Arcane/Reflection.hpp re-export what
// game code needs. This test fails when a library spelling -- Astra::,
// ASTRA_*, Manifold2D, Mosaic:: -- appears in game-facing code where the
// spec forbids it. The exemptions differ by surface (s6.3):
//
//   - ReferenceProject's game sources and every editor C++ template render:
//     OUTSIDE COMMENTS only (ScanMode::CommentsOnly). An #include of a
//     library header, a #define, a string literal or an ARCANE_INTERNAL fence
//     in game code is exactly the leak the guard exists to catch.
//   - The 15 headers a game module reads (kGameFacingHeaders): PUBLIC
//     DECLARATIONS only (ScanMode::PublicDeclarations) -- comments, string
//     literals, preprocessor directives (incl. #define continuations) and
//     `// ARCANE_INTERNAL_BEGIN: <why>` ... `// ARCANE_INTERNAL_END` fences are
//     exempt, since the engine includes and implements on the libraries.

#include <catch2/catch_test_macros.hpp>

#include <Project/ClassTemplates.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "Helpers/ReferenceProjectDir.hpp"

namespace
{
    constexpr const char* kGameFacingHeaders[] = {
        "ArcaneCore/src/Arcane/Plugin/GameModule.hpp",
        "ArcaneCore/src/Arcane/Plugin/GameComponents.hpp",
        "ArcaneCore/src/Arcane/Plugin/GameSystems.hpp",
        "ArcaneCore/src/Arcane/Plugin/SystemFactory.hpp",
        "ArcaneCore/src/Arcane/Plugin/PluginABI.hpp",
        "ArcaneCore/src/Arcane/Base/ProcessContext.hpp",
        "ArcaneCore/src/Arcane/Base/Runtime.hpp",
        "ArcaneCore/src/Arcane/Base/Assert.hpp",
        "ArcaneCore/src/Arcane/Base/Log.hpp",
        "ArcaneClient/src/Arcane/Client/ClientRuntime.hpp",
        "ArcaneCore/src/Arcane/Scene/SceneResources.hpp",
        "ArcaneCore/src/Arcane/Scene/Components.hpp",
        "ArcaneCore/src/Arcane/Scene/TransformSystems.hpp",
        "ArcaneCore/src/Arcane/Scene/PhysicsComponents.hpp",
        "ArcaneCore/src/Arcane/Scene/PhysicsSystem.hpp",
    };

    struct Hit { std::string where; int line; std::string text; };

    enum class ScanMode
    {
        CommentsOnly,       // game sources + template renders: only comments are exempt
        PublicDeclarations, // the 15 headers: comments, strings, preprocessor lines and fences are exempt
    };

    // Line-oriented scan. Comments are always stripped; PublicDeclarations also
    // strips string literals, preprocessor directives (+ continuations) and
    // ARCANE_INTERNAL fences. CommentsOnly still parses string literals (so a
    // "//" inside one is not read as a comment) but keeps their contents.
    std::vector<Hit> Scan(const std::string& where, const std::string& text, ScanMode mode)
    {
        const bool decl = mode == ScanMode::PublicDeclarations;
        static const std::regex library(R"((\bAstra::|\bASTRA_[A-Z_]+|\bManifold2D\b|\bMosaic::))");
        std::vector<Hit> hits;
        std::istringstream in(text);
        std::string raw;
        int lineNo = 0;
        bool inBlock = false, inFence = false, inDefine = false;
        while (std::getline(in, raw))
        {
            ++lineNo;
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            if (decl)
            {
                if (raw.find("ARCANE_INTERNAL_BEGIN") != std::string::npos) { inFence = true;  continue; }
                if (raw.find("ARCANE_INTERNAL_END")   != std::string::npos) { inFence = false; continue; }
                if (inFence) continue;
                if (inDefine) { inDefine = !raw.empty() && raw.back() == '\\'; continue; }
            }

            std::string code;
            for (std::size_t i = 0; i < raw.size(); ++i)
            {
                if (inBlock)
                {
                    if (raw.compare(i, 2, "*/") == 0) { inBlock = false; ++i; }
                    continue;
                }
                if (raw.compare(i, 2, "/*") == 0) { inBlock = true; ++i; continue; }
                if (raw.compare(i, 2, "//") == 0) break;
                if (raw[i] == '"')
                {
                    // R"( ... )" raw strings on one line, and ordinary strings
                    const bool rawStr = i > 0 && raw[i - 1] == 'R';
                    const std::string close = rawStr ? ")\"" : "\"";
                    std::size_t j = i + 1;
                    while (j < raw.size())
                    {
                        if (!rawStr && raw[j] == '\\') { j += 2; continue; }
                        if (raw.compare(j, close.size(), close) == 0) break;
                        ++j;
                    }
                    const std::size_t end = std::min(j + close.size(), raw.size());
                    code += decl ? std::string("\"\"") : raw.substr(i, end - i);
                    i = end - 1;
                    continue;
                }
                code += raw[i];
            }
            const auto first = code.find_first_not_of(" \t");
            if (decl && first != std::string::npos && code[first] == '#')
            {
                inDefine = !raw.empty() && raw.back() == '\\';
                continue;
            }
            if (std::regex_search(code, library))
                hits.push_back({ where, lineNo, raw });
        }
        return hits;
    }

    std::string Slurp(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf(); return ss.str();
    }

    void Report(const std::vector<Hit>& hits)
    {
        for (const Hit& h : hits)
            UNSCOPED_INFO(h.where << ":" << h.line << ": " << h.text);
        CHECK(hits.empty());
    }
}

namespace
{
    const std::string kScannerSample =
        "// Astra::Registry in a comment\n"
        "const char* s = \"Astra::Registry\";\n"
        "#include <Manifold2D/Physics/PhysicsWorld.hpp>\n"
        "#define M(x) \\\n"
        "    Astra::Thing(x)\n"
        "// ARCANE_INTERNAL_BEGIN: test\n"
        "Astra::Registry hidden;\n"
        "// ARCANE_INTERNAL_END\n"
        "Arcane::Registry fine;\n"
        "Astra::Registry leaked;\n";
}

TEST_CASE("guard: header mode skips comments, strings, preprocessor lines and fences", "[guard]")
{
    const auto hits = Scan("sample", kScannerSample, ScanMode::PublicDeclarations);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].line == 10);
}

TEST_CASE("guard: comments-only mode flags includes, #define continuations, strings and fenced lines", "[guard]")
{
    const auto hits = Scan("sample", kScannerSample, ScanMode::CommentsOnly);
    std::vector<int> lines;
    for (const Hit& h : hits) lines.push_back(h.line);
    // 2 = string, 3 = #include <Manifold2D/...>, 5 = #define continuation,
    // 7 = inside an ARCANE_INTERNAL fence, 10 = plain code. Comments (1, 6, 8) stay exempt.
    CHECK(lines == std::vector<int>{ 2, 3, 5, 7, 10 });

    // A "//" inside a string is not a comment start: the token after it still counts.
    const auto url = Scan("url", "const char* u = \"a//b\"; Mosaic::Thing t;\n", ScanMode::CommentsOnly);
    REQUIRE(url.size() == 1);
    CHECK(url[0].line == 1);
}

TEST_CASE("guard: ReferenceProject's game sources spell Arcane:: only (outside comments)", "[guard]")
{
    const auto root = Arcane::Test::FindReferenceProjectDir();
    REQUIRE_FALSE(root.empty());
    std::vector<Hit> hits;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "Source"))
    {
        const auto ext = entry.path().extension();
        if (ext != ".hpp" && ext != ".cpp") continue;
        auto h = Scan(entry.path().generic_string(), Slurp(entry.path()), ScanMode::CommentsOnly);
        hits.insert(hits.end(), h.begin(), h.end());
    }
    Report(hits);
}

TEST_CASE("guard: every editor C++ template render spells Arcane:: only (outside comments)", "[guard]")
{
    using namespace Arcane::Editor;
    std::vector<Hit> hits;
    auto scan = [&](const char* what, const ClassTemplates::Rendered& r)
    {
        auto a = Scan(std::string(what) + " header", r.header, ScanMode::CommentsOnly);
        auto b = Scan(std::string(what) + " source", r.source, ScanMode::CommentsOnly);
        hits.insert(hits.end(), a.begin(), a.end());
        hits.insert(hits.end(), b.begin(), b.end());
    };
    scan("component", ClassTemplates::Render(ClassTemplates::Kind::Component, "C", "P"));
    scan("plain", ClassTemplates::Render(ClassTemplates::Kind::PlainClass, "C", "P"));
    for (int phase = 0; phase < ClassTemplates::kSystemPhaseChoiceCount; ++phase)
        for (int role = 0; role < ClassTemplates::kSystemRoleChoiceCount; ++role)
            scan("system", ClassTemplates::Render(ClassTemplates::Kind::System, "S", "P",
                                                  ClassTemplates::SystemOptionsForChoiceIndices(phase, role)));
    Report(hits);
}

TEST_CASE("guard: the 15 game-facing engine headers spell Arcane:: outside comments, preprocessor lines and fences", "[guard]")
{
    const auto root = Arcane::Test::FindReferenceProjectDir().parent_path();
    std::vector<Hit> hits;
    for (const char* rel : kGameFacingHeaders)
    {
        const auto path = root / rel;
        INFO(path.generic_string());
        REQUIRE(std::filesystem::exists(path));
        auto h = Scan(rel, Slurp(path), ScanMode::PublicDeclarations);
        hits.insert(hits.end(), h.begin(), h.end());
    }
    Report(hits);
}

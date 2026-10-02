// THE RULE (input-seam spec 2026-10-02 s6.3): GAME-FACING CODE SPELLS
// Arcane:: ONLY. The standalone libraries keep their namespaces underneath;
// Arcane/Ecs.hpp, Arcane/EcsFwd.hpp and Arcane/Reflection.hpp re-export what
// game code needs. This test fails when a library spelling -- Astra::,
// ASTRA_*, Manifold2D, Mosaic:: -- appears in game-facing code OUTSIDE a
// comment, a string literal, a preprocessor directive (incl. #define
// continuations) or an `// ARCANE_INTERNAL_BEGIN: <why>` ... `// ARCANE_INTERNAL_END`
// fence.
//
// Game-facing = ReferenceProject's game sources, every editor C++ template
// render, and the 15 headers a game module reads (kGameFacingHeaders).

#include <catch2/catch_test_macros.hpp>

#include <Project/ClassTemplates.hpp>

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

    // Line-oriented scan with comment/string/preprocessor/fence stripping.
    std::vector<Hit> Scan(const std::string& where, const std::string& text)
    {
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
            if (raw.find("ARCANE_INTERNAL_BEGIN") != std::string::npos) { inFence = true;  continue; }
            if (raw.find("ARCANE_INTERNAL_END")   != std::string::npos) { inFence = false; continue; }
            if (inFence) continue;
            if (inDefine) { inDefine = !raw.empty() && raw.back() == '\\'; continue; }

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
                    i = j + close.size() - 1;
                    code += "\"\"";
                    continue;
                }
                code += raw[i];
            }
            const auto first = code.find_first_not_of(" \t");
            if (first != std::string::npos && code[first] == '#')
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

TEST_CASE("guard: the scanner skips comments, strings, preprocessor lines and fences", "[guard]")
{
    const std::string sample =
        "// Astra::Registry in a comment\n"
        "const char* s = \"Astra::Registry\";\n"
        "#include <Astra/Registry/Registry.hpp>\n"
        "#define M(x) \\\n"
        "    Astra::Thing(x)\n"
        "// ARCANE_INTERNAL_BEGIN: test\n"
        "Astra::Registry hidden;\n"
        "// ARCANE_INTERNAL_END\n"
        "Arcane::Registry fine;\n"
        "Astra::Registry leaked;\n";
    const auto hits = Scan("sample", sample);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].line == 10);
}

TEST_CASE("guard: ReferenceProject's game sources spell Arcane:: only", "[guard]")
{
    const auto root = Arcane::Test::FindReferenceProjectDir();
    REQUIRE_FALSE(root.empty());
    std::vector<Hit> hits;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "Source"))
    {
        const auto ext = entry.path().extension();
        if (ext != ".hpp" && ext != ".cpp") continue;
        auto h = Scan(entry.path().generic_string(), Slurp(entry.path()));
        hits.insert(hits.end(), h.begin(), h.end());
    }
    Report(hits);
}

TEST_CASE("guard: every editor C++ template render spells Arcane:: only", "[guard]")
{
    using namespace Arcane::Editor;
    std::vector<Hit> hits;
    auto scan = [&](const char* what, const ClassTemplates::Rendered& r)
    {
        auto a = Scan(std::string(what) + " header", r.header);
        auto b = Scan(std::string(what) + " source", r.source);
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
        auto h = Scan(rel, Slurp(path));
        hits.insert(hits.end(), h.begin(), h.end());
    }
    Report(hits);
}

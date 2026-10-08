// THE RULE (settings spec s11.0): every well-known location resolves through
// Arcane::Paths. This guard fails when engine, client, editor or host code
// builds one by hand, outside the allow-list. The patterns are:
//   - a `/ "Saved"`, `/ "Intermediate"` or `/ "Diagnostics"` join;
//   - a read of LOCALAPPDATA;
//   - a call to temp_directory_path().
// Comments are exempt. [paths]

#include <catch2/catch_test_macros.hpp>

#include "Helpers/ReferenceProjectDir.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* kScannedRoots[] = {
        "ArcaneCore/src", "ArcaneClient/src", "ArcaneEditor/src",
        "ArcaneRuntime/src", "ArcaneServer/src", "ArcaneCrashReporter/src",
    };
    // The two places where a hand-built location is the design, not a leak.
    constexpr const char* kAllowed[] = {
        "ArcaneCore/src/Arcane/Platform/Paths.cpp",   // the one implementation
        "ArcaneCore/src/Arcane/Assets/Assets.cpp",    // artifacts beside the CONTENT ROOT it was given (a mount)
    };

    // Drop a // comment outside a string literal, and track /* */ blocks.
    std::string StripComments(const std::string& line, bool& inBlock)
    {
        std::string out;
        bool inString = false;
        char quote = 0;
        for (std::size_t i = 0; i < line.size(); ++i)
        {
            const char c = line[i];
            const char next = i + 1 < line.size() ? line[i + 1] : '\0';
            if (inBlock) { if (c == '*' && next == '/') { inBlock = false; ++i; } continue; }
            if (inString)
            {
                out += c;
                if (c == '\\' && next) { out += next; ++i; }
                else if (c == quote) inString = false;
                continue;
            }
            if (c == '"' || c == '\'') { inString = true; quote = c; out += c; continue; }
            if (c == '/' && next == '/') break;
            if (c == '/' && next == '*') { inBlock = true; ++i; continue; }
            out += c;
        }
        return out;
    }
}

TEST_CASE("Paths guard: no engine, client, editor or host source builds a well-known location by hand", "[paths]")
{
    const fs::path repo = Arcane::Test::FindReferenceProjectDir().parent_path();
    REQUIRE_FALSE(repo.empty());
    // Delimited raw string: the pattern itself contains `)"`, which would end a bare R"(...)".
    static const std::regex leak(R"rx((/\s*L?"(Saved|Intermediate|Diagnostics)")|(L?"LOCALAPPDATA")|(temp_directory_path\s*\())rx");
    std::vector<std::string> hits;
    for (const char* root : kScannedRoots)
    {
        std::error_code ec;
        const fs::path dir = repo / root;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".h") continue;
            const std::string rel = fs::relative(entry.path(), repo).generic_string();
            if (std::find(std::begin(kAllowed), std::end(kAllowed), rel) != std::end(kAllowed)) continue;
            std::ifstream in(entry.path(), std::ios::binary);
            std::string line;
            int lineNo = 0;
            bool inBlock = false;
            while (std::getline(in, line))
            {
                ++lineNo;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (std::regex_search(StripComments(line, inBlock), leak))
                    hits.push_back(rel + ":" + std::to_string(lineNo) + ": " + line);
            }
        }
    }
    std::string all;
    for (const std::string& h : hits) all += h + "\n";
    INFO(all);
    CHECK(hits.empty());
}

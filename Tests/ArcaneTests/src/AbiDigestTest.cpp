// The compiler-derived plugin ABI digest (3D-readiness Plan B, Task 1).
//
// build/abi-digest.txt holds AbiManifestMix() at the default Astra defines;
// kGamePluginABIDigest is the same value, compiled in. The tool
// arcane-abi-digest (Source/Programs/arcane-abi-digest) writes and checks the
// file. These cases pin the file, the constant, comment stability (through a
// SECOND translation unit that compiles a comment-appended copy of the
// manifest header), and the two completeness scans that keep the manifest in
// step with the 21 game-facing headers.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Core/AbiManifest.hpp>
#include <Arcane/Core/AbiVersion.hpp>

#include "Helpers/ReferenceProjectDir.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define ABI_TEST_POPEN  _popen
#define ABI_TEST_PCLOSE _pclose
#else
#define ABI_TEST_POPEN  popen
#define ABI_TEST_PCLOSE pclose
#endif

namespace
{
    namespace fs = std::filesystem;

    // The 21 headers a game module reads: the SAME list as
    // ArcaneSpellingGuardTest.cpp's kGameFacingHeaders. Keep them equal.
    constexpr const char* kAbiHeaders[] = {
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/GameModule.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/GameComponents.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/GameSystems.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/SystemFactory.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/PluginABI.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Base/ProcessContext.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Base/Runtime.hpp",
        "Source/Runtime/ArcaneCore/src/Arcane/Base/Assert.hpp",
        "Source/Runtime/ArcaneCore/src/Arcane/Base/Log.hpp",
        "Source/Client/ArcaneClient/src/Arcane/Client/ClientRuntime.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneResources.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCacheRecords.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/Components.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/TransformSystems.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsComponents.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsEvents2D.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Physics2D.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Physics2DFwd.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/Physics2DWorld.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/Physics2DHandles.hpp",
    };

    fs::path RepoRootDir() { return Arcane::Test::FindReferenceProjectDir().parent_path(); }

    std::string ReadFileBinary(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::string Hex16(std::uint64_t v)
    {
        std::ostringstream hex;
        hex << std::hex << std::setfill('0') << std::setw(16) << v;
        return hex.str();
    }

    std::string TrimTrailingNewline(std::string s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    }

    // ---- public-declaration stripping (ArcaneSpellingGuardTest ScanMode::PublicDeclarations)
    //
    // Comments, string literals (kept as ""), preprocessor lines (+ continuations)
    // and `ARC_INTERNAL_BEGIN` ... `ARC_INTERNAL_END` fences are removed. Line
    // structure is kept (one output line per input line) so the result stays readable.
    std::string StripPublic(const std::string& text)
    {
        std::string out;
        std::istringstream in(text);
        std::string raw;
        bool inBlock = false, inFence = false, inDefine = false;
        while (std::getline(in, raw))
        {
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            if (raw.find("ARC_INTERNAL_BEGIN") != std::string::npos) { inFence = true;  out += '\n'; continue; }
            if (raw.find("ARC_INTERNAL_END")   != std::string::npos) { inFence = false; out += '\n'; continue; }
            if (inFence) { out += '\n'; continue; }
            if (inDefine) { inDefine = !raw.empty() && raw.back() == '\\'; out += '\n'; continue; }

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
                    code += "\"\"";
                    i = end - 1;
                    continue;
                }
                code += raw[i];
            }
            const auto first = code.find_first_not_of(" \t");
            if (first != std::string::npos && code[first] == '#')
            {
                inDefine = !raw.empty() && raw.back() == '\\';
                out += '\n';
                continue;
            }
            out += code;
            out += '\n';
        }
        return out;
    }

    std::string Trim(const std::string& s)
    {
        const auto a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        const auto b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // ---- struct field scan -------------------------------------------------------
    //
    // Finds every `struct` defined at NAMESPACE scope (not nested in a class, a
    // struct or a function) and returns (struct name, field name) for each
    // non-static data member in its PUBLIC part. Functions, constructors, usings,
    // friends, templates and enums are not fields. A `private:`/`protected:`
    // label hides the members after it (their offsetof is ill-formed), until a
    // `public:` label. Brace initialisers and inline function bodies are skipped.
    enum class Scope { Namespace, Struct, Other };

    std::string StripAccess(std::string head, bool& priv)
    {
        static const std::regex label(R"(^\s*(public|private|protected)\s*:(?!:))");
        std::smatch m;
        while (std::regex_search(head, m, label))
        {
            priv = m[1].str() != "public";
            head = m.suffix().str();
        }
        return head;
    }

    // Splits `head` at depth-0 commas; returns the declarator names.
    std::vector<std::string> FieldNames(const std::string& declIn)
    {
        std::string decl = Trim(declIn);
        if (decl.empty()) return {};
        static const std::regex skipLead(R"(^(static|using|typedef|friend|template|enum|struct\s+\w+\s*$|class\s+\w+\s*$|static_assert|namespace)\b)");
        if (std::regex_search(decl, skipLead)) return {};
        if (decl.find("operator") != std::string::npos) return {};
        static const std::regex fnPtr(R"(\(\s*\*\s*(\w+)\s*\))");
        std::smatch pm;
        const bool hasParen = decl.find('(') != std::string::npos;
        if (hasParen)
        {
            // A function-pointer field is `ret (*name)(args)`; any other `(` is a function.
            if (std::regex_search(decl, pm, fnPtr)) return { pm[1].str() };
            return {};
        }
        // Drop attributes, then split declarators at depth 0.
        decl = std::regex_replace(decl, std::regex(R"(\[\[[^\]]*\]\])"), " ");
        std::vector<std::string> parts;
        std::string cur;
        int depth = 0;
        for (char c : decl)
        {
            if (c == '<' || c == '[' || c == '(') ++depth;
            else if (c == '>' || c == ']' || c == ')') --depth;
            if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        parts.push_back(cur);
        std::vector<std::string> names;
        for (std::string part : parts)
        {
            const auto eq = part.find('=');
            if (eq != std::string::npos) part = part.substr(0, eq);
            part = std::regex_replace(part, std::regex(R"(:\s*\d+\s*$)"), "");   // bit-field width
            part = std::regex_replace(part, std::regex(R"(\[[^\]]*\]\s*$)"), ""); // array extent
            static const std::regex last(R"((\w+)\s*$)");
            std::smatch lm;
            if (std::regex_search(part, lm, last)) names.push_back(lm[1].str());
        }
        return names;
    }

    std::vector<std::pair<std::string, std::string>> StructFields(const std::string& source)
    {
        std::string text = StripPublic(source);
        text = std::regex_replace(text, std::regex(R"(\bARC_CHANGE_TRACKED\b)"), " ");
        text = std::regex_replace(text, std::regex(R"(\bARC_CONSTANT\s*\(\s*""\s*\))"), " ");

        std::vector<std::pair<std::string, std::string>> fields;
        std::vector<std::pair<Scope, std::string>> stack;   // scope kind + struct name
        bool priv = false;
        std::string head;
        int skipDepth = 0;   // inside a skipped brace group (function body / initialiser)
        const auto top = [&]() { return stack.empty() ? Scope::Namespace : stack.back().first; };

        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (skipDepth > 0)
            {
                if (c == '{') ++skipDepth;
                else if (c == '}') --skipDepth;
                continue;
            }
            if (c == '{')
            {
                bool p = priv;
                const std::string h = Trim(StripAccess(head, p));
                static const std::regex nsRe(R"(^(inline\s+)?namespace\b|^extern\s*""$)");
                static const std::regex structRe(R"(^struct\s+(?:(?:ARC_\w+|alignas\s*\([^)]*\))\s+)*(\w+)\s*(?:final\s*)?(?::[^{]*)?$)");
                std::smatch m;
                if (top() == Scope::Namespace && std::regex_search(h, nsRe))
                {
                    stack.push_back({ Scope::Namespace, {} });
                    head.clear();
                }
                else if (top() == Scope::Namespace && h.find("template") == std::string::npos &&
                         std::regex_search(h, m, structRe))
                {
                    stack.push_back({ Scope::Struct, m[1].str() });
                    priv = false;
                    head.clear();
                }
                else if (top() == Scope::Struct)
                {
                    // A function body (the head ends in `)`/qualifiers) is discarded with
                    // its declaration; any other brace group is an initialiser, which
                    // belongs to the declaration and is simply skipped.
                    static const std::regex fnTail(R"(\)\s*(?:const|noexcept|override|final|volatile|&|&&|\s)*(?:->[^;{]*)?$)");
                    const bool isFn = std::regex_search(h, fnTail);
                    priv = p;
                    if (isFn) head.clear();
                    else head = h + " ";
                    skipDepth = 1;
                }
                else
                {
                    stack.push_back({ Scope::Other, {} });
                    head.clear();
                }
                continue;
            }
            if (c == '}')
            {
                if (!stack.empty()) stack.pop_back();
                head.clear();
                priv = false;
                continue;
            }
            if (c == ';')
            {
                if (top() == Scope::Struct)
                {
                    bool p = priv;
                    const std::string h = StripAccess(head, p);
                    priv = p;
                    if (!priv)
                        for (const std::string& n : FieldNames(h))
                            fields.push_back({ stack.back().second, n });
                }
                head.clear();
                continue;
            }
            head += c;
        }
        return fields;
    }

    // Names of the structs defined at namespace scope.
    std::vector<std::string> StructNames(const std::string& source)
    {
        const std::string text = StripPublic(source);
        std::vector<std::string> names;
        static const std::regex re(R"(\bstruct\s+(?:ARC_\w+\s+)*(\w+)\s*(?:final\s*)?(?::[^{;]*)?\{)");
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
            names.push_back((*it)[1].str());
        return names;
    }

    // Struct types a declaration passes or returns BY VALUE (not a pointer, a
    // reference, or a template argument such as std::span<const X>).
    std::set<std::string> ByValueUses(const std::string& source, const std::set<std::string>& structs)
    {
        const std::string text = StripPublic(source);
        std::set<std::string> used;
        for (const std::string& name : structs)
        {
            const std::regex param("[(,]\\s*(?:const\\s+)?(?:\\w+::)*" + name + "\\s+\\w+\\s*(?=[,)=])");
            const std::regex ret("(?:^|[;{}:>]|\\bconstexpr|\\binline|\\bstatic|\\bvirtual|\\]\\]|\\bARC_\\w+)\\s*(?:const\\s+)?(?:\\w+::)*" +
                                 name + "\\s+\\w+\\s*\\(");
            if (std::regex_search(text, param) || std::regex_search(text, ret))
                used.insert(name);
        }
        return used;
    }

    bool ManifestHasOffsetof(const std::string& manifest, const std::string& type, const std::string& field)
    {
        const std::regex re("offsetof\\(\\s*(?:\\w+::)*" + type + "\\s*,\\s*" + field + "\\s*\\)");
        return std::regex_search(manifest, re);
    }

    bool ManifestHasRoot(const std::string& manifest, const std::string& name)
    {
        const std::regex re("f\\.Type\\(\\s*\"(?:\\w+::)*" + name + "\"");
        return std::regex_search(manifest, re);
    }

    fs::path ManifestPath()
    {
        return RepoRootDir() / "Source/Runtime/ArcaneCore/src/Arcane/Core/AbiManifest.hpp";
    }

    std::string RunCaptured(const std::string& commandLine)
    {
        const std::string line = "\"" + commandLine + "\"";
        FILE* pipe = ABI_TEST_POPEN(line.c_str(), "r");
        REQUIRE(pipe != nullptr);
        std::string out;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
        ABI_TEST_PCLOSE(pipe);
        return out;
    }

    fs::path DigestToolPath()
    {
        const fs::path exeDir = fs::path(Arcane::ExecutablePathUtf8()).parent_path();
#if defined(_WIN32)
        return exeDir.parent_path() / "arcane-abi-digest" / "arcane-abi-digest.exe";
#else
        return exeDir.parent_path() / "arcane-abi-digest" / "arcane-abi-digest";
#endif
    }
}

TEST_CASE("abi digest matches build/abi-digest.txt", "[plugin][abi]")
{
    const auto path = RepoRootDir() / "build/abi-digest.txt";
    REQUIRE(fs::exists(path));
    std::string file = ReadFileBinary(path);
    file = TrimTrailingNewline(file);   // a CRLF checkout (autocrlf) ends in CRLF
    CHECK(file == Hex16(Arcane::AbiManifestMix()));
}

TEST_CASE("kGamePluginABIDigest equals the compiler-derived mix", "[plugin][abi]")
{
    CHECK(Arcane::kGamePluginABIDigest == Arcane::AbiManifestMix());
}

TEST_CASE("a comment-only change does not move the abi digest", "[plugin][abi]")
{
    // The tool compiles TWO translation units: one includes AbiManifest.hpp, the
    // other a copy of it with a comment line appended (generated by the tool's
    // prebuild step into Intermediate/AbiDigest). Each bakes its mix at compile
    // time. A source-text hash of the copy differs; the mix must not.
    const fs::path original = ManifestPath();
    const fs::path commented = RepoRootDir() / "Intermediate/AbiDigest/AbiManifest.commented.hpp";
    REQUIRE(fs::exists(original));
    REQUIRE(fs::exists(commented));
    const std::string a = ReadFileBinary(original);
    const std::string b = ReadFileBinary(commented);
    CHECK(a != b);
    CHECK(b.size() > a.size());

    const fs::path tool = DigestToolPath();
    REQUIRE(fs::exists(tool));
    const std::string plain = TrimTrailingNewline(RunCaptured(tool.string() + " --print"));
    const std::string other = TrimTrailingNewline(RunCaptured(tool.string() + " --print-commented"));
    CHECK(plain == Hex16(Arcane::AbiManifestMix()));
    CHECK(other == plain);
}

TEST_CASE("every public struct field is in the abi manifest", "[plugin][abi]")
{
    // Self-checks for the scanner.
    {
        const auto probe = StructFields("struct Probe { int alpha; };");
        REQUIRE(probe.size() == 1);
        CHECK(probe[0].first == "Probe");
        CHECK(probe[0].second == "alpha");
        CHECK(StructFields("// int alpha;").empty());
        const auto two = StructFields("struct P { int a, b; static constexpr int k = 1; void F() { int local; } int c{ 3 }; private: int hidden; };");
        REQUIRE(two.size() == 3);
        CHECK(two[0].second == "a");
        CHECK(two[1].second == "b");
        CHECK(two[2].second == "c");
    }

    const std::string manifest = ReadFileBinary(ManifestPath());
    REQUIRE_FALSE(manifest.empty());
    const fs::path root = RepoRootDir();
    std::size_t scanned = 0;
    for (const char* header : kAbiHeaders)
    {
        INFO(header);
        const std::string source = ReadFileBinary(root / header);
        REQUIRE_FALSE(source.empty());
        for (const auto& [type, field] : StructFields(source))
        {
            ++scanned;
            INFO(type << "::" << field);
            CHECK(ManifestHasOffsetof(manifest, type, field));
        }
    }
    CHECK(scanned > 100);   // the walk must actually have seen the fields
}

TEST_CASE("a by-value public parameter is in the abi root list", "[plugin][abi]")
{
    // Self-checks.
    {
        const std::set<std::string> probeSet{ "Probe" };
        CHECK(ByValueUses("Probe Foo(Probe value);", probeSet).count("Probe") == 1);
        CHECK(ByValueUses("Probe* Foo(const Probe& value);", probeSet).empty());
        CHECK(ByValueUses("void Foo(std::span<const Probe> view);", probeSet).empty());
    }

    const std::string manifest = ReadFileBinary(ManifestPath());
    REQUIRE_FALSE(manifest.empty());
    const fs::path root = RepoRootDir();

    std::set<std::string> structs{ "Entity", "Guid" };   // by-value engine types the headers use
    std::vector<std::string> sources;
    for (const char* header : kAbiHeaders)
    {
        sources.push_back(ReadFileBinary(root / header));
        for (const std::string& n : StructNames(sources.back())) structs.insert(n);
    }
    std::set<std::string> used;
    for (const std::string& source : sources)
        for (const std::string& n : ByValueUses(source, structs)) used.insert(n);

    CHECK_FALSE(used.empty());
    for (const std::string& name : used)
    {
        INFO(name);
        CHECK(ManifestHasRoot(manifest, name));
    }
}

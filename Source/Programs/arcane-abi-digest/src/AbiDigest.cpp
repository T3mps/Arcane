// arcane-abi-digest: asks the COMPILER for the game-facing layout digest.
//
//   --print              print AbiManifestMix() (16 lowercase hex digits)
//   --print-commented    print the mix of a comment-appended copy of the manifest
//   --write              write build/abi-digest.txt (hex + one newline)
//   --check              exit 1 if the file is missing, disagrees with the mix, or
//                        kGamePluginABIDigest disagrees with the mix
//   --root <dir>         the checkout (default: found upward from cwd, then the exe)
//
// Exit codes: 0 ok, 1 check failed / io error, 2 usage. Nothing here registers a
// component; the digest is a pure constexpr evaluation of AbiManifest.hpp.

#include <Arcane/Core/AbiManifest.hpp>
#include <Arcane/Core/AbiVersion.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace AbiDigestTool { std::uint64_t CommentedMix() noexcept; }

namespace
{
    namespace fs = std::filesystem;

    constexpr std::uint64_t kPlainMix = Arcane::AbiManifestMix();

    std::string Hex16(std::uint64_t v)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
        return buf;
    }

    bool IsRoot(const fs::path& dir)
    {
        std::error_code ec;
        return fs::is_regular_file(dir / "premake5.lua", ec) && fs::is_regular_file(dir / "build" / "modules.lua", ec);
    }

    fs::path FindRoot(const char* argv0)
    {
        std::error_code ec;
        for (fs::path start : { fs::current_path(ec), fs::absolute(argv0, ec).parent_path() })
        {
            for (fs::path dir = start; !dir.empty(); dir = dir.parent_path())
            {
                if (IsRoot(dir)) return dir;
                if (dir == dir.parent_path()) break;
            }
        }
        return {};
    }
}

int main(int argc, char** argv)
{
    bool print = false, printCommented = false, write = false, check = false;
    fs::path root;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--print") print = true;
        else if (a == "--print-commented") printCommented = true;
        else if (a == "--write") write = true;
        else if (a == "--check") check = true;
        else if (a == "--root" && i + 1 < argc) root = argv[++i];
        else { std::fprintf(stderr, "arcane-abi-digest: unknown argument '%s'\nusage: arcane-abi-digest --print|--print-commented|--write|--check [--root <dir>]\n", a.c_str()); return 2; }
    }
    if (static_cast<int>(print) + printCommented + write + check != 1)
    {
        std::fprintf(stderr, "usage: arcane-abi-digest --print|--print-commented|--write|--check [--root <dir>]\n");
        return 2;
    }

    if (print) { std::printf("%s\n", Hex16(kPlainMix).c_str()); return 0; }
    if (printCommented) { std::printf("%s\n", Hex16(AbiDigestTool::CommentedMix()).c_str()); return 0; }

    if (root.empty()) root = FindRoot(argv[0]);
    if (root.empty() || !IsRoot(root))
    {
        std::fprintf(stderr, "arcane-abi-digest: cannot find the checkout root (pass --root <dir>)\n");
        return 1;
    }
    const fs::path file = root / "build" / "abi-digest.txt";
    const std::string hex = Hex16(kPlainMix);

    if (write)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) { std::fprintf(stderr, "arcane-abi-digest: cannot write %s\n", file.string().c_str()); return 1; }
        out << hex << '\n';
        out.close();
        std::printf("wrote %s: %s\nkGamePluginABIDigest must be 0x%sull\n", file.string().c_str(), hex.c_str(), hex.c_str());
        return 0;
    }

    // --check
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "arcane-abi-digest: %s is missing; run arcane-abi-digest --write\n", file.string().c_str());
        return 1;
    }
    std::stringstream ss; ss << in.rdbuf();
    std::string line = ss.str();
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    int rc = 0;
    if (line != hex)
    {
        std::fprintf(stderr, "arcane-abi-digest: %s says %s but the compiler says %s; rebuild this tool, run --write, update kGamePluginABIDigest\n",
                     file.string().c_str(), line.c_str(), hex.c_str());
        rc = 1;
    }
    if (Arcane::kGamePluginABIDigest != kPlainMix)
    {
        std::fprintf(stderr, "arcane-abi-digest: kGamePluginABIDigest is %s but the compiler says %s; update AbiVersion.hpp\n",
                     Hex16(Arcane::kGamePluginABIDigest).c_str(), hex.c_str());
        rc = 1;
    }
    if (rc == 0) std::printf("abi digest ok: %s\n", hex.c_str());
    return rc;
}

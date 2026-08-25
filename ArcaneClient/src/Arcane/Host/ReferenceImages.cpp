#include <Arcane/Host/ReferenceImages.hpp>
#include <Arcane/Assets/ImageIo.hpp>

#include <system_error>

namespace fs = std::filesystem;

namespace Arcane
{
    namespace
    {
        // A reference name is a NAME, not a path. Refuse anything that could
        // walk out of the project -- this string comes from a command line and
        // BlessReference/DiffArtifactPath both turn it into a file path.
        // Applied to both `name` and `backend` everywhere in this file:
        // DiffArtifactPath builds a path from both, exactly as
        // ResolveReference does, and both strings arrive from the same
        // command-line surface (Task 8 reads `backend` from the host, not a
        // literal).
        [[nodiscard]] bool NameIsSafe(const std::string& name) noexcept
        {
            if (name.empty()) return false;
            if (name.find('/') != std::string::npos)  return false;
            if (name.find('\\') != std::string::npos) return false;
            if (name.find("..") != std::string::npos) return false;
            if (name.front() == '.') return false;
            return true;
        }
    }

    ReferenceResolution ResolveReference(const fs::path& projectRoot,
                                         const std::string& name, const std::string& backend)
    {
        ReferenceResolution out;
        if (!NameIsSafe(name) || !NameIsSafe(backend))
            return out;   // level None, blessTarget empty -- refused

        const fs::path root   = projectRoot / "Verify" / "References";
        const fs::path shared = root / (name + ".png");
        const fs::path keyed  = root / backend / (name + ".png");

        std::error_code ec;
        if (fs::exists(keyed, ec))
        {
            out.level = ReferenceLevel::Backend;
            out.path = keyed;
            out.blessTarget = keyed;
            return out;
        }
        if (fs::exists(shared, ec))
        {
            out.level = ReferenceLevel::Shared;
            out.path = shared;
            out.blessTarget = shared;
            return out;
        }

        // Nothing resolved: a first bless creates the SHARED image. If the two
        // backends turn out to disagree, the other one's failure is what tells
        // us to split it -- we do not guess up front.
        out.level = ReferenceLevel::None;
        out.blessTarget = shared;
        return out;
    }

    bool BlessReference(const ReferenceResolution& resolution,
                        std::uint32_t width, std::uint32_t height, const unsigned char* rgba)
    {
        if (resolution.blessTarget.empty() || rgba == nullptr) return false;
        return WritePngRgba(resolution.blessTarget, width, height, rgba);
    }

    fs::path DiffArtifactPath(const fs::path& projectRoot,
                              const std::string& name, const std::string& backend)
    {
        // Guarded exactly as ResolveReference guards its arguments -- this
        // path is also built from command-line-sourced strings, and Task 8
        // WRITES to it when a comparison fails. Leaving this unguarded (the
        // hole this fixes) let a hostile `name` or `backend` escape the
        // project on write, with no exists-check to catch it the way
        // ResolveReference's fs::exists incidentally would.
        if (!NameIsSafe(name) || !NameIsSafe(backend))
            return fs::path{};   // refused: nowhere safe to write

        return projectRoot / "Saved" / "Verify" / (name + "-" + backend + "-diff.png");
    }
}

#pragma once

// Where a golden reference image lives, and how an intentional visual change is
// accepted.
//
// HOST-TIER, not Assets-tier, deliberately: this knows about projects and
// backends, which ImageCompare does not and must not -- the comparator answers
// "are these two images the same", and nothing about where images come from.
//
// The hierarchy follows Unity's ColorSpace/Platform/GraphicsAPI shape, reduced
// to the one axis that actually varies for us: an image sits at the MOST
// GENERAL level that is still correct, and resolution walks up from most
// specific. This matters because D3D12 and Vulkan legitimately differ for some
// content and legitimately must not for other content -- mesh.hlsl carries a
// `#if SPIRV` split, and Plan A's desk pass measured the editor's full-UI
// capture differing by 121 ImGui text pixels across backends while the scene
// itself was identical. A flat directory forces one wrong answer or the other.

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace Arcane
{
    // Whether `name` is a safe reference NAME rather than something that
    // could walk out of the project when turned into a file path: non-empty,
    // no '/' or '\', no ".." substring, no leading '.'. ResolveReference and
    // DiffArtifactPath both apply this to `name` AND `backend` below.
    //
    // EXPORTED (Task 8) so HostConfig's own --compare parse-time refusal can
    // share this exact rule rather than duplicating it -- a name HostConfig
    // accepted that this file went on to refuse would misreport a rejected
    // name as "no reference on disk" (ResolveReference's refusal and "the
    // file genuinely does not exist" both resolve to ReferenceLevel::None),
    // and a second, independently-drifting copy of this predicate is exactly
    // how the two validators would end up disagreeing about what "unsafe"
    // means.
    [[nodiscard]] ARCANE_API bool ReferenceNameIsSafe(const std::string& name) noexcept;

    enum class ReferenceLevel : std::uint8_t
    {
        None,      // nothing on disk for this name
        Shared,    // Verify/References/<name>.png
        Backend,   // Verify/References/<backend>/<name>.png
    };

    struct ReferenceResolution
    {
        ReferenceLevel        level = ReferenceLevel::None;
        std::filesystem::path path;          // empty iff level == None
        // Where --bless writes. The level the image RESOLVED FROM, or the
        // shared level when nothing resolved. EMPTY means the name itself was
        // refused, and no write of any kind may happen.
        std::filesystem::path blessTarget;
    };

    // `name` is a bare name -- no extension, no directory. A name containing a
    // separator or a parent-directory component is REFUSED (level None,
    // blessTarget empty) rather than resolved: it arrives from a command line,
    // and blessing writes files. `backend` is guarded the same way -- it is
    // also command-line-sourced (Task 8 reads it from the host), not a literal.
    [[nodiscard]] ARCANE_API ReferenceResolution ResolveReference(
        const std::filesystem::path& projectRoot,
        const std::string& name, const std::string& backend);

    // Write `rgba` (tight RGBA8) to resolution.blessTarget, creating parents.
    // False on a refused name (blessTarget empty) or any IO failure.
    [[nodiscard]] ARCANE_API bool BlessReference(
        const ReferenceResolution& resolution,
        std::uint32_t width, std::uint32_t height, const unsigned char* rgba);

    // Where a failing comparison's diff image goes. Under Saved/, which the
    // project's .gitignore already excludes, so a failed run never leaves a
    // staged artifact behind -- Plan A's desk pass verified that ignore form
    // holds after a full editor session.
    //
    // `name` and `backend` are guarded exactly as ResolveReference guards
    // them: this path is also built from command-line-sourced strings, and
    // Task 8 WRITES to the result on a comparison failure. An EMPTY return
    // means the name or backend was refused -- the caller must not write
    // anything in that case.
    [[nodiscard]] ARCANE_API std::filesystem::path DiffArtifactPath(
        const std::filesystem::path& projectRoot,
        const std::string& name, const std::string& backend);
}

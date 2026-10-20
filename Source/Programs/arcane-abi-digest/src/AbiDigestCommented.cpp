// The comment-stability witness: this TU includes a COPY of AbiManifest.hpp with a
// comment line appended (the project's prebuild step makes the copy in
// Intermediate/AbiDigest). Its mix is baked at compile time; arcane-abi-digest
// --print-commented prints it and the [abi] test requires it to equal the plain mix.

#include <AbiManifest.commented.hpp>

#include <cstdint>

namespace AbiDigestTool
{
    namespace { constexpr std::uint64_t kCommentedMix = Arcane::AbiManifestMix(); }
    std::uint64_t CommentedMix() noexcept { return kCommentedMix; }
}

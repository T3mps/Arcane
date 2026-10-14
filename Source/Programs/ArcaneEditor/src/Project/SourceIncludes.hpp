#pragma once

// Quoted / angle #include extraction for the Asset Graph's Source neighborhood.
// Engine ListAssetReferences treats .cpp/.hpp as opaque (no JSON, no guid
// literals it can honestly see). The editor walks includes against the
// registry's source:// entries and feeds them as References.

#include <Arcane/Guid.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    // Inner paths of #include "..." and #include <...>, in file order.
    // Line comments (//) drop the rest of the line; block comments are not
    // a full preprocessor -- a stray include inside a comment may still
    // match, which is acceptable for a graph, not for a compiler.
    [[nodiscard]] std::vector<std::string> ParseIncludeDirectives(std::string_view text);

    // Resolve one include against registry All() (guid, mountPath). Prefers
    // a file next to the includer, then a unique suffix match among
    // source:// entries. Ambiguous or engine/system headers return nullopt
    // -- no tombstone for <vector>.
    [[nodiscard]] std::optional<Arcane::Guid> ResolveSourceInclude(
        std::string_view fromMount,
        std::string_view include,
        const std::vector<std::pair<Arcane::Guid, std::string>>& all);
}

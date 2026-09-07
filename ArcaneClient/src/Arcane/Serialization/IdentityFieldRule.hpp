#pragma once

// The ONE identity-field rule (asset-manager redesign, spec s3.3).
//
// A reflected Guid field whose NAME says it is an IDENTITY -- exactly "id" or
// "guid", case-insensitive -- is the entity's own name badge, not a reference
// to another asset. Arcane::Identity::id is the live case: every entity carries
// one, no AssetRegistry can ever resolve it, and treating it as an asset
// reference painted "(missing)" on perfectly healthy rows.
//
// EXACT match on purpose. A substring test would eat "textureId", which the
// kind heuristic (AssetPanelModel.hpp's AssetKindFilterForFieldName) correctly
// claims as a texture reference.
//
// This is a NAME heuristic because reflection carries no per-field attribute
// saying "this guid is an asset reference" yet; when it does, every caller of
// this function is the list of places to revisit.
//
// It lives HERE, in one engine header, because three consumers need the same
// answer and one of them is the editor: the scene serializer's manifest
// collector (ReflectionJson.hpp), the scene structural scan (Assets.cpp), and
// the Inspector's dangling-reference styling (via ArcaneEditor's
// Panels/AssetPanelModel.hpp, which now DELEGATES here rather than carrying a
// second copy). The engine cannot include an editor header -- the directional
// rule in CLAUDE.md -- so the shared definition has to sit on the engine side;
// the previous arrangement was two hand-synced copies with a comment asking
// future readers to keep them in step, which is exactly the maintenance debt
// this promotion pays off.

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace Arcane
{
    [[nodiscard]] inline bool IsIdentityGuidFieldName(std::string_view fieldName)
    {
        std::string lower(fieldName);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower == "id" || lower == "guid";
    }
}

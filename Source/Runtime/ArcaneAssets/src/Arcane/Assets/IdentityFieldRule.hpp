#pragma once

// The shared guid-field rules (asset-manager redesign, spec s3.3): which
// reflected Guid a walker should treat as an asset REFERENCE. Two questions,
// one about the field's NAME (IsIdentityGuidFieldName) and one about the
// value's JSON SHAPE (IsGuidShapedJson), both answered here so every walker
// answers them identically.
//
// ---- 1. The identity-field rule --------------------------------------------
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

#include <Json.hpp>

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

    // ---- 2. The guid-SHAPE rule --------------------------------------------
    //
    // True when this JSON value IS a serialized Guid: an object of EXACTLY two
    // unsigned-number members, `hi` and `lo`. That is the wire shape
    // Components.hpp's ASTRA_REFLECT_TYPE(Guid) produces -- Guid's only two
    // reflected fields are its own hi/lo u64s, and ReflectionJson's WriteScalar
    // copies a u64 verbatim -- so a {"hi":H,"lo":L} object literally IS
    // Guid{H,L}.
    //
    // The test is STRUCTURAL rather than on the field's type hash because one
    // of the two callers has no reflection to ask: the scene structural scan
    // (Assets.cpp's ScanSceneJson) walks a PARSED document with no FieldInfo in
    // sight. The other -- the v4 save-time manifest collector
    // (ReflectionJson.hpp) -- could ask, but must agree with the scan on WHICH
    // GUIDS COUNT as references or a v4 manifest and a pre-v4 fallback scan
    // would disagree about the same scene. Calling one function is how they
    // agree; the previous arrangement was two byte-identical copies with a
    // comment claiming they were "provably the same rule".
    //
    // Says nothing about whether the guid is nil, resolvable, or an identity
    // field -- those are each caller's own follow-up question.
    [[nodiscard]] inline bool IsGuidShapedJson(const nlohmann::json& value)
    {
        return value.is_object() && value.size() == 2 &&
               value.contains("hi") && value.contains("lo") &&
               value["hi"].is_number_unsigned() && value["lo"].is_number_unsigned();
    }
}

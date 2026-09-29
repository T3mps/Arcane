#pragma once

#include <Arcane/Guid.hpp>

#include <Json.hpp>

#include <string>

// Tolerant reads over an input-actions DRAFT. The draft is whatever the user
// last typed (a document opens a malformed source for repair, behind its
// banner), so any key may hold any JSON type: nlohmann's value()/get<> THROW
// on a type mismatch, and nothing on the draw path catches. Every read of a
// draft node in the input document, its rows, its Inspector page and its
// editor model goes through these, which check the type and degrade to the
// fallback instead.
namespace Arcane::Editor
{
    // The row's "id" as a Guid; an invalid Guid when the key is missing, not a
    // string, or not a Guid.
    [[nodiscard]] inline Guid IdOf(const nlohmann::json& row)
    {
        if (!row.is_object() || !row.contains("id") || !row["id"].is_string()) return {};
        return Guid::FromString(row["id"].get<std::string>()).value_or(Guid{});
    }
    // True when `row` carries the VALID id `id` (an invalid id matches nothing,
    // so a row with a malformed id is never mistaken for the empty selection).
    [[nodiscard]] inline bool IdIs(const nlohmann::json& row, const Guid& id)
    { return id.IsValid() && IdOf(row) == id; }
    [[nodiscard]] inline std::string Str(const nlohmann::json& row, const char* key)
    { return row.is_object() && row.contains(key) && row[key].is_string() ? row[key].get<std::string>() : std::string{}; }
    [[nodiscard]] inline bool Bool(const nlohmann::json& row, const char* key, bool fallback = false)
    { return row.is_object() && row.contains(key) && row[key].is_boolean() ? row[key].get<bool>() : fallback; }
    [[nodiscard]] inline int Int(const nlohmann::json& row, const char* key, int fallback)
    { return row.is_object() && row.contains(key) && row[key].is_number_integer() ? row[key].get<int>() : fallback; }
    // The map row carrying `id` in draft["actionMaps"]; nullptr when none.
    [[nodiscard]] inline const nlohmann::json* FindMap(const nlohmann::json& draft, const Guid& id)
    {
        if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return nullptr;
        for (const auto& m : draft["actionMaps"]) if (IdIs(m, id)) return &m;
        return nullptr;
    }
    // The first object at or under `node` carrying the valid id `id` (depth first).
    [[nodiscard]] inline const nlohmann::json* FindById(const nlohmann::json& node, const Guid& id)
    {
        if (!id.IsValid()) return nullptr;
        if (node.is_object())
        {
            if (IdOf(node) == id) return &node;
            for (const auto& [k, child] : node.items()) if (const auto* m = FindById(child, id)) return m;
        }
        else if (node.is_array())
            for (const auto& child : node) if (const auto* m = FindById(child, id)) return m;
        return nullptr;
    }
}

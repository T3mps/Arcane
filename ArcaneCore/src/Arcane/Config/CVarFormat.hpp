#pragma once

// The text forms of cvar values (settings spec 2026-10-03 s4.1, s4.8): what
// the console prints and parses (--set goes through the console's parser),
// and the "#RRGGBBAA" colour hex the JSON files use.

#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Core/Api.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    // "#RRGGBBAA", upper case. RGB are linear floats encoded through the sRGB
    // transfer and rounded to 8 bits; alpha is linear. Channels outside
    // [0,1] clamp. Decode(Encode(byte)) is exact for every byte value.
    ARC_CORE_API std::string CVarColorToHex(const CVarColor& linear);
    // "#RRGGBB" (alpha 1) or "#RRGGBBAA", either case. nullopt otherwise.
    ARC_CORE_API std::optional<CVarColor> CVarColorFromHex(std::string_view text);

    // The console's spelling. Bool "true"/"false"; integers decimal;
    // Float32/Float64 std::to_string (v1's form, unchanged); Vec* components
    // space-separated in shortest round-trip form; Color as hex; Enum as its
    // name, or the ordinal's digits when it is outside `enumNames`.
    ARC_CORE_API std::string FormatCVarValue(const CVarValue& value, const std::vector<std::string>& enumNames = {});

    // Parse console / --set text as `type`. On failure: nullopt, and `error`
    // holds the reason the console shows ("expected int32", ...).
    //   Vec2/3/4: exactly N finite floats, separated by spaces and/or commas,
    //             optionally in [ ].
    //   Color:    "#RRGGBB", "#RRGGBBAA", or 3 or 4 finite linear floats.
    //   Enum:     an exact name from `enumNames`, or a decimal ordinal inside it.
    // The eight v1 types parse exactly as v1's Execute did.
    ARC_CORE_API std::optional<CVarValue> ParseCVarText(std::string_view text, CVarType type,
                                                           const std::vector<std::string>& enumNames, std::string& error);

    // The name's ordinal in `enumNames`, or nullopt.
    ARC_CORE_API std::optional<std::int32_t> CVarEnumOrdinal(const std::vector<std::string>& enumNames, std::string_view name);

    // "bool", "int32", "uint32", "int64", "uint64", "float", "double",
    // "string", "color", "vec2", "vec3", "vec4", "enum".
    ARC_CORE_API const char* CVarTypeName(CVarType type);
}

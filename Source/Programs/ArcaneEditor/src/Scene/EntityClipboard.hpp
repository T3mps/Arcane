#pragma once

// The OS-clipboard envelope around Edit::SerializeSubtrees payloads (spec
// II.B). A versioned wrapper key so Paste can tell "our entities" from
// arbitrary clipboard text without guessing -- UE puts T3D text on the OS
// clipboard the same way. ImGui-free; the SetClipboardText/GetClipboardText
// calls stay at the consume site.

#include <Json.hpp>

#include <optional>
#include <string>
#include <utility>

namespace Arcane::Editor
{
    inline constexpr const char* kEntityClipboardKey = "arcane_entities";

    [[nodiscard]] inline std::string WrapEntityClipboard(nlohmann::json payload)
    {
        nlohmann::json env;
        env[kEntityClipboardKey] = std::move(payload);
        return env.dump();
    }

    // The payload, or nullopt for foreign/absent/unparseable clipboard text.
    // Non-throwing parse: pasting after copying prose must be a quiet no-op,
    // not an exception path.
    [[nodiscard]] inline std::optional<nlohmann::json> ParseEntityClipboard(const char* text)
    {
        if (!text || !*text)
            return std::nullopt;
        nlohmann::json env = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
        if (env.is_discarded() || !env.is_object())
            return std::nullopt;
        const auto it = env.find(kEntityClipboardKey);
        if (it == env.end() || !it->is_object())
            return std::nullopt;
        return std::move(*it);
    }
}

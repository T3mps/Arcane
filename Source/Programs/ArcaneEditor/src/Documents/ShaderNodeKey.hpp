#pragma once

// The shader document's NODE selection key (node page spec 2026-09-30
// s5.1.1): "node:<pass>:<id>". <pass> is GraphOptAt's chain index (0 = the
// base, k = m_data.passes[k-1]); <id> is GraphNode::id, unique only PER GRAPH
// -- hence the pass. Both are canonical unsigned decimals (no sign, no
// leading zero except "0", no whitespace) and id > 0, so every node has
// exactly one history key. Pure: no ImGui (the tests drive it headlessly).
// InputSelectionKey.hpp is the input-actions document's counterpart.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Arcane::Editor
{
    struct NodeKey
    {
        std::size_t   pass = 0;
        std::uint32_t id = 0;
        friend bool operator==(const NodeKey&, const NodeKey&) = default;
    };

    namespace NodeKeyDetail
    {
        // Digits only, no leading zero unless the text IS "0", and it fits T.
        template <class T>
        [[nodiscard]] std::optional<T> ParseCanonical(std::string_view s) noexcept
        {
            if (s.empty() || (s.size() > 1 && s[0] == '0'))
                return std::nullopt;
            for (const char c : s)
                if (c < '0' || c > '9')
                    return std::nullopt;
            T v{};
            const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
            if (ec != std::errc{} || end != s.data() + s.size())
                return std::nullopt;   // out of range
            return v;
        }
    }

    inline constexpr std::string_view kNodeKeyPrefix = "node:";

    [[nodiscard]] inline std::optional<NodeKey> ParseNodeKey(std::string_view key) noexcept
    {
        if (!key.starts_with(kNodeKeyPrefix))
            return std::nullopt;
        key.remove_prefix(kNodeKeyPrefix.size());
        const std::size_t colon = key.find(':');
        if (colon == std::string_view::npos)
            return std::nullopt;
        const auto pass = NodeKeyDetail::ParseCanonical<std::size_t>(key.substr(0, colon));
        const auto id = NodeKeyDetail::ParseCanonical<std::uint32_t>(key.substr(colon + 1));
        if (!pass || !id || *id == 0)
            return std::nullopt;
        return NodeKey{ *pass, *id };
    }

    [[nodiscard]] inline std::string FormatNodeKey(NodeKey k)
    {
        return std::string(kNodeKeyPrefix) + std::to_string(k.pass) + ":" + std::to_string(k.id);
    }

    // --select-in-document's spelling: "<id>" (the active pass) or "<pass>/<id>".
    [[nodiscard]] inline std::optional<NodeKey> ParseNodeSelectPath(std::string_view path,
                                                                    std::size_t activePass) noexcept
    {
        std::size_t pass = activePass;
        if (const std::size_t slash = path.find('/'); slash != std::string_view::npos)
        {
            const auto p = NodeKeyDetail::ParseCanonical<std::size_t>(path.substr(0, slash));
            if (!p)
                return std::nullopt;
            pass = *p;
            path.remove_prefix(slash + 1);
        }
        const auto id = NodeKeyDetail::ParseCanonical<std::uint32_t>(path);
        if (!id || *id == 0)
            return std::nullopt;
        return NodeKey{ pass, *id };
    }

    // What one DRAWN graph-canvas frame's selection read means (s5.1.1), read
    // after ed::End: exactly one selected node -> that node in the active
    // pass, 0 or 2+ -> none (the material page). `selectedNodes` is
    // GetSelectedNodes' capped count, so 2 means "two or more". An EVENT is a
    // change the library saw this frame that the document did not apply
    // itself (a restore arms a request and sets `applying`).
    struct CanvasSelectionRead
    {
        std::optional<NodeKey> sel;
        bool event = false;
    };
    [[nodiscard]] inline CanvasSelectionRead ReadCanvasSelection(int selectedNodes, std::uint32_t firstId,
                                                                 std::size_t activePass, bool selectionChanged,
                                                                 bool applying) noexcept
    {
        CanvasSelectionRead r;
        if (selectedNodes == 1 && firstId != 0)
            r.sel = NodeKey{ activePass, firstId };
        r.event = selectionChanged && !applying;
        return r;
    }
}

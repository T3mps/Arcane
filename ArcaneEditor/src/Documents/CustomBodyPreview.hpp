#pragma once

// The Custom node's in-canvas body preview (settings sweep S6-35): the first
// editor.shader.bodyPreviewLines lines of the HLSL body, each clipped to
// editor.shader.bodyPreviewChars with a "..." suffix, plus a truncated flag
// when lines remain past the cap (the node then draws a trailing "..." row).
// Pure, so the cap and the marker are testable at non-default values.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    struct CustomBodyPreview
    {
        std::vector<std::string> lines;
        bool truncated = false;   // body lines remain past maxLines
    };

    inline CustomBodyPreview BuildCustomBodyPreview(std::string_view body, int maxLines, std::size_t lineChars)
    {
        CustomBodyPreview out;
        int shown = 0;
        while (!body.empty() && shown < maxLines)
        {
            const std::size_t nl = body.find('\n');
            std::string_view lineText = body.substr(0, nl);
            if (!lineText.empty() && lineText.back() == '\r')
                lineText.remove_suffix(1);
            std::string display(lineText.substr(0, lineChars));
            if (lineText.size() > lineChars)
                display += "...";
            out.lines.push_back(std::move(display));
            ++shown;
            if (nl == std::string_view::npos)
            {
                body = {};
                break;
            }
            body.remove_prefix(nl + 1);
        }
        out.truncated = !body.empty();
        return out;
    }
}

#pragma once

namespace Grimoire
{
    // Install the editor's fonts on the CURRENT ImGui context: Roboto base + merged
    // lucide icon glyphs. Call once in Init, after the editor ImGuiLayer is up and its
    // context is current, before the first frame. Font paths resolve exe-relative.
    void InstallEditorFonts(float sizePx = 16.0f);
}

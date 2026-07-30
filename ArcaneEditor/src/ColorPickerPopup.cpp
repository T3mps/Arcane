#include "ColorPickerPopup.hpp"

#include "EditorWidgets.hpp"   // SrgbToLinear / LinearToSrgb

#include <cstdio>
#include <cstring>

namespace Arcane::Editor
{
    namespace
    {
        // Encode for display. RGB converts, alpha never does.
        void EncodeForDisplay(const float lin[4], bool hdr, float out[4]) noexcept
        {
            if (hdr)
            {
                for (int i = 0; i < 4; ++i) out[i] = lin[i];
                return;
            }
            out[0] = LinearToSrgb(lin[0]);
            out[1] = LinearToSrgb(lin[1]);
            out[2] = LinearToSrgb(lin[2]);
            out[3] = lin[3];
        }

        void DecodeFromDisplay(const float disp[4], bool hdr, float out[4]) noexcept
        {
            if (hdr)
            {
                for (int i = 0; i < 4; ++i) out[i] = disp[i];
                return;
            }
            out[0] = SrgbToLinear(disp[0]);
            out[1] = SrgbToLinear(disp[1]);
            out[2] = SrgbToLinear(disp[2]);
            out[3] = disp[3];
        }

        ImVec4 SwatchColor(const float lin[4]) noexcept
        {
            // ALWAYS encoded -- imgui.hlsl's colours are display-referred.
            return ImVec4(LinearToSrgb(lin[0]), LinearToSrgb(lin[1]),
                          LinearToSrgb(lin[2]), lin[3]);
        }

        void HexOf(const float c[4], char out[10]) noexcept
        {
            auto b = [](float v) -> int
            {
                const float s = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                return static_cast<int>(s * 255.0f + 0.5f);
            };
            std::snprintf(out, 10, "%02X%02X%02X%02X", b(c[0]), b(c[1]), b(c[2]), b(c[3]));
        }

        // Returns true when `text` parsed as RRGGBB or RRGGBBAA. A 6-digit form
        // leaves out[3] AS THE CALLER SEEDED IT, so a 6-digit paste keeps the
        // colour's existing alpha rather than forcing it opaque.
        bool ParseHex(const char* text, float out[4]) noexcept
        {
            unsigned r = 0, g = 0, b = 0, a = 0;
            bool haveAlpha = false;
            const std::size_t n = std::strlen(text);
            if (n == 6)
            {
                if (std::sscanf(text, "%2x%2x%2x", &r, &g, &b) != 3) return false;
            }
            else if (n == 8)
            {
                if (std::sscanf(text, "%2x%2x%2x%2x", &r, &g, &b, &a) != 4) return false;
                haveAlpha = true;
            }
            else
            {
                return false;
            }
            out[0] = r / 255.0f; out[1] = g / 255.0f; out[2] = b / 255.0f;
            if (haveAlpha)
                out[3] = a / 255.0f;
            return true;
        }

        // One labelled hex row. `encode` selects which space the field speaks.
        bool HexRow(const char* label, float linear[4], bool encodeSrgb)
        {
            float shown[4];
            if (encodeSrgb) EncodeForDisplay(linear, /*hdr*/ false, shown);
            else            std::memcpy(shown, linear, sizeof(shown));

            char buf[10];
            HexOf(shown, buf);

            char seed[10];
            std::memcpy(seed, buf, sizeof(seed));

            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            const bool committed = ImGui::InputText(label, buf, sizeof(buf),
                                                    ImGuiInputTextFlags_CharsHexadecimal
                                                    | ImGuiInputTextFlags_CharsUppercase
                                                    | ImGuiInputTextFlags_EnterReturnsTrue);
            if (!committed)
                return false;

            if (std::strcmp(buf, seed) == 0)
                return false;      // bare Enter on an untouched field is not an edit --
                                   // reparsing would re-quantize storage to 8 bits and
                                   // report a change the user never made

            float parsed[4];
            std::memcpy(parsed, shown, sizeof(parsed));   // alpha survives a 6-digit entry
            if (!ParseHex(buf, parsed))
                return false;          // garbage typed -- reverts on the next frame's reseed

            if (encodeSrgb) DecodeFromDisplay(parsed, /*hdr*/ false, linear);
            else            std::memcpy(linear, parsed, sizeof(parsed));
            return true;
        }
    }

    ImGuiID ColorPopupId(const char* id)
    {
        return ImGui::GetID(id);
    }

    bool ColorSwatchButton(const char* id, const float linear[4], ImVec2 size)
    {
        if (size.x <= 0.0f) size.x = ImGui::GetFrameHeight();
        if (size.y <= 0.0f) size.y = ImGui::GetFrameHeight();
        // AlphaPreviewHalf so a translucent colour is legible; NoTooltip because the
        // popup is one click away and a tooltip over a button that opens it is noise.
        // NoDragDrop: this swatch is deliberately ENCODED, and ImGui's colour
        // drag-drop payload is raw floats -- dropping it on a linear-space widget
        // would write the sRGB number into linear storage.
        return ImGui::ColorButton(id, SwatchColor(linear),
                                  ImGuiColorEditFlags_AlphaPreviewHalf
                                  | ImGuiColorEditFlags_NoTooltip
                                  | ImGuiColorEditFlags_NoDragDrop,
                                  size);
    }

    bool ColorPopupBody(float linear[4], const float original[4], bool hdr)
    {
        bool changed = false;

        // ---- Old / New, drawn by US -------------------------------------------
        // ImGui's own side preview reads the buffer and cannot be re-encoded from
        // outside, so it would be wrong per the space contract. NoSidePreview below.
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Old");
        ImGui::ColorButton("##old", SwatchColor(original),
                           ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoTooltip
                           | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(ImGui::GetFontSize() * 4.0f, ImGui::GetFontSize() * 1.5f));
        ImGui::TextUnformatted("New");
        ImGui::ColorButton("##new", SwatchColor(linear),
                           ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoTooltip
                           | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(ImGui::GetFontSize() * 4.0f, ImGui::GetFontSize() * 1.5f));
        ImGui::EndGroup();
        ImGui::SameLine();

        // ---- the picker itself, on ENCODED values ------------------------------
        float display[4];
        EncodeForDisplay(linear, hdr, display);

        const ImGuiColorEditFlags pickerFlags =
            (hdr ? (ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                 : ImGuiColorEditFlags_Uint8)
            | ImGuiColorEditFlags_DisplayRGB
            | ImGuiColorEditFlags_DisplayHSV
            | ImGuiColorEditFlags_AlphaBar
            | ImGuiColorEditFlags_PickerHueWheel
            | ImGuiColorEditFlags_NoSidePreview
            | ImGuiColorEditFlags_NoLabel;

        ImGui::BeginGroup();
        if (!hdr)
            ImGui::TextUnformatted("sRGB");
        // Pin the wheel's size. ColorPicker4 takes CalcItemWidth(), which in a
        // popup with no explicit width is 65% of the window -- and the window
        // auto-fits to the Linear row and the hex fields below, so the wheel's
        // diameter would be an accident of rows unrelated to it, with a
        // first-frame transient while the fit converges. ImGui's own colour
        // popup pins the same way (imgui_widgets.cpp:5978).
        ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 12.0f);
        if (ImGui::ColorPicker4("##picker", display, pickerFlags))
        {
            DecodeFromDisplay(display, hdr, linear);
            changed = true;
        }
        ImGui::EndGroup();

        // ---- the LINEAR readout, ours -----------------------------------------
        ImGui::Separator();
        ImGui::TextUnformatted("Linear");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        // NoPicker/NoSmallPreview: this row is a numeric readout of storage, not a
        // second picker. Float display because storage IS float and this is the row
        // that never lies about the space. DisplayRGB and NoOptions PIN that: without
        // them the mode comes from the global g.ColorEditOptions and this row's own
        // right-click menu can switch it to HSV, which would both mislabel the space
        // and clobber the hue/saturation slot the picker above restores from.
        if (ImGui::ColorEdit4("##linear", linear,
                              ImGuiColorEditFlags_Float
                              | ImGuiColorEditFlags_NoPicker
                              | ImGuiColorEditFlags_NoSmallPreview
                              | ImGuiColorEditFlags_NoLabel
                              | ImGuiColorEditFlags_NoDragDrop
                              | ImGuiColorEditFlags_DisplayRGB
                              | ImGuiColorEditFlags_NoOptions
                              | (hdr ? ImGuiColorEditFlags_HDR : 0)))
            changed = true;

        // ---- both hexes, each labelled ----------------------------------------
        if (!hdr)
        {
            if (HexRow("Hex sRGB",   linear, /*encodeSrgb*/ true))  changed = true;
            if (HexRow("Hex Linear", linear, /*encodeSrgb*/ false)) changed = true;
        }

        return changed;
    }
}

#pragma once

// Arcane::Editor -- THE colour picker for the editor. UE-shaped: one dense panel,
// no tabs, every colour space named on screen.
//
// THE DEFECT THIS EXISTS TO FIX is not a wrong curve -- the curve is correct. It is
// that no number on screen ever said which space it was in. That ambiguity produced
// a sanity check whose own expected output (128 -> 186) was computed with the
// pow(1/2.2) the work forbade; the true answer is 188.
//
// SPACE CONTRACT, and it is the whole design:
//   * STORAGE is linear. Callers pass linear and get linear back. Always.
//   * The four inline channel boxes at a property row show LINEAR, unconverted.
//   * ImGui's ColorPicker4 is handed sRGB-ENCODED values, and its rows are labelled
//     sRGB. This is forced, not chosen: the SV cursor position and picking response
//     derive from HSV of the buffer, so linear values put display 0.735-1.0 across
//     half the picking area and crush the whole shadow range into a sliver. Its
//     alpha-bar tint (imgui_widgets.cpp:6350) and side preview (:6245) read the buffer
//     too and render dark. Every colour picker works in gamma space for this reason.
//   * Both hexes are shown, always, each labelled. Better than UE, which switches
//     ONE field between modes (SColorPicker.cpp:378-413) and so still lets a reader
//     see the wrong number.
//   * SWATCHES ARE ALWAYS ENCODED. imgui.hlsl states its own contract -- "vertex
//     colors ... are display-referred; no linearization" -- so a swatch filled with
//     a raw linear value renders too dark. That is the original tint defect one
//     layer up. There is no toggle: UE's sRGB Preview checkbox solves this same
//     problem, but it is only needed because UE shows no linear/sRGB pair
//     numerically. We do, so the un-encoded view has no use.
//   * ALPHA IS NEVER ENCODED. Coverage, not colour.
//
// UNDO IS THE CALLER'S. These three functions report; they never touch the
// CommandStack. Bracket with EditGesture::BeginOnPopupOpen / EndOnPopupClose on
// ColorPopupId(id) -- the activation pair does NOT work here, because ImGui only
// lends its ActiveId to popups IT opened (EditGesture.hpp's ShouldClosePopup note).

#include <imgui.h>

namespace Arcane::Editor
{
    // The popup id for a site. Stable for a given `id` string within a window, and
    // the same value to pass to ImGui::OpenPopup (imgui.h:868's ImGuiID overload)
    // and to the EditGesture popup pair.
    [[nodiscard]] ImGuiID ColorPopupId(const char* id);

    // The property-row swatch. Fills with the ENCODED colour (see the contract
    // above) and returns true when clicked, so the caller opens the popup. Default
    // size follows the current frame height, square.
    [[nodiscard]] bool ColorSwatchButton(const char* id, const float linear[4],
                                         ImVec2 size = ImVec2(0, 0));

    // The dense popup body. Call INSIDE an already-open popup window.
    //
    // `linear` is read and written in linear. `original` is the colour as it was
    // when the popup opened, for the Old/New pair -- the caller latches it once on
    // open. `hdr` skips the sRGB encode for the PICKER BUFFER only and hides both hex
    // rows, for values that may exceed 1 (a ConstColor node feeds raw shader maths);
    // the Old/New swatches still encode unconditionally through SwatchColor regardless
    // of `hdr` (swatches are ALWAYS encoded, per the contract above). The SV response
    // is poor for HDR values, which is inherent and equally true in UE.
    //
    // Returns true only on frames ImGui reported a change.
    [[nodiscard]] bool ColorPopupBody(float linear[4], const float original[4], bool hdr);
}

#pragma once

// EditorTheme: the Arcane Editor's ImGui color scheme -- a MONOCHROMATIC
// gray ramp modelled on UE5's editor chrome, replacing ImGui's stock dark
// style (which is a blue family: FrameBg/Button/Header/Tab/CheckMark/
// SliderGrab/TitleBgActive/ResizeGrip/NavCursor all resolve from
// ImVec4(0.26,0.59,0.98) -- imgui_draw.cpp:192-255).
//
// THREE TONAL LAYERS, and the ordering between them is the whole look:
//
//   CHROME  (kChromeDeep/kChrome) -- title bars, menu bar, the tab strip and
//           unselected tabs. Darker than the panels they frame.
//   PANEL   (kPanel/kPanelRaised) -- window and child bodies, tree rows, the
//           SELECTED tab (so an active tab merges into the body below it).
//   WELL    (kWell/kWellHovered/kWellActive) -- every input: drag/slider
//           fields, text inputs, search boxes, combo boxes. NEAR-BLACK and
//           visibly darker than the panel behind them, so a field reads as an
//           inset well rather than a raised button. This is the single most
//           load-bearing distinction in the reference and the reason the theme
//           needs a third tone at all; ImGui's stock style has no such layer
//           (its FrameBg is a translucent blue tint of the window behind it).
//
// The one non-gray hue is kSelection, a muted desaturated blue-gray used ONLY
// where something is SELECTED (selected rows/items, selected text, the docking
// preview, the selected tab's overline). Everything else is neutral gray.
//
// Domain color-coding is deliberately NOT monochrome and does not live here:
// the inspector's X/Y/Z axis bars (EditorWidgets.cpp), the shader graph's
// typed pin dots and node accents (ShaderEditorDocument.cpp), and the amber
// viewport selection outline (the NRI outline composite's kSelectColor,
// Render/Nri/nodes/PickOutlineNodes.cpp:101) all keep their hues.
// UE does the same -- the monochrome rule governs CHROME, not data.
//
// All values are DISPLAY-REFERRED: the editor's ImGui pass draws post-tonemap
// straight into the backbuffer (imgui.hlsl), so these are literally what the
// user sees. Hex comments are the 8-bit spelling of the float triple.
//
// Header-only and free of every editor type on purpose: ApplyEditorTheme takes
// the ImGuiStyle to fill, so any Arcane ImGui consumer (a game's debug HUD, a
// future tool host) can adopt the same look with one call. The editor is the
// only caller today -- ArcaneRuntime/Sandbox HUDs are untouched.

#include <imgui.h>

namespace Arcane::Editor
{
    namespace Theme
    {
        // Same tone at a different alpha, so a translucent entry still names
        // the tone it comes from instead of repeating its channels. constexpr
        // because ImVec4's 4-float constructor is (imgui.h:317).
        constexpr ImVec4 WithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

        // -- CHROME -------------------------------------------------------
        // Title bars sit at the bottom of the ramp (near-black in the
        // reference); the menu bar / tab strip / popups one step above.
        inline constexpr ImVec4 kChromeDeep   = ImVec4(0.047f, 0.047f, 0.047f, 1.00f); // #0c0c0c
        inline constexpr ImVec4 kChrome       = ImVec4(0.098f, 0.098f, 0.098f, 1.00f); // #191919

        // -- PANEL --------------------------------------------------------
        // kPanel is the editor's base surface. kPanelRaised is the one step
        // up used for transient row/tab hover. (Table header bands moved to
        // kChrome -- sharing the hover tone made headers read as rows.)
        inline constexpr ImVec4 kPanel        = ImVec4(0.118f, 0.118f, 0.118f, 1.00f); // #1e1e1e
        inline constexpr ImVec4 kPanelRaised  = ImVec4(0.165f, 0.165f, 0.165f, 1.00f); // #2a2a2a

        // -- FIELD WELLS --------------------------------------------------
        // Below chrome, well below panel. Hover/active step up just enough to
        // acknowledge the cursor without ever reaching the panel tone -- a
        // field that brightened past its panel would stop reading as inset.
        inline constexpr ImVec4 kWell         = ImVec4(0.071f, 0.071f, 0.071f, 1.00f); // #121212
        inline constexpr ImVec4 kWellHovered  = ImVec4(0.094f, 0.094f, 0.094f, 1.00f); // #181818
        inline constexpr ImVec4 kWellActive   = ImVec4(0.110f, 0.110f, 0.110f, 1.00f); // #1c1c1c

        // -- RAISED (buttons, scrollbar grabs) ----------------------------
        // Flat panel-family gray, lighter on hover. Never a tint.
        inline constexpr ImVec4 kButton        = ImVec4(0.184f, 0.184f, 0.184f, 1.00f); // #2f2f2f
        inline constexpr ImVec4 kButtonHovered = ImVec4(0.239f, 0.239f, 0.239f, 1.00f); // #3d3d3d
        inline constexpr ImVec4 kButtonActive  = ImVec4(0.294f, 0.294f, 0.294f, 1.00f); // #4b4b4b

        // -- SELECTION ----------------------------------------------------
        // The ONE hue in the theme: UE's selected-row blue-gray, desaturated
        // far enough that it reads as "a gray with a cast" beside the ramp.
        inline constexpr ImVec4 kSelection    = ImVec4(0.180f, 0.251f, 0.325f, 1.00f); // #2e4053

        // -- TEXT AND LINES -----------------------------------------------
        // Text is off-white, not white: pure white on a near-black well
        // glares. kBorder is DARKER than every surface it outlines, which is
        // what draws the 1px inset edge around a field well.
        inline constexpr ImVec4 kText          = ImVec4(0.878f, 0.878f, 0.878f, 1.00f); // #e0e0e0
        inline constexpr ImVec4 kTextDim       = ImVec4(0.451f, 0.451f, 0.451f, 1.00f); // #737373
        inline constexpr ImVec4 kBorder        = ImVec4(0.051f, 0.051f, 0.051f, 1.00f); // #0d0d0d
        inline constexpr ImVec4 kSeparator     = ImVec4(0.200f, 0.200f, 0.200f, 1.00f); // #333333
        inline constexpr ImVec4 kSeparatorHot  = ImVec4(0.290f, 0.290f, 0.290f, 1.00f); // #4a4a4a
        inline constexpr ImVec4 kSeparatorHeld = ImVec4(0.431f, 0.431f, 0.431f, 1.00f); // #6e6e6e

        // -- GRABS AND MARKS ----------------------------------------------
        // Checkmarks, radio dots and slider grabs are LIGHT neutral gray in
        // the reference -- they are the widget's only foreground, so they read
        // off the text end of the ramp rather than the surface end.
        inline constexpr ImVec4 kGrab       = ImVec4(0.604f, 0.604f, 0.604f, 1.00f); // #9a9a9a
        inline constexpr ImVec4 kGrabActive = ImVec4(0.784f, 0.784f, 0.784f, 1.00f); // #c8c8c8
        inline constexpr ImVec4 kCheck      = ImVec4(0.831f, 0.831f, 0.831f, 1.00f); // #d4d4d4

        // -- DELIBERATELY NOT GRAY ----------------------------------------
        // The drop-target frame speaks the editor's existing amber "this is
        // the thing you are acting on" language (the viewport outline's
        // kSelectColor, Render/Nri/nodes/PickOutlineNodes.cpp:101, and the
        // shader graph's selected-node border), not the stock pure yellow.
        // Histogram bars are a data mark, exempt like the axis bars.
        inline constexpr ImVec4 kAmber      = ImVec4(1.000f, 0.650f, 0.100f, 1.00f);
        inline constexpr ImVec4 kAmberLight = ImVec4(1.000f, 0.780f, 0.350f, 1.00f);

        // Fully transparent -- spelled once so the entries that mean "draw
        // nothing here" say so rather than repeating a zero vector.
        inline constexpr ImVec4 kNone = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    }

    // Apply the theme to `style`. Fills ALL of ImGuiCol_COUNT (63 entries in
    // the vendored 1.92.9, imgui.h:1821-1886): StyleColorsDark runs first so a
    // future upstream entry has a sane value the day it appears, then every
    // entry that exists today is overwritten below. Call once at boot, before
    // the first frame, on the context that will use it.
    inline void ApplyEditorTheme(ImGuiStyle& style)
    {
        ImGui::StyleColorsDark(&style);

        ImVec4* c = style.Colors;

        c[ImGuiCol_Text]                   = Theme::kText;
        c[ImGuiCol_TextDisabled]           = Theme::kTextDim;
        c[ImGuiCol_WindowBg]               = Theme::kPanel;                 // opaque: stock's 0.94 alpha
        c[ImGuiCol_ChildBg]                = Theme::kNone;                  // children inherit the panel
        c[ImGuiCol_PopupBg]                = Theme::kChrome;
        c[ImGuiCol_Border]                 = Theme::kBorder;
        c[ImGuiCol_BorderShadow]           = Theme::kNone;

        // The wells. FrameBg reaches every input ImGui frames: InputText,
        // Drag*/Slider*, Combo, Checkbox, ColorEdit's swatch row (imgui.h:1830).
        c[ImGuiCol_FrameBg]                = Theme::kWell;
        c[ImGuiCol_FrameBgHovered]         = Theme::kWellHovered;
        c[ImGuiCol_FrameBgActive]          = Theme::kWellActive;

        c[ImGuiCol_TitleBg]                = Theme::kChromeDeep;
        c[ImGuiCol_TitleBgActive]          = Theme::kChrome;                // focused: one step up, still chrome
        c[ImGuiCol_TitleBgCollapsed]       = Theme::WithAlpha(Theme::kChromeDeep, 0.75f);
        c[ImGuiCol_MenuBarBg]              = Theme::kChrome;

        // Scrollbar: near-black track (an inset channel, same idea as a well),
        // raised-gray grab.
        c[ImGuiCol_ScrollbarBg]            = Theme::kChromeDeep;
        c[ImGuiCol_ScrollbarGrab]          = Theme::kButton;
        c[ImGuiCol_ScrollbarGrabHovered]   = Theme::kButtonHovered;
        c[ImGuiCol_ScrollbarGrabActive]    = Theme::kButtonActive;

        c[ImGuiCol_CheckMark]              = Theme::kCheck;
        c[ImGuiCol_CheckboxSelectedBg]     = Theme::kWellActive;            // a checked box stays a well
        c[ImGuiCol_SliderGrab]             = Theme::kGrab;
        c[ImGuiCol_SliderGrabActive]       = Theme::kGrabActive;

        c[ImGuiCol_Button]                 = Theme::kButton;
        c[ImGuiCol_ButtonHovered]          = Theme::kButtonHovered;
        c[ImGuiCol_ButtonActive]           = Theme::kButtonActive;

        // Header* is BOTH the selected state of Selectable/TreeNode (an
        // outliner row, an asset tile) and the background of a bare
        // CollapsingHeader (imgui.h:1848). Selected takes the accent; hover
        // and held stay gray, so hovering an unselected row never flashes a
        // second hue. The inspector's category bands push their own trio over
        // this one (EditorWidgets.cpp, PushHeaderBandColors).
        c[ImGuiCol_Header]                 = Theme::kSelection;
        c[ImGuiCol_HeaderHovered]          = Theme::kPanelRaised;
        c[ImGuiCol_HeaderActive]           = Theme::kButton;

        // Separator also draws the splitter between docked windows, so its
        // hover/held states are what a resize drag feels like.
        c[ImGuiCol_Separator]              = Theme::kSeparator;
        c[ImGuiCol_SeparatorHovered]       = Theme::kSeparatorHot;
        c[ImGuiCol_SeparatorActive]        = Theme::kSeparatorHeld;

        c[ImGuiCol_ResizeGrip]             = Theme::WithAlpha(Theme::kGrab, 0.20f);
        c[ImGuiCol_ResizeGripHovered]      = Theme::WithAlpha(Theme::kGrab, 0.55f);
        c[ImGuiCol_ResizeGripActive]       = Theme::WithAlpha(Theme::kGrabActive, 0.85f);

        c[ImGuiCol_InputTextCursor]        = Theme::kText;                  // caret, light gray

        // Tabs: selected = the panel tone (the tab and the body under it are
        // one surface), unselected = chrome, hover = one step of panel.
        c[ImGuiCol_TabHovered]             = Theme::kPanelRaised;
        c[ImGuiCol_Tab]                    = Theme::kChrome;
        c[ImGuiCol_TabSelected]            = Theme::kPanel;
        c[ImGuiCol_TabSelectedOverline]    = Theme::kSelection;             // selected == accent
        c[ImGuiCol_TabDimmed]              = Theme::kChromeDeep;            // unfocused tab bar sinks
        c[ImGuiCol_TabDimmedSelected]      = Theme::kChrome;
        c[ImGuiCol_TabDimmedSelectedOverline] = Theme::kNone;

        c[ImGuiCol_DockingPreview]         = Theme::WithAlpha(Theme::kSelection, 0.70f);
        c[ImGuiCol_DockingEmptyBg]         = Theme::kWell;                  // an empty node reads as a void

        c[ImGuiCol_PlotLines]              = Theme::kGrab;
        c[ImGuiCol_PlotLinesHovered]       = Theme::kGrabActive;
        c[ImGuiCol_PlotHistogram]          = Theme::kAmber;                 // data mark, exempt (see above)
        c[ImGuiCol_PlotHistogramHovered]   = Theme::kAmberLight;

        // Chrome, not kPanelRaised: kPanelRaised is ALSO the row-hover fill
        // (HeaderHovered above), and a header band that shares its color
        // with a hovered row reads as another row -- the 2026-08-10 outliner
        // complaint. As chrome it frames the rows the way the title/menu
        // bars frame panels, and it matches the outliner's bottom status
        // bar (MenuBarBg), bracketing the panel in the same tone.
        c[ImGuiCol_TableHeaderBg]          = Theme::kChrome;
        c[ImGuiCol_TableBorderStrong]      = Theme::kSeparatorHot;
        c[ImGuiCol_TableBorderLight]       = Theme::kSeparator;
        c[ImGuiCol_TableRowBg]             = Theme::kNone;
        // Row striping is a WHITE wash over whatever is behind it; at stock's
        // 0.06 it reads as a stripe on this darker panel, so it is halved.
        c[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

        c[ImGuiCol_TextLink]               = Theme::kCheck;                 // link affordance is the underline
        c[ImGuiCol_TextSelectedBg]         = Theme::WithAlpha(Theme::kSelection, 0.80f);
        c[ImGuiCol_TreeLines]              = Theme::kSeparator;

        c[ImGuiCol_DragDropTarget]         = Theme::WithAlpha(Theme::kAmber, 0.90f);
        c[ImGuiCol_DragDropTargetBg]       = Theme::kNone;
        c[ImGuiCol_UnsavedMarker]          = Theme::kText;

        c[ImGuiCol_NavCursor]              = Theme::kGrab;
        c[ImGuiCol_NavWindowingHighlight]  = Theme::WithAlpha(Theme::kText, 0.70f);
        // Stock dims with a light gray wash (0.80 gray) -- on a dark editor
        // that LIGHTENS the screen behind a modal. Dim toward black instead.
        c[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.02f, 0.02f, 0.02f, 0.55f);
        c[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.02f, 0.02f, 0.02f, 0.55f);

        // The first of TWO metrics this theme changes. Default is 0
        // (imgui.cpp:1533): with no frame border a near-black well on a dark
        // panel has only its fill to separate it, and small fields lose their
        // edge entirely. One pixel of kBorder (darker than both) is the inset
        // line the reference shows around every field. Everything else --
        // FrameRounding 0, GrabRounding 0, the paddings -- is left at ImGui's
        // default, which is already the near-square shape the reference wants.
        style.FrameBorderSize = 1.0f;

        // The second: kill the dock node's OWN close button (the X at the
        // right end of every tab bar, which closes the node's visible window).
        // Each docked tab already carries its own X (window->HasCloseButton,
        // per-tab at imgui.cpp:19661, independent of this) -- two X's per
        // panel read as clutter, and the corner one closes whichever tab
        // happens to be selected, which is never what the user aimed at.
        style.DockingNodeHasCloseButton = false;
    }
}

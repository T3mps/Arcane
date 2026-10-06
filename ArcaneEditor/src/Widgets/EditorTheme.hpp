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
// TWO non-gray hues. kSelection, a muted desaturated blue-gray, marks what is
// SELECTED (selected rows/items, selected text, the docking preview). kAccent,
// a brighter steel blue, marks what is ON / ACTIVE / PLAYING (toggle-on fills,
// the selected tab's overline, Play presence -- node page phase s6.1).
// Everything else is neutral gray.
//
// Domain color-coding is deliberately NOT monochrome: the X/Y/Z axes, the
// inspector header bands, the acting-on frame and the colour picker's channel
// markers keep their hues as palette tokens below (settings S4-3 / S6-26);
// the input pills, asset kinds and camera frame are editor.theme.* cvars of
// their own (Settings/EditorThemeSettings.hpp); the shader graph's typed pin
// dots and node accents live with the graph (ShaderEditorDocument.cpp).
// UE does the same -- the monochrome rule governs CHROME, not data.
//
// All values are DISPLAY-REFERRED: the editor's ImGui pass draws post-tonemap
// straight into the backbuffer (imgui.hlsl), so these are literally what the
// user sees. Hex comments are the 8-bit spelling of the float triple.
//
// Header-only and free of every editor type on purpose: ApplyEditorTheme takes
// the ImGuiStyle to fill, so any Arcane ImGui consumer (a game's debug HUD, a
// future tool host) can adopt the same look with one call. Callers today:
// the editor, and ArcaneCrashReporter (ReporterWindow.cpp), which reaches
// this header through a bare `ArcaneEditor/src` include path -- a shared
// header-only home for it is owed (crash-window spec s13).

#include <imgui.h>
#include <cmath>

namespace Arcane::Editor
{
    namespace Theme
    {
        // Same tone at a different alpha, so a translucent entry still names
        // the tone it comes from instead of repeating its channels. constexpr
        // because ImVec4's 4-float constructor is (imgui.h:317).
        constexpr ImVec4 WithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

        // WCAG 2.x contrast ratio of two opaque sRGB colours (alpha ignored),
        // 1..21. Pure. The node page's category-colour test (s5.1.11) and
        // T4's dim-text test (s6.6) both measure with it.
        [[nodiscard]] inline float ContrastRatio(const ImVec4& a, const ImVec4& b)
        {
            const auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
            const auto lum = [&](const ImVec4& c) { return 0.2126f * lin(c.x) + 0.7152f * lin(c.y) + 0.0722f * lin(c.z); };
            const float la = lum(a), lb = lum(b);
            const float hi = la > lb ? la : lb, lo = la > lb ? lb : la;
            return (hi + 0.05f) / (lo + 0.05f);
        }

        // ---- THE PALETTE (settings arc S4, spec s7.1) -------------------
        // Every colour token the editor draws with. kDarkPalette is today's
        // theme and the default of EditorThemeSettings; Live() is the palette
        // in force, swapped by the editor when a theme cvar publishes
        // (Settings/AppearanceApplier). The named tokens below are constant
        // REFERENCES into the live palette, so every existing Theme::kX read
        // follows a re-theme with no call-site change. Hosts without a
        // registry (ArcaneCrashReporter) never swap it and draw Dark.
        struct Palette
        {
            ImVec4 chromeDeep, chrome;
            ImVec4 panel, panelRaised;
            ImVec4 well, wellHovered, wellActive;
            ImVec4 button, buttonHovered, buttonActive;
            ImVec4 selection;
            ImVec4 accent, accentHovered, accentActive;
            ImVec4 text, textDim, border, separator, separatorHot, separatorHeld;
            ImVec4 grab, grabActive, check;
            ImVec4 amber, amberLight, error, warning;
            ImVec4 axisX, axisY, axisZ;
            ImVec4 modalDim, rowStripe, actingOnFrame;
            ImVec4 headerBand, headerBandHovered, headerBandActive;
            ImVec4 channelR, channelG, channelB, channelW;
        };

        // The three tonal layers, the two hues and the data marks described at
        // the top of this file. Every value is today's, verbatim.
        inline constexpr Palette kDarkPalette = {
            ImVec4(0.047f, 0.047f, 0.047f, 1.00f),   // chromeDeep    #0c0c0c  title bars, scrollbar track
            ImVec4(0.098f, 0.098f, 0.098f, 1.00f),   // chrome        #191919  menu bar, popups, table headers
            ImVec4(0.118f, 0.118f, 0.118f, 1.00f),   // panel         #1e1e1e  base surface
            ImVec4(0.165f, 0.165f, 0.165f, 1.00f),   // panelRaised   #2a2a2a  row/tab hover
            ImVec4(0.071f, 0.071f, 0.071f, 1.00f),   // well          #121212  input wells (darker than panel: inset)
            ImVec4(0.094f, 0.094f, 0.094f, 1.00f),   // wellHovered   #181818
            ImVec4(0.110f, 0.110f, 0.110f, 1.00f),   // wellActive    #1c1c1c
            ImVec4(0.184f, 0.184f, 0.184f, 1.00f),   // button        #2f2f2f
            ImVec4(0.239f, 0.239f, 0.239f, 1.00f),   // buttonHovered #3d3d3d
            ImVec4(0.294f, 0.294f, 0.294f, 1.00f),   // buttonActive  #4b4b4b
            ImVec4(0.180f, 0.251f, 0.325f, 1.00f),   // selection     #2e4053
            ImVec4(0.357f, 0.498f, 0.651f, 1.00f),   // accent        #5b7fa6  (bars pinned by EditorThemeContrastTest)
            ImVec4(0.388f, 0.525f, 0.678f, 1.00f),   // accentHovered #6386ad
            ImVec4(0.322f, 0.463f, 0.612f, 1.00f),   // accentActive  #52769c
            ImVec4(0.878f, 0.878f, 0.878f, 1.00f),   // text          #e0e0e0
            ImVec4(0.557f, 0.557f, 0.557f, 1.00f),   // textDim       #8e8e8e (5.09:1 on panel)
            ImVec4(0.051f, 0.051f, 0.051f, 1.00f),   // border        #0d0d0d
            ImVec4(0.200f, 0.200f, 0.200f, 1.00f),   // separator     #333333
            ImVec4(0.290f, 0.290f, 0.290f, 1.00f),   // separatorHot  #4a4a4a
            ImVec4(0.431f, 0.431f, 0.431f, 1.00f),   // separatorHeld #6e6e6e
            ImVec4(0.604f, 0.604f, 0.604f, 1.00f),   // grab          #9a9a9a
            ImVec4(0.784f, 0.784f, 0.784f, 1.00f),   // grabActive    #c8c8c8
            ImVec4(0.831f, 0.831f, 0.831f, 1.00f),   // check         #d4d4d4
            ImVec4(1.000f, 0.650f, 0.100f, 1.00f),   // amber         drop target, histogram, the selection outline
            ImVec4(1.000f, 0.780f, 0.350f, 1.00f),   // amberLight
            ImVec4(0.900f, 0.350f, 0.350f, 1.00f),   // error         #e65959
            ImVec4(0.950f, 0.770f, 0.300f, 1.00f),   // warning       #f2c44d
            ImVec4(196.0f / 255.0f,  64.0f / 255.0f,  54.0f / 255.0f, 1.0f),   // axisX  (the inspector's old kAxisBarColors[0])
            ImVec4( 96.0f / 255.0f, 166.0f / 255.0f,  58.0f / 255.0f, 1.0f),   // axisY
            ImVec4( 58.0f / 255.0f, 122.0f / 255.0f, 196.0f / 255.0f, 1.0f),   // axisZ
            ImVec4(0.02f, 0.02f, 0.02f, 0.55f),      // modalDim      dims toward black, not stock's 0.80 gray
            ImVec4(1.00f, 1.00f, 1.00f, 0.03f),      // rowStripe     a white wash, halved from stock's 0.06
            ImVec4(0x7a / 255.0f, 0x5a / 255.0f, 0x20 / 255.0f, 1.0f),   // actingOnFrame #7a5a20  the amber pill / card frame border
            ImVec4(48.0f / 255.0f, 48.0f / 255.0f, 52.0f / 255.0f, 1.0f),   // headerBand        #303034  inspector component headers
            ImVec4(58.0f / 255.0f, 58.0f / 255.0f, 64.0f / 255.0f, 1.0f),   // headerBandHovered #3a3a40
            ImVec4(66.0f / 255.0f, 66.0f / 255.0f, 73.0f / 255.0f, 1.0f),   // headerBandActive  #424249
            ImVec4(240.0f / 255.0f,  20.0f / 255.0f,  20.0f / 255.0f, 1.0f),   // channelR  the colour picker's channel markers
            ImVec4( 20.0f / 255.0f, 240.0f / 255.0f,  20.0f / 255.0f, 1.0f),   // channelG  (ImGui's GDefaultRgbaColorMarkers)
            ImVec4( 20.0f / 255.0f,  20.0f / 255.0f, 240.0f / 255.0f, 1.0f),   // channelB
            ImVec4(140.0f / 255.0f, 140.0f / 255.0f, 140.0f / 255.0f, 1.0f),   // channelW
        };

        namespace Detail { inline constinit Palette g_live = kDarkPalette; }

        [[nodiscard]] inline const Palette& Live() noexcept { return Detail::g_live; }
        inline void SetLivePalette(const Palette& p) noexcept { Detail::g_live = p; }

        // Tests and previews: swap the live palette for a scope, restore after.
        struct [[nodiscard]] ScopedLivePalette
        {
            explicit ScopedLivePalette(const Palette& p) noexcept : m_saved(Detail::g_live) { Detail::g_live = p; }
            ~ScopedLivePalette() { Detail::g_live = m_saved; }
            ScopedLivePalette(const ScopedLivePalette&) = delete;
            ScopedLivePalette& operator=(const ScopedLivePalette&) = delete;
        private:
            Palette m_saved;
        };

        // The named tokens (meaning per kDarkPalette's comments). References,
        // so `constexpr ImVec4 x = Theme::kPanel;` no longer compiles: write
        // `const ImVec4&` (follows the theme) at such a site.
        inline constexpr const ImVec4& kChromeDeep    = Detail::g_live.chromeDeep;
        inline constexpr const ImVec4& kChrome        = Detail::g_live.chrome;
        inline constexpr const ImVec4& kPanel         = Detail::g_live.panel;
        inline constexpr const ImVec4& kPanelRaised   = Detail::g_live.panelRaised;
        inline constexpr const ImVec4& kWell          = Detail::g_live.well;
        inline constexpr const ImVec4& kWellHovered   = Detail::g_live.wellHovered;
        inline constexpr const ImVec4& kWellActive    = Detail::g_live.wellActive;
        inline constexpr const ImVec4& kButton        = Detail::g_live.button;
        inline constexpr const ImVec4& kButtonHovered = Detail::g_live.buttonHovered;
        inline constexpr const ImVec4& kButtonActive  = Detail::g_live.buttonActive;
        inline constexpr const ImVec4& kSelection     = Detail::g_live.selection;
        inline constexpr const ImVec4& kAccent        = Detail::g_live.accent;
        inline constexpr const ImVec4& kAccentHovered = Detail::g_live.accentHovered;
        inline constexpr const ImVec4& kAccentActive  = Detail::g_live.accentActive;
        // The lit state of an IconToggle (s4.9): DERIVED aliases of the accent trio.
        inline constexpr const ImVec4& kToggleOn        = Detail::g_live.accent;
        inline constexpr const ImVec4& kToggleOnHovered = Detail::g_live.accentHovered;
        inline constexpr const ImVec4& kToggleOnActive  = Detail::g_live.accentActive;
        inline constexpr const ImVec4& kText          = Detail::g_live.text;
        inline constexpr const ImVec4& kTextDim       = Detail::g_live.textDim;
        inline constexpr const ImVec4& kBorder        = Detail::g_live.border;
        inline constexpr const ImVec4& kSeparator     = Detail::g_live.separator;
        inline constexpr const ImVec4& kSeparatorHot  = Detail::g_live.separatorHot;
        inline constexpr const ImVec4& kSeparatorHeld = Detail::g_live.separatorHeld;
        inline constexpr const ImVec4& kGrab          = Detail::g_live.grab;
        inline constexpr const ImVec4& kGrabActive    = Detail::g_live.grabActive;
        inline constexpr const ImVec4& kCheck         = Detail::g_live.check;
        inline constexpr const ImVec4& kAmber         = Detail::g_live.amber;
        inline constexpr const ImVec4& kAmberLight    = Detail::g_live.amberLight;
        inline constexpr const ImVec4& kError         = Detail::g_live.error;
        inline constexpr const ImVec4& kWarning       = Detail::g_live.warning;
        inline constexpr const ImVec4& kAxisX         = Detail::g_live.axisX;
        inline constexpr const ImVec4& kAxisY         = Detail::g_live.axisY;
        inline constexpr const ImVec4& kAxisZ         = Detail::g_live.axisZ;
        inline constexpr const ImVec4& kModalDim      = Detail::g_live.modalDim;
        inline constexpr const ImVec4& kRowStripe     = Detail::g_live.rowStripe;
        inline constexpr const ImVec4& kActingOnFrame = Detail::g_live.actingOnFrame;
        inline constexpr const ImVec4& kHeaderBand        = Detail::g_live.headerBand;
        inline constexpr const ImVec4& kHeaderBandHovered = Detail::g_live.headerBandHovered;
        inline constexpr const ImVec4& kHeaderBandActive  = Detail::g_live.headerBandActive;
        inline constexpr const ImVec4& kChannelR      = Detail::g_live.channelR;
        inline constexpr const ImVec4& kChannelG      = Detail::g_live.channelG;
        inline constexpr const ImVec4& kChannelB      = Detail::g_live.channelB;
        inline constexpr const ImVec4& kChannelW      = Detail::g_live.channelW;

        // The unfocused dock's selected-tab overline: the accent at this
        // alpha. Dark's value and EditorThemeSettings::unfocusedOverlineAlpha's
        // default (editor.theme.unfocusedOverlineAlpha).
        inline constexpr float kDarkUnfocusedOverlineAlpha = 0.45f;

        // Fully transparent: "draw nothing here". A CONSTANT, not a token (any change is a bug).
        inline constexpr ImVec4 kNone = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    }

    // Apply the theme to `style`. Fills ALL of ImGuiCol_COUNT (63 entries in
    // the vendored 1.92.9, imgui.h:1821-1886): StyleColorsDark runs first so a
    // future upstream entry has a sane value the day it appears, then every
    // entry that exists today is overwritten below. Call once at boot, before
    // the first frame, on the context that will use it.
    inline void ApplyEditorThemeColors(ImGuiStyle& style, float unfocusedOverlineAlpha = Theme::kDarkUnfocusedOverlineAlpha)
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
        c[ImGuiCol_TitleBgActive]          = Theme::kChromeDeep;            // focus never re-tones the well: the overline alone marks it (user, 2026-10-02)
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
        // CollapsingHeader (imgui.h:1848). Selected takes kSelection (not
        // kAccent: selected is not "on", s6.1); hover and held stay gray, so
        // hovering an unselected row never flashes a second hue. The
        // inspector's category bands push their own trio over this one
        // (EditorWidgets.cpp, PushHeaderBandColors).
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

        // Tabs, Visual Studio's language (user, 2026-10-02): the SELECTED tab is
        // a real tab in the panel tone (it and the body under it are one
        // surface); an UNSELECTED tab draws no fill at all -- only its label,
        // dimmed (the ImGui local fix in TabItemLabelAndCloseButton draws it in
        // TextDisabled), sitting on the strip; hover lifts it one step of panel.
        c[ImGuiCol_TabHovered]             = Theme::kPanelRaised;
        c[ImGuiCol_Tab]                    = Theme::kNone;                  // label only
        c[ImGuiCol_TabSelected]            = Theme::kPanel;
        c[ImGuiCol_TabSelectedOverline]    = Theme::kAccent;                // selected == accent (s6.1)
        c[ImGuiCol_TabDimmed]              = Theme::kNone;                  // == Tab: focus never re-tones a tab
        c[ImGuiCol_TabDimmedSelected]      = Theme::kPanel;                 // == TabSelected: only the overline dims
        // Every dock node marks its active tab; an unfocused one at 45%
        // (composite #374758, 1.85:1 on its #191919 tab: quieter than the
        // focused overline, still brighter than the pre-s6.1 focused one).
        c[ImGuiCol_TabDimmedSelectedOverline] = Theme::WithAlpha(Theme::kAccent, unfocusedOverlineAlpha);

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
        c[ImGuiCol_TableRowBgAlt]          = Theme::kRowStripe;

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
        c[ImGuiCol_NavWindowingDimBg]      = Theme::kModalDim;
        c[ImGuiCol_ModalWindowDimBg]       = Theme::kModalDim;
    }

    // The full look: the colours above, then the six metrics. Boot and the
    // crash reporter call this; a live re-theme calls ApplyEditorThemeColors
    // alone so the UI scale's metrics (S4-15) are never reset. WindowPadding
    // (4,4) is the user-requested inset (controller 2026-10-05) and stays.
    inline void ApplyEditorTheme(ImGuiStyle& style)
    {
        ApplyEditorThemeColors(style);

        // The first of SIX metrics this theme changes (FrameBorderSize,
        // DockingNodeHasCloseButton, TabBarOverlineSize, DisabledAlpha,
        // TabRounding, WindowPadding). Default
        // is 0 (imgui.cpp:1533): with no frame border a near-black well on a dark
        // panel has only its fill to separate it, and small fields lose their
        // edge entirely. One pixel of kBorder (darker than both) is the inset
        // line the reference shows around every field. Everything else --
        // FrameRounding 0, GrabRounding 0, the frame/item paddings -- is left at ImGui's
        // default, which is already the near-square shape the reference wants.
        style.FrameBorderSize = 1.0f;

        // The second: kill the dock node's OWN close button (the X at the
        // right end of every tab bar, which closes the node's visible window).
        // Each docked tab already carries its own X (window->HasCloseButton,
        // per-tab at imgui.cpp:19661, independent of this) -- two X's per
        // panel read as clutter, and the corner one closes whichever tab
        // happens to be selected, which is never what the user aimed at.
        style.DockingNodeHasCloseButton = false;

        // The third: a 2 px tab overline (ImGui's default is 1, imgui.cpp:1555).
        // The overline is drawn over the tab fill (imgui_widgets.cpp:10883-10898),
        // and the fill ramp itself (TabSelected kPanel vs Tab kChrome) is
        // unchanged, so this line carries focus. ScaleAllSizes DPI-scales it
        // (imgui.cpp:1638) -- the crash reporter, which applies this theme
        // (ReporterWindow.cpp:535-536), gets the same line.
        style.TabBarOverlineSize = 2.0f;

        // The fourth: DisabledAlpha 0.6 -> 0.45 (s6.6). At ImGui's stock 0.6
        // (imgui.cpp:1519) disabled kText composites to #929292, BRIGHTER than
        // kTextDim -- raising dim text alone would make the two indistinguishable.
        // At 0.45 disabled kText is #757575 (3.62:1), a step under dim text.
        style.DisabledAlpha = 0.45f;

        // The fifth: TabRounding 5 -> 2 (user, 2026-10-02: "reduce the rounding
        // on tabs"). ImGui's stock radius (imgui.cpp:1548) rounds a 2 px accent
        // overline into a pill on short tabs; 2 px keeps a hint of a corner and
        // reads closer to the near-square frames. ScaleAllSizes DPI-scales it
        // (imgui.cpp:1631).
        style.TabRounding = 2.0f;

        // The sixth: WindowPadding 8 -> 4 (user, 2026-10-04: "less than it was,
        // maybe 4px"). ImGui's stock (8,8) (imgui.cpp:1520) insets every panel's
        // content by a visible margin; Unreal's dock tab content area pads 0
        // (SDockTab ContentPadding, SDockTab.h:97) and each panel insets itself by
        // a few px, so 4 everywhere -- panels, popups, menus, tooltips -- is the
        // nearest single value. ScaleAllSizes DPI-scales it.
        style.WindowPadding = ImVec2(4.0f, 4.0f);
    }
}

#include "ShaderEditorDocument.hpp"

#include "AssetBrowser.hpp"
#include "EditorTheme.hpp"
#include "GraphGridPass.hpp"
#include "MaterialParamWidgets.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Material/MaterialGraph.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/ShaderConventions.hpp>

#include <imgui.h>
// AddSettingsHandler / FindSettingsHandler (:3502-3504), ImGuiSettingsHandler
// (:2212-2225) and MarkIniSettingsDirty (:3499) are internal-only -- ImGui's
// ini extension point has never been in the public header.
#include <imgui_internal.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
        Arcane::MaterialSurface SurfaceOf(int surface)
        {
            return surface == 1 ? Arcane::MaterialSurface::Sprite
                                : Arcane::MaterialSurface::Fullscreen;
        }

        // One param edit as an undo step. The live edit already happened (the
        // ICommand contract); Undo restores the BEFORE override state (value or
        // no-override), Redo re-applies the AFTER. Doc-identity (review M3): the
        // command holds the DOCUMENT weakly and forwards by name hash to its
        // CURRENT instance -- a recompile swaps the instance but the step stays
        // live; closing the document expires the anchor (the step goes inert).
        class ParamEditCommand final : public Arcane::ICommand
        {
        public:
            ParamEditCommand(std::weak_ptr<ShaderEditorDocument*> anchor,
                             std::uint32_t nameHash, std::string label,
                             bool hadBefore, Arcane::MatParamValue before,
                             bool hasAfter, Arcane::MatParamValue after)
                : m_anchor(std::move(anchor)), m_nameHash(nameHash),
                  m_label(std::move(label)), m_hadBefore(hadBefore),
                  m_before(before), m_hasAfter(hasAfter), m_after(after)
            {
            }

            void Undo() override { Apply(m_hadBefore, m_before); }
            void Redo() override { Apply(m_hasAfter, m_after); }
            const char* Label() const override { return m_label.c_str(); }

        private:
            void Apply(bool hasValue, const Arcane::MatParamValue& value)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplyParamEdit(m_nameHash, hasValue, value);
            }

            std::weak_ptr<ShaderEditorDocument*> m_anchor;
            std::uint32_t m_nameHash;
            std::string   m_label;
            bool          m_hadBefore;
            Arcane::MatParamValue m_before;
            bool          m_hasAfter;
            Arcane::MatParamValue m_after;
        };

        std::uint64_t StageKey(const Arcane::Guid& id, bool vertex,
                               std::size_t pass = 0)
        {
            // Coalesce per (document, stage, pass): a newer submit of the SAME
            // stage supersedes; stages never cancel each other. The pass bits
            // sit at << 8, clear of the stage bits and the sprite cache's
            // guid^0x4/0x8 keys.
            return (id.hi ^ (id.lo * 1099511628211ull)) ^ (vertex ? 0x1u : 0x2u) ^
                   (static_cast<std::uint64_t>(pass) << 8);
        }

        // Node-preview compiles coalesce per (document, pass, node), in a key
        // band the StageKey space never reaches (salt in the top bits).
        std::uint64_t NodePreviewKey(const Arcane::Guid& id, std::size_t pass,
                                     std::uint32_t node)
        {
            std::uint64_t h = (id.hi ^ (id.lo * 1099511628211ull)) ^
                              0x9E3779B97F4A7C15ull;
            h = (h ^ (static_cast<std::uint64_t>(pass) << 32) ^ node) *
                1099511628211ull;
            return h;
        }

        // ---- Graph canvas plumbing (Slice 9) ----
        namespace ed = ax::NodeEditor;

        // Pass-canvas fixed ids (chain index c = node id c+1; these sit far
        // above any realistic pass count).
        constexpr std::uint32_t kPassOutputNodeId = 900000;
        constexpr std::uint32_t kPassSceneNodeId  = 900001;   // the Scene source
        constexpr std::uint32_t kPassOutputLinkId = 800000;

        // Pin id encoding: node id * 1000 + slot band. Inputs at +1.., outputs
        // at +501.. (a node type never has anywhere near 500 pins).
        constexpr std::uint64_t kPinInBase = 1, kPinOutBase = 501;
        ed::PinId InPin(std::uint32_t node, std::uint32_t pin)
        { return ed::PinId(node * 1000ull + kPinInBase + pin); }
        ed::PinId OutPin(std::uint32_t node, std::uint32_t pin)
        { return ed::PinId(node * 1000ull + kPinOutBase + pin); }
        struct DecodedPin
        {
            std::uint32_t node = 0, pin = 0;
            bool isInput = false, valid = false;
        };
        DecodedPin DecodePin(ed::PinId id)
        {
            DecodedPin d;
            if (!id)
                return d;
            const std::uint64_t v = static_cast<std::uint64_t>(id.Get());
            d.node = static_cast<std::uint32_t>(v / 1000ull);
            const std::uint64_t rem = v % 1000ull;
            d.isInput = rem < kPinOutBase;
            d.pin = static_cast<std::uint32_t>(d.isInput ? rem - kPinInBase
                                                         : rem - kPinOutBase);
            d.valid = d.node != 0;
            return d;
        }

        // -------------------------------------------------------------------
        // GRAPH CANVAS PALETTE -- the single place the canvas and node colors
        // live, on the same rule EditorWidgets.cpp:239-253 states for the
        // inspector's constants: no magic colors inside draw calls.
        //
        // All values are DISPLAY-REFERRED: ImGui draws post-tonemap into the
        // backbuffer and samples the backdrop RT straight through
        // (imgui.hlsl:1-5), so these are what the user sees.
        //
        // Tones follow the Unity Shader Graph reference -- a near-flat dark
        // canvas, a node body one step above it, a title band one step below
        // the body, a border one step above the body again.
        // -------------------------------------------------------------------
        // The canvas surface IS the editor's panel tone, not a second opinion
        // about it: a graph document's body is the same flat dark surface every
        // other panel body is. Referencing the theme constant keeps them from
        // drifting apart; the value is unchanged (#1e1e1e), so the approved
        // canvas look is untouched.
        constexpr ImVec4 kCanvasColor      = Theme::kPanel;                        // #1e1e1e
        constexpr ImVec4 kGridMinorColor   = ImVec4(0.180f, 0.180f, 0.196f, 0.55f);
        constexpr ImVec4 kGridMajorColor   = ImVec4(0.235f, 0.235f, 0.255f, 0.90f);
        constexpr ImVec4 kNodeBodyColor    = ImVec4(0.176f, 0.176f, 0.188f, 1.0f); // #2d2d30
        constexpr ImVec4 kNodeTitleColor   = ImVec4(0.137f, 0.137f, 0.149f, 1.0f); // #232326
        constexpr ImVec4 kNodeBorderColor  = ImVec4(0.243f, 0.243f, 0.267f, 1.0f);
        constexpr ImVec4 kNodeTitleText    = ImVec4(0.808f, 0.808f, 0.831f, 1.0f);
        constexpr ImVec4 kNodeBadgeText    = ImVec4(1.0f,   0.4f,   0.3f,   1.0f);
        // Selection/hover reuse the viewport's outline language so one accent
        // means "selected" everywhere in the editor (SelectionOutline.hpp:47-48).
        constexpr ImVec4 kNodeSelBorder    = ImVec4(1.0f,  0.65f, 0.10f, 1.0f);   // amber
        constexpr ImVec4 kNodeHovBorder    = ImVec4(0.25f, 0.70f, 1.0f,  1.0f);   // cyan
        constexpr ImVec4 kGroupBgColor     = ImVec4(0.220f, 0.220f, 0.235f, 0.25f);
        constexpr ImVec4 kGroupBorderColor = ImVec4(0.290f, 0.290f, 0.310f, 0.60f);

        // Pin/wire colors by PIN WIDTH. Unity's convention mapped onto Arcane's
        // pin domain, which is 1 / 2 / 4 / 0-means-dynamic
        // (GraphPinDesc::width, MaterialGraph.hpp:150-154). Unity's vec3-yellow
        // and texture-red-orange entries have no counterpart here: Arcane has
        // no 3-lane pin and textures are params, not pins -- so those two rows
        // of the reference table are deliberately absent rather than mapped
        // onto something they do not mean.
        constexpr ImVec4 kPinScalarColor  = ImVec4(0.502f, 0.808f, 1.0f,   1.0f); // pale azure
        constexpr ImVec4 kPinVec2Color    = ImVec4(0.549f, 0.863f, 0.549f, 1.0f); // green
        constexpr ImVec4 kPinVec4Color    = ImVec4(0.941f, 0.549f, 0.863f, 1.0f); // magenta
        constexpr ImVec4 kPinDynamicColor = ImVec4(0.745f, 0.745f, 0.765f, 1.0f); // gray

        // Node geometry (canvas units at zoom 1).
        constexpr float kNodeRounding    = 4.0f;
        constexpr float kNodeBorderWidth = 1.0f;
        constexpr float kNodeHovBorderWidth = 1.5f;
        constexpr float kNodeSelBorderWidth = 2.0f;
        constexpr float kNodePadX = 10.0f;
        constexpr float kNodePadY = 6.0f;
        // Breathing room between the BOTTOM EDGE OF THE TITLE BAND and the first
        // body row. Not the same thing as kNodePadY: that one is the band's own
        // internal padding (how far the band extends past the title text), this
        // one is body space below the band. Without it the first pin row does
        // not merely sit flush -- it renders INSIDE the band, because ImGui
        // places the next item one ItemSpacing.y (4 px) under the title text
        // while the band reaches kNodePadY (6 px) under it.
        //
        // Canvas units, like every other constant here: everything inside
        // ed::Begin/End is authored in canvas space, so this scales with zoom on
        // its own and must not be pre-multiplied by anything.
        constexpr float kNodeHeaderGap = 5.0f;
        constexpr float kPinDotRadius   = 4.0f;
        constexpr float kPinRingWidth   = 1.6f;
        constexpr int   kPinDotSegments = 12;
        constexpr float kWireThickness  = 2.0f;

        // ---- Gradient wires -------------------------------------------------
        // ed::Link paints one flat colour, so a wire cannot say "this end is a
        // float, that end is a float2" the way its two dots do. We therefore
        // submit the link with a FULLY TRANSPARENT colour and draw the curve
        // ourselves in the library's own link layer.
        //
        // Alpha 0 costs nothing and breaks nothing. The draw helper returns
        // immediately on `if ((color >> 24) == 0)`
        // (imgui_node_editor.cpp:494-495), so the flat wire is never
        // tessellated. Registration ignores the colour entirely -- DoLink
        // stores it and calls UpdateEndpoints unconditionally
        // (imgui_node_editor.cpp:1648-1653) -- and every hit path
        // (Link::TestHit :984-1032, FindLinkAt :2240-2247) reads only the
        // geometry and m_Thickness. So hover, selection, rect-select and the
        // delete flow are untouched, and the thickness we pass still has to be
        // the REAL one or the wire would be hard to grab.
        //
        // Hover/selection feedback also survives on its own: those passes use
        // StyleColor_HovLinkBorder / StyleColor_SelLinkBorder, not the link's
        // colour (imgui_node_editor.cpp:899-929), and land in
        // c_LinkChannel_Selection, one channel BELOW the links -- so they stay
        // a halo behind our gradient exactly as they were behind the flat wire.
        //
        // c_LinkChannel_Links is a file-static in the vendored translation unit
        // (imgui_node_editor.cpp:130-131), so it cannot be named from here; it
        // is reproduced from the constants it is built out of
        // (imgui_node_editor.cpp:113-121). Reproduced rather than guessed:
        // c_UserLayerChannelStart(0) + c_UserLayersCount(5) =
        // c_BackgroundChannelStart(5), + c_BackgroundChannelCount(1) =
        // c_LinkStartChannel(6), + 1 = 7.
        //
        // Retargeting the channel is not optional. Between ed::Begin and
        // ed::End but outside a node, the current channel is m_ExternalChannel
        // (imgui_node_editor.cpp:1191-1194), which is 0 -- the BOTTOM of the
        // merge, under the grid's own opaque background fill
        // (imgui_node_editor.cpp:1512). Wires drawn there would simply be
        // painted over. The other reachable layer, GetNodeBackgroundDrawList,
        // is a per-node channel and sits ABOVE the links, so wires would cross
        // in front of node bodies. Channel 7 is the only one that puts them
        // where the flat wires were: above group nodes, below regular nodes
        // (the End reshuffle keeps the four link channels contiguous and in
        // order, imgui_node_editor.cpp:1488-1492).
        //
        // The index is only meaningful DURING submission -- End swaps the
        // channels into their final z-order -- so this must run inside
        // ed::Begin/End, which it does (the link loop is in DrawGraphPanel).
        constexpr int kLinkChannelLinks = 7;

        // Cubic bezier at t. Same evaluation the library tessellates
        // (ImCubicBezier* in imgui_bezier_math.inl); we need per-segment points
        // because the colour changes along the curve.
        ImVec2 CubicBezierAt(const ImVec2& p0, const ImVec2& p1,
                             const ImVec2& p2, const ImVec2& p3, float t) noexcept
        {
            const float u = 1.0f - t;
            const float w0 = u * u * u;
            const float w1 = 3.0f * u * u * t;
            const float w2 = 3.0f * u * t * t;
            const float w3 = t * t * t;
            return ImVec2(p0.x * w0 + p1.x * w1 + p2.x * w2 + p3.x * w3,
                          p0.y * w0 + p1.y * w1 + p2.y * w2 + p3.y * w3);
        }

        // Straight sRGB lerp. The pin palette is four light, low-saturation
        // tones, so the midpoints stay clean without an OkLab detour; the one
        // pairing that could band (azure -> magenta) crosses through a plausible
        // lavender rather than through grey.
        ImVec4 LerpColor(const ImVec4& a, const ImVec4& b, float t) noexcept
        {
            return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                          a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
        }

        // Hover/selection already reads through the library's halo (see
        // DrawGradientWire); this lifts the wire itself the same way a
        // highlighted dot lifts, so the emphasis lands on the whole run.
        ImVec4 BrightenColor(const ImVec4& c) noexcept
        {
            return LerpColor(c, ImVec4(1.0f, 1.0f, 1.0f, c.w), 0.25f);
        }

        // -------------------------------------------------------------------
        // ZOOM STOPS -- Unreal's graph-editor table, ported exactly.
        //
        // These are FFixedZoomLevelsContainer's 20 entries, verbatim and in
        // order (vendored UE at Arcane/.example/UnrealEngine-release/Engine/
        // Source/Editor/GraphEditor/Private/SNodePanel.cpp:53-75). UE calls the
        // number ZoomAmount and it is a VIEW SCALE: 1.000 is 1:1, 2.000 draws
        // everything twice as large. The vendored node editor's
        // ed::Config::CustomZoomLevels is the same quantity -- the table feeds
        // NavigateAction::m_ZoomLevels (imgui_node_editor.cpp:3333) and m_Zoom
        // is assigned view.Scale (:3639) -- so the numbers transfer with no
        // conversion. (ed::GetCurrentZoom, by contrast, hands back the
        // RECIPROCAL; see ViewScale() below.)
        //
        // Replacing the vendored default table (0.1 .. 8.0,
        // imgui_node_editor.cpp:3309-3312) is the point of doing this: 8x
        // magnification has no use on a node graph, UE's stops are much finer
        // in the readable band, and the LOD tiers are defined against exactly
        // these numbers.
        constexpr float kZoomLevels[] = {
            0.100f, 0.125f, 0.150f, 0.175f, 0.200f,
            0.225f, 0.250f, 0.375f, 0.500f, 0.675f,
            0.750f, 0.875f, 1.000f, 1.250f, 1.375f,
            1.500f, 1.675f, 1.750f, 1.875f, 2.000f,
        };

        // CustomZoomLevels is an ImVector, so the table is pushed in rather
        // than aggregate-initialized. The caller's `cfg` may die immediately
        // after CreateEditor: the editor holds a Config BY VALUE
        // (imgui_node_editor_internal.h:1486) and its ctor deep-copies through
        // ImVector::operator= (imgui_node_editor.cpp:5785-5789), so the pointer
        // NavigateAction caches at :3333 is into the editor's own copy.
        void ApplyZoomLevels(ed::Config& cfg)
        {
            cfg.CustomZoomLevels.reserve(static_cast<int>(std::size(kZoomLevels)));
            for (float z : kZoomLevels)
                cfg.CustomZoomLevels.push_back(z);
        }

        // The canvas's view scale, in the same units as kZoomLevels. THE TRAP:
        // ed::GetCurrentZoom returns InvScale -- canvas units per screen pixel
        // (imgui_node_editor_api.cpp:665-668) -- which is the reciprocal of the
        // scale everything else in this file means by "zoom". One helper, so
        // the flip is written once.
        //
        // Valid on either side of ed::Begin within a frame: Begin installs the
        // view the previous End computed (imgui_node_editor.cpp:1258) and the
        // navigate action only re-derives it during End, so both reads return
        // the scale this frame's nodes are actually drawn at.
        float ViewScale() noexcept
        {
            const float invScale = ed::GetCurrentZoom();
            return invScale > 0.0001f ? 1.0f / invScale : 1.0f;
        }

        // -------------------------------------------------------------------
        // RENDERING LOD BOUNDARIES -- the third column of UE's zoom table.
        //
        // Each constant is the LAST kZoomLevels entry belonging to that tier,
        // read straight off FFixedZoomLevelsContainer (SNodePanel.cpp:56-75):
        //   0.100 .. 0.200          LowestDetail
        //   0.225 .. 0.250          LowDetail
        //   0.375 .. 0.675          MediumDetail
        //   0.750 .. 1.375          DefaultDetail
        //   1.500 .. 2.000          FullyZoomedIn
        // UE indexes its table and looks the tier up by INDEX
        // (SNodePanel.cpp:1921); we compare the scale instead, because the
        // canvas can also sit BETWEEN stops -- ed::NavigateToContent /
        // NavigateToSelection fit a rectangle and land on an arbitrary scale
        // (imgui_node_editor.cpp:3516-3548), which an index lookup has no
        // answer for. Comparing covers both.
        constexpr float kLodLowestMax  = 0.200f;
        constexpr float kLodLowMax     = 0.250f;
        constexpr float kLodMediumMax  = 0.675f;
        constexpr float kLodDefaultMax = 1.375f;

        // The canvas's tier at a given view scale. A boundary value belongs to
        // the LOWER tier (0.200 is LowestDetail, not LowDetail), matching the
        // table; the epsilon only protects that from float round-trips through
        // the editor's zoom state.
        NodeLOD NodeLODForScale(float scale) noexcept
        {
            constexpr float kEps = 1e-4f;
            if (scale <= kLodLowestMax  + kEps) return NodeLOD::LowestDetail;
            if (scale <= kLodLowMax     + kEps) return NodeLOD::LowDetail;
            if (scale <= kLodMediumMax  + kEps) return NodeLOD::MediumDetail;
            if (scale <= kLodDefaultMax + kEps) return NodeLOD::DefaultDetail;
            return NodeLOD::FullyZoomedIn;
        }

        // ImVec4 -> the plain float[4] the grid CB mirrors.
        void FillRgba(float (&dst)[4], const ImVec4& c) noexcept
        {
            dst[0] = c.x;
            dst[1] = c.y;
            dst[2] = c.z;
            dst[3] = c.w;
        }

        ImVec4 PinColorForWidth(int width) noexcept
        {
            switch (width)
            {
                case 1:  return kPinScalarColor;
                case 2:  return kPinVec2Color;
                case 4:  return kPinVec4Color;
                default: return kPinDynamicColor;   // 0 = adapts to what feeds it
            }
        }

        // One port dot: FILLED when a wire is attached, a hollow ring when not
        // (the Shader Graph reading -- "this port carries something" is visible
        // without tracing the wire). Advances the cursor by exactly the dot, so
        // the caller follows with SameLine + the label. Returns the dot's CENTRE
        // in canvas space -- the pin rows anchor their wire pivot off it.
        ImVec2 DrawPinDot(const ImVec4& color, bool connected)
        {
            const float lineH = ImGui::GetTextLineHeight();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(kPinDotRadius * 2.0f, lineH));
            const ImVec2 c(p.x + kPinDotRadius, p.y + lineH * 0.5f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = ImGui::GetColorU32(color);
            if (connected)
                dl->AddCircleFilled(c, kPinDotRadius, col, kPinDotSegments);
            else
            {
                dl->AddCircleFilled(c, kPinDotRadius,
                                    ImGui::GetColorU32(kNodeBodyColor), kPinDotSegments);
                dl->AddCircle(c, kPinDotRadius, col, kPinDotSegments, kPinRingWidth);
            }
            return c;
        }

        // Horizontal spacer that right-aligns a row of `rowWidth` inside a
        // content column of `contentWidth`. Both are canvas units; a
        // non-positive slack draws nothing, which is what the first frame of a
        // brand-new node (no measured width yet) gets.
        void RightAlignRow(float contentWidth, float rowWidth)
        {
            const float slack = contentWidth - rowWidth;
            if (slack <= 1.0f)
                return;
            ImGui::Dummy(ImVec2(slack, 0.0f));
            ImGui::SameLine(0.0f, 0.0f);
        }

        // One-time style for a node-editor context. Written to the PERSISTENT
        // style (ed::GetStyle returns a mutable reference, imgui_node_editor.h:295)
        // instead of pushed per frame, because every value here is latched into
        // the object at BeginNode/BeginPin time (imgui_node_editor.cpp:5270-5278,
        // 5367-5377) -- one assignment covers every node for the context's life.
        void ApplyGraphCanvasStyle()
        {
            ed::Style& s = ed::GetStyle();
            // The vendored grid AND background fill are switched off; the
            // shader backdrop (graph_grid.hlsl) is blitted underneath instead.
            // Wholesale replacement is the only option available: the built-in
            // grid's 32 px spacing is a hardcoded local with no StyleVar and no
            // LOD fade (imgui_node_editor.cpp:1506-1517).
            s.Colors[ed::StyleColor_Grid] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            s.Colors[ed::StyleColor_Bg]   = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            s.Colors[ed::StyleColor_NodeBg]        = kNodeBodyColor;
            s.Colors[ed::StyleColor_NodeBorder]    = kNodeBorderColor;
            s.Colors[ed::StyleColor_HovNodeBorder] = kNodeHovBorder;
            s.Colors[ed::StyleColor_SelNodeBorder] = kNodeSelBorder;
            s.Colors[ed::StyleColor_GroupBg]       = kGroupBgColor;
            s.Colors[ed::StyleColor_GroupBorder]   = kGroupBorderColor;
            // A pin draws nothing of its own except a hover rect
            // (imgui_node_editor.cpp:575-594) -- that rectangle would fight the
            // dot, so its alpha goes to zero and the dot IS the pin visual.
            s.Colors[ed::StyleColor_PinRect]       = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            s.Colors[ed::StyleColor_PinRectBorder] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            s.NodeRounding            = kNodeRounding;
            s.NodeBorderWidth         = kNodeBorderWidth;
            s.HoveredNodeBorderWidth  = kNodeHovBorderWidth;
            s.SelectedNodeBorderWidth = kNodeSelBorderWidth;
            s.NodePadding = ImVec4(kNodePadX, kNodePadY, kNodePadX, kNodePadY);
        }

        // Re-key a saved-params entry old -> new. Merge rule (assisted rename):
        // when BOTH names exist the new-name value wins and the orphan drops.
        void RekeySavedParam(
            std::vector<std::pair<std::string, Arcane::MatParamValue>>& params,
            const std::string& oldName, const std::string& newName)
        {
            const auto oldIt = std::find_if(params.begin(), params.end(),
                [&](const auto& p) { return p.first == oldName; });
            if (oldIt == params.end())
                return;
            const bool hasNew = std::any_of(params.begin(), params.end(),
                [&](const auto& p) { return p.first == newName; });
            if (hasNew)
            {
                ARC_WARN("param rename: a '{}' value already exists -- dropping "
                         "the orphaned '{}' entry", newName, oldName);
                params.erase(oldIt);
            }
            else
                oldIt->first = newName;
        }

        // Case-insensitive substring match for the create-menu search field.
        bool ContainsInsensitive(const char* hay, const char* needle)
        {
            const std::size_t n = std::strlen(needle);
            if (n == 0)
                return true;
            for (const char* p = hay; *p; ++p)
            {
                std::size_t i = 0;
                while (i < n && p[i] &&
                       std::tolower(static_cast<unsigned char>(p[i])) ==
                           std::tolower(static_cast<unsigned char>(needle[i])))
                    ++i;
                if (i == n)
                    return true;
            }
            return false;
        }

        // Would adding data-flow edge from->to close a cycle? Yes iff `from` is
        // already downstream of `to`. (Codegen re-detects as the backstop; this
        // check is what makes the canvas REFUSE the wire silently, SG-style.)
        bool WouldCycle(const Arcane::MaterialGraph& g, std::uint32_t from, std::uint32_t to)
        {
            if (from == to)
                return true;
            std::vector<std::uint32_t> stack{ to };
            std::unordered_set<std::uint32_t> seen;
            while (!stack.empty())
            {
                const std::uint32_t cur = stack.back();
                stack.pop_back();
                if (cur == from)
                    return true;
                if (!seen.insert(cur).second)
                    continue;
                for (const Arcane::GraphLink& l : g.links)
                    if (l.fromNode == cur)
                        stack.push_back(l.toNode);
            }
            return false;
        }

        // ---- Inline pin literals (a value ON an unwired input pin) ----

        // Whether a pin takes a literal at all (the SEAM SCOPE exclusion list)
        // and how many lanes it stores are ENGINE predicates:
        // Arcane::GraphPinAcceptsLiteral / Arcane::GraphPinLiteralLanes, declared beside
        // GraphNodeInputPin and defined beside the emission switch they mirror
        // (MaterialGraph.hpp:385-403, MaterialGraph.cpp:1234-1285). They used
        // to be duplicated here, which made a future argOr-bypassing node type
        // a silent dead widget with nothing to fail; the engine copy has a
        // truth-table test over every node type instead.

        // What the widget shows on a pin that carries no literal yet: codegen's
        // NEUTRAL for that pin, so an untouched field never lies about the
        // value the shader is using and a first drag starts from it instead of
        // snapping the material to zero. The eight argOr call sites that pass
        // something other than "0.0" are enumerated at MaterialGraph.cpp:674-680
        // and each is cited below. Returns false when the neutral is not a
        // constant at all (Panner's v.uv), which the caller renders as a
        // non-numeric placeholder.
        bool PinNeutralDefault(const Arcane::GraphNode& n, std::uint32_t pin, float out[4])
        {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            switch (n.type)
            {
                case Arcane::GraphNodeType::Combine:        // alpha opaque (:855)
                    if (pin == 3)
                        out[0] = 1.0f;
                    return true;
                case Arcane::GraphNodeType::Clamp:          // max (:859)
                    if (pin == 2)
                        out[0] = 1.0f;
                    return true;
                case Arcane::GraphNodeType::Smoothstep:     // edge1 (:862)
                    if (pin == 1)
                        out[0] = 1.0f;
                    return true;
                case Arcane::GraphNodeType::Power:          // exponent (:869)
                    if (pin == 1)
                        out[0] = 1.0f;
                    return true;
                case Arcane::GraphNodeType::TilingOffset:   // tiling, splat (:892)
                    if (pin == 1)
                        out[0] = out[1] = 1.0f;
                    return true;
                case Arcane::GraphNodeType::SimpleNoise:    // scale (:956)
                    if (pin == 1)
                        out[0] = 10.0f;
                    return true;
                case Arcane::GraphNodeType::Panner:         // uv -> v.uv (:1058)
                    return pin != 0;
                case Arcane::GraphNodeType::ScaleOffset:    // scale = identity (:1074)
                    if (pin == 2)
                        out[0] = 1.0f;
                    return true;
                default:
                    return true;
            }
        }

        // Value equality for a pass's optional graph, for the gesture builders'
        // no-op guard ONLY. MaterialGraph is a plain aggregate with no
        // operator== (MaterialGraph.hpp:334-372; neither GraphNode nor
        // GraphLink has one either), so this rides the existing public
        // serialization: GraphToJson's text is byte-stable for equal graphs,
        // which MaterialGraphTest.cpp:755 and :1386 already assert. Doc-local
        // on purpose -- an engine-header operator== is a wider commitment than
        // this guard needs.
        //
        // Cost is one serialization per GESTURE CLOSE (not per frame, not per
        // drag tick), against graphs of tens of nodes -- the same order as the
        // whole-graph copy the step itself carries.
        //
        // Conservative in the one direction that matters: any float pair that
        // dumps differently (0.0 vs -0.0) reads as CHANGED and still pushes.
        bool GraphOptEqual(const std::optional<Arcane::MaterialGraph>& a,
                           const std::optional<Arcane::MaterialGraph>& b)
        {
            if (a.has_value() != b.has_value())
                return false;   // nullopt vs engaged: a real difference
            if (!a.has_value())
                return true;
            return Arcane::GraphToJson(*a).dump() == Arcane::GraphToJson(*b).dump();
        }

        // One graph gesture as an undo step (same doc-identity anchor pattern
        // as ParamEditCommand). Whole-graph before/after: our graphs are tens
        // of nodes -- the SG full-snapshot-undo pathology was per-edit JSON
        // reserialization + full preview regeneration, neither of which this
        // does (ApplyGraphState reuses the debounced compile loop).
        class GraphEditCommand final : public Arcane::ICommand
        {
        public:
            GraphEditCommand(std::weak_ptr<ShaderEditorDocument*> anchor, std::string label,
                             std::size_t pass,
                             std::optional<Arcane::MaterialGraph> before,
                             std::optional<Arcane::MaterialGraph> after)
                : m_anchor(std::move(anchor)), m_label(std::move(label)), m_pass(pass),
                  m_before(std::move(before)), m_after(std::move(after))
            {
            }

            void Undo() override { Apply(m_before); }
            void Redo() override { Apply(m_after); }
            const char* Label() const override { return m_label.c_str(); }

        private:
            void Apply(const std::optional<Arcane::MaterialGraph>& state)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplyGraphState(m_pass, state);
            }

            std::weak_ptr<ShaderEditorDocument*> m_anchor;
            std::string m_label;
            std::size_t m_pass;   // which pass's graph the step edits (0 = base)
            std::optional<Arcane::MaterialGraph> m_before, m_after;
        };

        // One pass-canvas STRUCTURAL gesture as an undo step (add/remove/
        // rewire/reorder/rename): whole pass-list before/after through the
        // same doc-identity anchor.
        class PassListCommand final : public Arcane::ICommand
        {
        public:
            PassListCommand(std::weak_ptr<ShaderEditorDocument*> anchor, std::string label,
                            ShaderEditorDocument::PassListState before,
                            ShaderEditorDocument::PassListState after)
                : m_anchor(std::move(anchor)), m_label(std::move(label)),
                  m_before(std::move(before)), m_after(std::move(after))
            {
            }

            void Undo() override { Apply(m_before); }
            void Redo() override { Apply(m_after); }
            const char* Label() const override { return m_label.c_str(); }

        private:
            void Apply(const ShaderEditorDocument::PassListState& state)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplyPassListState(state);
            }

            std::weak_ptr<ShaderEditorDocument*> m_anchor;
            std::string m_label;
            ShaderEditorDocument::PassListState m_before, m_after;
        };

        // ---- Pane splitters ------------------------------------------------
        // Divider geometry and limits, shared by both of Draw's splits.
        constexpr float kSplitBarPx  = 6.0f;     // the divider's hit width
        constexpr float kSplitLinePx = 1.0f;     // hairline drawn at rest
        constexpr float kSplitHotPx  = 2.0f;     // ... and while hovered/held
        constexpr float kPaneMinPx   = 120.0f;   // neither pane goes under this
        constexpr float kSplitMinF   = 0.15f;    // ... unless the span is too
        constexpr float kSplitMaxF   = 0.85f;    //     small for two floors

        // ---- Pane layout persistence (imgui.ini) ---------------------------
        // The ini section the ratio lives in: "[ArcaneEditorLayout]
        // [MaterialPanel]". TypeName may not contain '[' or ']'
        // (imgui_internal.h:2214); the entry name is what ReadOpen matches on,
        // and the pair is what lets a future panel add its own entry under the
        // same type without touching this handler.
        //
        // STALE ENTRIES from before the Material panel existed are inert, by
        // the two mechanisms already in place: the retired "[ArcaneEditorLayout]
        // [ShaderEditor]" section makes ReadOpen return null (which is how it
        // has always rejected an unknown name -- ImGui then skips that entry's
        // lines), and a retired "MainSplit=" line inside a section that IS
        // matched simply fails both sscanf branches in ReadLine and is dropped.
        // Neither path allocates or dereferences, so an old imgui.ini loads
        // clean; the next save rewrites the file without them.
        constexpr const char* kLayoutIniType = "ArcaneEditorLayout";
        constexpr const char* kLayoutIniName = "MaterialPanel";

        // A stored ratio arrives from a text file a human can edit, so it is
        // not trusted: anything non-finite or outside the working range is
        // pulled back to the fraction limits. The per-frame ClampSplit still
        // applies the pixel floors on top of this -- this only has to keep a
        // garbage line from parking a pane off-screen.
        float SanitizeSplit(float v)
        {
            if (!(v > 0.0f) || !(v < 1.0f))   // false for NaN, by construction
                return 0.5f;
            return (std::min)((std::max)(v, kSplitMinF), kSplitMaxF);
        }

        // ReadOpen returns the entry the following lines write into; returning
        // null makes ImGui skip the entry's lines, which is what an unknown
        // name should do (imgui.cpp:4498-4505 registers the stock "Window"
        // handler in this same shape).
        void* LayoutSettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
        {
            return std::strcmp(name, kLayoutIniName) == 0
                       ? static_cast<void*>(&ShaderEditorDocument::Layout())
                       : nullptr;
        }

        void LayoutSettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*,
                                    void* entry, const char* line)
        {
            auto* prefs = static_cast<ShaderEditorDocument::LayoutPrefs*>(entry);
            float v = 0.0f;
            if (std::sscanf(line, "PreviewSplit=%f", &v) == 1)
                prefs->previewSplit = SanitizeSplit(v);
        }

        void LayoutSettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler,
                                    ImGuiTextBuffer* buf)
        {
            const ShaderEditorDocument::LayoutPrefs& prefs = ShaderEditorDocument::Layout();
            buf->reserve(buf->size() + 64);
            buf->appendf("[%s][%s]\n", handler->TypeName, kLayoutIniName);
            buf->appendf("PreviewSplit=%.4f\n", prefs.previewSplit);
            buf->append("\n");
        }

        // The fraction a split may actually use, given `span` pixels of shared
        // extent. The PIXEL floor is what keeps a pane usable in a large
        // window; the FRACTION floor is what keeps both panes alive in a small
        // one -- under 2 * kPaneMinPx the two pixel floors would cross, so each
        // is folded against 0.5 first, which leaves lo <= 0.5 <= hi always (an
        // inverted range would make the clamp order-dependent).
        float ClampSplit(float ratio, float span)
        {
            float lo = kSplitMinF, hi = kSplitMaxF;
            if (span > 0.0f)
            {
                lo = (std::max)(lo, (std::min)(kPaneMinPx / span, 0.5f));
                hi = (std::min)(hi, (std::max)(1.0f - kPaneMinPx / span, 0.5f));
            }
            return (std::min)((std::max)(ratio, lo), hi);
        }

        // A draggable divider between two sibling panes -- ImGui's standard
        // splitter recipe: an InvisibleButton owns the gap, and because ImGui
        // holds ActiveId for as long as the button is held, MouseDelta keeps
        // arriving every frame even after the cursor leaves the rect. `span`
        // is the extent the two panes SHARE (their region minus this divider),
        // so pixels convert into the same fraction the caller laid out with.
        // Double-click restores `defaultRatio` -- the only way back to a round
        // split once dragged.
        //
        // Two axes, but only the vertical one (dragX=false) has a caller today
        // -- the Material panel's preview/params divider. The horizontal branch
        // is kept because the axis is the ONLY thing that differs between them
        // (four ternaries), so specialising it would not shrink this function,
        // and the node-properties section this panel is slated to grow is the
        // obvious next horizontal split.
        //
        // Submit it OUTSIDE ed::Begin/End (both callers do; the canvas's
        // Begin/End is down inside DrawGraphPanel): within the canvas the node editor
        // takes ImGui's input for itself and moves ImGui into canvas space
        // (imgui_canvas.cpp), so a divider there would both compete with the
        // pan/zoom gestures and drag at the zoom's rate rather than the
        // cursor's.
        void PaneSplitter(const char* id, bool dragX, float crossSize, float span,
                          float& ratio, float defaultRatio)
        {
            const ImVec2 size = dragX ? ImVec2(kSplitBarPx, crossSize)
                                      : ImVec2(crossSize, kSplitBarPx);
            if (size.x <= 0.0f || size.y <= 0.0f)
                return;   // degenerate region (InvisibleButton asserts on zero)

            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(id, size);
            const bool held    = ImGui::IsItemActive();
            const bool hovered = ImGui::IsItemHovered();
            if (held || hovered)
                ImGui::SetMouseCursor(dragX ? ImGuiMouseCursor_ResizeEW
                                            : ImGuiMouseCursor_ResizeNS);
            if (held && span > 0.0f)
            {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                ratio = ClampSplit(ratio + (dragX ? d.x : d.y) / span, span);
            }
            // After the drag, so the reset wins on the frame it fires (that
            // frame's own drag delta is ~0 anyway -- the click did not move).
            const bool reset = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            if (reset)
                ratio = defaultRatio;
            // Persist on RELEASE, not per frame: MarkIniSettingsDirty starts
            // ImGui's own save timer (IniSavingRate), so marking every frame of
            // a drag would keep re-arming a timer that writes the same file
            // anyway. IsItemDeactivated is true on the frame the drag ends
            // (imgui_internal.h's ActiveIdPreviousFrame bookkeeping), which is
            // exactly one mark per gesture.
            if (reset || ImGui::IsItemDeactivated())
                ImGui::MarkIniSettingsDirty();

            // Style-relative and three-tone, the same ramp ImGui's own docking
            // splitter uses: a hairline in Separator at rest, one step brighter
            // and one pixel wider on hover, brightest while held. The editor
            // theme fills all three entries (EditorTheme.hpp:175-177).
            const ImU32 col = ImGui::GetColorU32(held    ? ImGuiCol_SeparatorActive
                                               : hovered ? ImGuiCol_SeparatorHovered
                                                         : ImGuiCol_Separator);
            const float line = (held || hovered) ? kSplitHotPx : kSplitLinePx;
            const ImVec2 a = dragX ? ImVec2(p0.x + (size.x - line) * 0.5f, p0.y)
                                   : ImVec2(p0.x, p0.y + (size.y - line) * 0.5f);
            const ImVec2 b = dragX ? ImVec2(a.x + line, p0.y + size.y)
                                   : ImVec2(p0.x + size.x, a.y + line);
            ImGui::GetWindowDrawList()->AddRectFilled(a, b, col);
        }
    }

    // InputTextMultiline over std::string (the imgui_stdlib resize pattern) +
    // pending cursor jump: the CallbackAlways pass moves the cursor to the
    // requested line once the widget is active -- stb_textedit then scrolls the
    // cursor into view, which is exactly click-to-jump. UserData is the
    // DOCUMENT (per-doc state, review m2 -- a shared static could deliver one
    // document's jump to another on a same-frame focus race); this forwarder is
    // the declared friend, so it may touch the members directly.
    struct SnippetCallbackForwarder
    {
        static int Callback(ImGuiInputTextCallbackData* data)
        {
            auto* doc = static_cast<ShaderEditorDocument*>(data->UserData);
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                // ActiveSnippet: pass chains route the editor at the selected
                // pass's buffer (pass 0 = m_snippet, unchanged single-pass).
                std::string& buf = doc->ActiveSnippet();
                buf.resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = buf.data();
            }
            else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways &&
                     doc->m_callbackJumpLine > 0)
            {
                int line = 1;
                int pos = 0;
                while (line < doc->m_callbackJumpLine && pos < data->BufTextLen)
                {
                    if (data->Buf[pos] == '\n')
                        ++line;
                    ++pos;
                }
                data->CursorPos = pos;
                data->SelectionStart = data->SelectionEnd = pos;
                doc->m_callbackJumpLine = 0;
            }
            return 0;
        }
    };

    ShaderEditorDocument::ShaderEditorDocument(DocServices services,
                                               std::filesystem::path path,
                                               Arcane::MaterialAssetData data)
        : m_services(services), m_path(std::move(path)), m_data(std::move(data))
    {
        m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name;
        m_windowLabel = m_title + (m_data.IsInstance() ? " (Instance)###matdoc_"
                                                       : " (Material)###matdoc_") +
                        m_data.id.ToString();
        m_snippet = m_data.snippet;
        m_anchor = std::make_shared<ShaderEditorDocument*>(this);

        // Device-less services (headless tests) skip the preview cleanly; the
        // document still parses, compiles, and saves.
        if (m_services.device && m_services.shaders)
        {
            m_preview = Arcane::OffscreenCanvas::Create(m_services.device,
                                                        *m_services.shaders, 512, 512);
            m_pass = Arcane::FullscreenMaterialPass::Create(m_services.device);
            if (!m_preview || !m_pass)
                ARC_WARN("ShaderEditorDocument '{}': preview resources failed", m_title);
        }

        if (!IsInstance() || ResolveParentChain())
        {
            // Preview surface follows the asset's kind (instances inherit the
            // chain BASE's kind -- the base owns the snippet and the surface).
            const std::string& kind =
                IsInstance() && !m_parentChain.empty() ? m_parentChain.back().kind
                                                       : m_data.kind;
            m_surface = Arcane::MaterialSurfaceForKind(kind) ==
                                Arcane::MaterialSurface::Sprite ? 1 : 0;
            // Regenerate every graph-owned pass (deterministic codegen == the
            // saved snippets, so this leaves the doc clean) then compile; for
            // text-only docs the regen loop no-ops into a plain Rebuild.
            RegenerateFromGraph();
        }
    }

    ShaderEditorDocument::~ShaderEditorDocument()
    {
        // Documents are DESTROYED synchronously on close and there is no
        // on-close hook (DocumentHost::Close erases the unique_ptr; CloseAll
        // does it for every document on a project switch). The X-button path is
        // already safe -- requestClose is raised INSIDE Draw and acted on after
        // the draw loop, so the ScopeGuard above has run -- but any close that
        // destroys the document between a gesture parking and its next Draw (a
        // close hotkey, the project-switch CloseAll) would strand the
        // transaction open, and a stranded transaction leaves InTransaction()
        // true editor-wide: structural edits refused AND Ctrl+Z/Ctrl+Y dead.
        // Closing here is what makes that unreachable.
        //
        // It is not free: a close that LANDS a step also clears the REDO stack
        // (CommandStack.cpp:70 -- reached only for a non-empty transaction,
        // since :61-62 returns first when nothing changed). So closing a
        // document mid-gesture discards redo history. That is the accepted
        // cost of ClosePending's commit-not-cancel rule, which exists because
        // Cancel would discard the transaction WITHOUT reverting the edits the
        // user already watched happen.
        if (m_services.undo)
            EditGesture::ClosePending(*m_services.undo, m_gesture);

        if (m_graphCtx)
        {
            ed::DestroyEditor(m_graphCtx);
            m_graphCtx = nullptr;
        }
        if (m_passCanvasCtx)
        {
            ed::DestroyEditor(m_passCanvasCtx);
            m_passCanvasCtx = nullptr;
        }
    }

    bool ShaderEditorDocument::ResolveParentChain()
    {
        m_parentChain.clear();
        const Arcane::Project* project =
            m_services.runtime ? m_services.runtime->CurrentProject() : nullptr;
        if (!project)
        {
            m_parseErrors = { "instance materials need an open project (parent lookup)" };
            return false;
        }

        Arcane::Guid cursor = m_data.parent;
        std::vector<Arcane::Guid> visited{ m_data.id };
        while (cursor.IsValid())
        {
            for (const Arcane::Guid& seen : visited)
            {
                if (seen == cursor)
                {
                    m_parseErrors = { "parent chain contains a cycle" };
                    return false;
                }
            }
            visited.push_back(cursor);

            const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(cursor));
            if (!path)
            {
                m_parseErrors = { "parent material " + cursor.ToString() +
                                  " is not in the asset registry" };
                return false;
            }
            auto parent = Arcane::LoadMaterialAsset(*path);
            if (!parent)
            {
                m_parseErrors = { "parent material failed to load: " +
                                  path->generic_string() };
                return false;
            }
            cursor = parent->parent;
            m_parentChain.push_back(std::move(*parent));
        }

        if (m_parentChain.empty() || m_parentChain.back().IsInstance())
        {
            m_parseErrors = { "parent chain never reaches a base material" };
            return false;
        }
        return true;
    }

    const std::string& ShaderEditorDocument::SnippetSource() const
    {
        return IsInstance() && !m_parentChain.empty() ? m_parentChain.back().snippet
                                                      : m_snippet;
    }

    void ShaderEditorDocument::Rebuild()
    {
        if (!m_services.compiler || !m_services.sources)
            return;

        const char* templateFile = Arcane::MaterialTemplateFile(SurfaceOf(m_surface));
        const auto templateText = m_services.sources->Get(templateFile);
        if (!templateText)
        {
            m_parseErrors = { std::string("template not found: ") + templateFile };
            return;
        }

        // Invalidate every in-flight job (both paths): a result for a source
        // this Rebuild replaced must not bind.
        m_vsJob = m_psJob = 0;
        m_vsBytes.clear();
        m_psBytes.clear();
        m_passJobs.clear();

        if (ChainMode())
        {
            std::vector<Arcane::MaterialChainPassDesc> descs;
            descs.reserve(1 + m_data.passes.size());
            descs.push_back({ m_snippet, m_data.baseInputs });
            for (const Arcane::MaterialPass& p : m_data.passes)
                descs.push_back({ p.snippet, p.inputs });

            // The editor ALWAYS builds in post mode: scene reads must author
            // and preview here (the stand-in feeds them); only a non-post
            // RUNTIME consumer refuses them.
            Arcane::MaterialChainBuildResult build = Arcane::BuildMaterialChainSource(
                *templateText, descs, m_title, m_data.vertexSnippet,
                /*externalInput=*/true);
            m_passInputs = std::move(build.passInputs);
            m_chainInputSlots = build.chainInputSlots;
            m_vsLineOffset = 0;
            if (!m_data.vertexSnippet.empty() && !build.hlsl.empty())
                if (const std::size_t at = build.hlsl[0].find(m_data.vertexSnippet);
                    at != std::string::npos)
                    m_vsLineOffset = static_cast<int>(std::count(
                        build.hlsl[0].begin(),
                        build.hlsl[0].begin() + static_cast<std::ptrdiff_t>(at), '\n'));
            m_parseErrors = std::move(build.errors);
            for (std::size_t p = 0; p < build.passErrors.size(); ++p)
                for (const std::string& e : build.passErrors[p])
                    m_parseErrors.push_back(PassLabel(p) + ": " + e);

            m_passLineOffsets.assign(build.hlsl.size(), 0);
            for (std::size_t p = 0; p < build.hlsl.size(); ++p)
                if (const std::size_t at = build.hlsl[p].find(descs[p].snippet);
                    at != std::string::npos)
                    m_passLineOffsets[p] = static_cast<int>(std::count(
                        build.hlsl[p].begin(),
                        build.hlsl[p].begin() + static_cast<std::ptrdiff_t>(at), '\n'));
            m_snippetLineOffset = m_passLineOffsets.empty() ? 0 : m_passLineOffsets[0];
            m_pendingTemplate =
                std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
            m_metas = std::move(build.metas);

            m_passJobs.resize(build.hlsl.size());
            for (std::size_t p = 0; p < build.hlsl.size(); ++p)
            {
                Arcane::ShaderCompileRequest req;
                req.debugName = m_title + "_p" + std::to_string(p) + ".hlsl";
                req.sourceUtf8 = build.hlsl[p];
                req.entry = Arcane::kPsEntry;
                req.profile = Arcane::kPsProfile;
                req.coalesceKey = StageKey(m_data.id, /*vertex=*/false, p);
                m_passJobs[p].psJob = m_services.compiler->Submit(req, Now());
                req.entry = Arcane::kVsEntry;
                req.profile = Arcane::kVsProfile;
                req.coalesceKey = StageKey(m_data.id, /*vertex=*/true, p);
                m_passJobs[p].vsJob = m_services.compiler->Submit(std::move(req), Now());
            }
            return;
        }

        // Instances inherit the base's vertex stage (they carry no snippets).
        const std::string& vertexSnippet =
            IsInstance() && !m_parentChain.empty() ? m_parentChain.back().vertexSnippet
                                                   : m_data.vertexSnippet;
        Arcane::MaterialBuildResult build =
            Arcane::BuildMaterialShaderSource(*templateText, SnippetSource(), m_title,
                                              SurfaceOf(m_surface), vertexSnippet);
        m_parseErrors = std::move(build.errors);
        m_vsLineOffset = 0;
        if (!vertexSnippet.empty())
            if (const std::size_t at = build.hlsl.find(vertexSnippet);
                at != std::string::npos)
                m_vsLineOffset = static_cast<int>(std::count(
                    build.hlsl.begin(),
                    build.hlsl.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
        // The snippet rides verbatim into the stitched source -- its line
        // offset maps compiler diag lines back into snippet space (error-row
        // jump; graph node badges via the codegen line map).
        m_snippetLineOffset = 0;
        if (const std::size_t at = build.hlsl.find(SnippetSource());
            at != std::string::npos)
            m_snippetLineOffset = static_cast<int>(
                std::count(build.hlsl.begin(), build.hlsl.begin() + at, '\n'));
        m_pendingTemplate = std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
        m_metas = std::move(build.metas);

        Arcane::ShaderCompileRequest req;
        req.debugName = m_title + ".hlsl";
        req.sourceUtf8 = build.hlsl;
        req.entry = Arcane::kPsEntry;
        req.profile = Arcane::kPsProfile;
        req.coalesceKey = StageKey(m_data.id, /*vertex=*/false);
        m_psJob = m_services.compiler->Submit(req, Now());
        req.entry = Arcane::kVsEntry;
        req.profile = Arcane::kVsProfile;
        req.coalesceKey = StageKey(m_data.id, /*vertex=*/true);
        m_vsJob = m_services.compiler->Submit(std::move(req), Now());
        m_vsBytes.clear();
        m_psBytes.clear();
    }

    bool ShaderEditorDocument::ConsumeResult(const Arcane::ShaderCompileResult& result)
    {
        // Node-preview jobs: diag-less (the MAIN compile owns every badge --
        // a preview clone can only fail where the real snippet also fails);
        // a failure simply leaves that node's last-good thumbnail showing.
        if (result.jobId == m_nodePreviewVsJob && m_nodePreviewVsJob != 0)
        {
            m_nodePreviewVsJob = 0;
            const auto& target = m_services.backend == Arcane::GraphicsBackend::Vulkan
                                     ? result.spirv : result.dxil;
            if (target.succeeded && m_services.device)
            {
                m_nodePreviewVs = m_services.device->createShader(
                    nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex)
                        .setEntryName(Arcane::kVsEntry)
                        .setDebugName((m_title + "_thumbvs").c_str()),
                    target.bytecode.data(), target.bytecode.size());
                // Bind every thumbnail that landed before the shared VS did.
                if (m_nodePreviewVs)
                    for (auto& [id, np] : m_nodePreviews)
                        if (!np.psBytes.empty())
                        {
                            nvrhi::ShaderHandle ps = m_services.device->createShader(
                                nvrhi::ShaderDesc()
                                    .setShaderType(nvrhi::ShaderType::Pixel)
                                    .setEntryName(Arcane::kPsEntry)
                                    .setDebugName("NodePreview_ps"),
                                np.psBytes.data(), np.psBytes.size());
                            np.psBytes.clear();
                            BindNodePreview(np, ps);
                        }
            }
            return true;
        }
        for (auto& [id, np] : m_nodePreviews)
        {
            if (result.jobId != np.psJob || np.psJob == 0)
                continue;
            np.psJob = 0;
            const auto& target = m_services.backend == Arcane::GraphicsBackend::Vulkan
                                     ? result.spirv : result.dxil;
            if (target.succeeded && m_services.device)
            {
                if (!m_nodePreviewVs)
                    np.psBytes = target.bytecode;   // bind when the VS lands
                else
                {
                    nvrhi::ShaderHandle ps = m_services.device->createShader(
                        nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel)
                            .setEntryName(Arcane::kPsEntry)
                            .setDebugName("NodePreview_ps"),
                        target.bytecode.data(), target.bytecode.size());
                    BindNodePreview(np, ps);
                }
            }
            return true;
        }

        // Chain-mode jobs first (mutually exclusive with the single-path ids --
        // Rebuild clears whichever set is not in play).
        for (std::size_t p = 0; p < m_passJobs.size(); ++p)
        {
            PassJobs& pj = m_passJobs[p];
            const bool chainVs = result.jobId == pj.vsJob && pj.vsJob != 0;
            const bool chainPs = result.jobId == pj.psJob && pj.psJob != 0;
            if (!chainVs && !chainPs)
                continue;
            const auto& target = m_services.backend == Arcane::GraphicsBackend::Vulkan
                                     ? result.spirv : result.dxil;
            if (chainPs)
            {
                pj.diags = target.diags;
                if (p == 0)
                    m_diags = target.diags;   // single-path mirror
                // Badges belong to the ACTIVE pass's canvas (any pass may be
                // graph-owned now).
                RebuildDiagBadges();
            }
            if (chainVs && p == 0)
                m_vsDiags = target.diags;   // one vertex stage, every pass alike
            if (target.succeeded)
            {
                (chainVs ? pj.vsBytes : pj.psBytes) = target.bytecode;
                BindIfComplete();
            }
            return true;
        }

        const bool isVs = result.jobId == m_vsJob && m_vsJob != 0;
        const bool isPs = result.jobId == m_psJob && m_psJob != 0;
        if (!isVs && !isPs)
            return false;

        const auto& target = m_services.backend == Arcane::GraphicsBackend::Vulkan
                                 ? result.spirv : result.dxil;
        if (isPs)
        {
            m_diags = target.diags;   // the ps stage carries the designer-relevant diags
            RebuildDiagBadges();
        }
        if (isVs)
            m_vsDiags = target.diags;   // the vertex stage's own errors (displace)
        if (target.succeeded)
        {
            (isVs ? m_vsBytes : m_psBytes) = target.bytecode;
            BindIfComplete();
        }
        return true;
    }

    void ShaderEditorDocument::BindIfComplete()
    {
        if (ChainMode())
        {
            BindChainIfComplete();
            return;
        }
        const bool sprite = m_surface == 1;
        if (m_vsBytes.empty() || m_psBytes.empty() || !m_pendingTemplate ||
            (sprite ? !m_preview : !m_pass))
            return;

        nvrhi::ShaderHandle vs = m_services.device->createShader(
            nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex)
                .setEntryName(Arcane::kVsEntry).setDebugName((m_title + "_vs").c_str()),
            m_vsBytes.data(), m_vsBytes.size());
        nvrhi::ShaderHandle ps = m_services.device->createShader(
            nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel)
                .setEntryName(Arcane::kPsEntry).setDebugName((m_title + "_ps").c_str()),
            m_psBytes.data(), m_psBytes.size());
        m_vsBytes.clear();
        m_psBytes.clear();
        if (!vs || !ps)
            return;
        // Fullscreen surface binds the pass here (a failure keeps last-good
        // AND the previous instance). Sprite surface registers with the
        // preview canvas's batcher AFTER the fresh instance exists below.
        if (!sprite && !m_pass->SetMaterial(m_pendingTemplate, vs, ps))
            return;

        PromotePendingInstance();

        if (sprite)
        {
            m_previewVs = vs;
            m_previewPs = ps;
            RefreshSpritePreviewBinding();
        }
    }

    void ShaderEditorDocument::PromotePendingInstance()
    {
        // Promote pending -> bound. Instance mode first layers the parent chain
        // (base's saved values innermost) so resolution walks child override ->
        // parents -> //@param default. Then migrate this document's own
        // overrides by name hash (first bind applies the asset's saved values;
        // params a snippet edit dropped are rejected by Set and retired).
        auto fresh = std::make_shared<Arcane::MaterialInstance>(m_pendingTemplate);
        for (auto it = m_parentChain.rbegin(); it != m_parentChain.rend(); ++it)
        {
            Arcane::ApplyMaterialParams(*it, *fresh);
            fresh = std::make_shared<Arcane::MaterialInstance>(
                std::shared_ptr<const Arcane::MaterialInstance>(fresh));
        }
        if (m_instance)
        {
            // Overrides forward by hash -- through the pending-rename
            // translation, so an assisted rename carries the value onto the
            // renamed decl instead of retiring it.
            for (const auto& [hash, value] : m_instance->Overrides())
                fresh->Set(TranslateOverrideHash(hash, *m_pendingTemplate), value);
        }
        else
        {
            Arcane::ApplyMaterialParams(m_data, *fresh);
        }
        // Re-baseline the dirty verdict against the FRESH instance's serial
        // space; unsaved param edits ride across the swap as the base flag.
        const bool paramsWereDirty = ParamsDirty();
        m_instance = std::move(fresh);
        m_paramsBaseDirty = paramsWereDirty;
        m_savedParamSerial = m_instance->EffectiveSerial();
        m_boundTemplate = m_pendingTemplate;
        m_boundMetas = m_metas;
    }

    void ShaderEditorDocument::BindChainIfComplete()
    {
        if (!m_pendingTemplate || m_passJobs.empty() || !m_services.device)
            return;
        for (const PassJobs& pj : m_passJobs)
            if (pj.vsBytes.empty() || pj.psBytes.empty())
                return;   // a stage is still in flight (or failed -- last-good stays)

        if (!m_chain)
            m_chain = Arcane::FullscreenMaterialChain::Create(m_services.device);
        if (!m_chain)
            return;

        std::vector<Arcane::FullscreenMaterialChain::PassShaders> shaders;
        shaders.reserve(m_passJobs.size());
        for (std::size_t p = 0; p < m_passJobs.size(); ++p)
        {
            PassJobs& pj = m_passJobs[p];
            const std::string stem = m_title + "_p" + std::to_string(p);
            Arcane::FullscreenMaterialChain::PassShaders ps;
            ps.vs = m_services.device->createShader(
                nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex)
                    .setEntryName(Arcane::kVsEntry).setDebugName((stem + "_vs").c_str()),
                pj.vsBytes.data(), pj.vsBytes.size());
            ps.ps = m_services.device->createShader(
                nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel)
                    .setEntryName(Arcane::kPsEntry).setDebugName((stem + "_ps").c_str()),
                pj.psBytes.data(), pj.psBytes.size());
            if (p < m_passInputs.size())
                ps.inputs = m_passInputs[p];
            pj.vsBytes.clear();
            pj.psBytes.clear();
            if (!ps.vs || !ps.ps)
                return;
            shaders.push_back(std::move(ps));
        }
        // Atomic: a rejected chain keeps the previous one bound AND the
        // previous instance (same last-good shape as SetMaterial).
        if (!m_chain->SetChain(m_pendingTemplate, shaders, m_chainInputSlots))
            return;

        PromotePendingInstance();
    }

    void ShaderEditorDocument::RefreshSpritePreviewBinding()
    {
        // (Re)register the preview material on the canvas's OWN batcher: the
        // shared instance pointer keeps live param edits flowing (PackCB reads
        // it at End()); texture params resolve fresh HERE, so a pick lands by
        // calling this again -- no recompile.
        if (!m_preview || !m_previewVs || !m_previewPs || !m_instance || !m_boundTemplate)
            return;
        Arcane::Material2DDesc desc;
        desc.vs = m_previewVs;
        desc.ps = m_previewPs;
        desc.templ = m_boundTemplate;
        desc.instance = m_instance;
        const std::vector<Arcane::Guid> texGuids = m_instance->ResolveTextures();
        desc.paramTextures.resize(texGuids.size());
        if (m_services.runtime)
            for (std::size_t i = 0; i < texGuids.size(); ++i)
                if (texGuids[i].IsValid())
                    desc.paramTextures[i] = m_services.runtime->AssetsFacade().GetTexture(
                        Arcane::AssetId::FromGuid(texGuids[i]));

        Arcane::Batcher2D& batcher = m_preview->Batch();
        if (m_previewSpriteMaterial != Arcane::Batcher2D::kInvalidMaterialId)
        {
            if (!batcher.UpdateMaterial(m_previewSpriteMaterial, std::move(desc)))
                ARC_WARN("ShaderEditorDocument '{}': sprite preview update failed", m_title);
            return;
        }
        m_previewSpriteMaterial = batcher.RegisterMaterial(std::move(desc));
    }

    bool ShaderEditorDocument::HasErrors() const
    {
        if (!m_parseErrors.empty())
            return true;
        for (const Arcane::ShaderDiag& d : m_diags)
            if (d.severity == Arcane::ShaderDiagSeverity::Error)
                return true;
        for (const PassJobs& pj : m_passJobs)
            for (const Arcane::ShaderDiag& d : pj.diags)
                if (d.severity == Arcane::ShaderDiagSeverity::Error)
                    return true;
        // Vertex-stage errors, filtered to the vertex body (same filter as
        // ForEachDiagnosticRow's vertex block).
        if (!m_data.vertexSnippet.empty())
        {
            const int vsLines = 1 + static_cast<int>(std::count(
                m_data.vertexSnippet.begin(), m_data.vertexSnippet.end(), '\n'));
            for (const Arcane::ShaderDiag& d : m_vsDiags)
            {
                const int rel = d.line - m_vsLineOffset;
                if (rel >= 1 && rel <= vsLines &&
                    d.severity == Arcane::ShaderDiagSeverity::Error)
                    return true;
            }
        }
        return false;
    }

    void ShaderEditorDocument::RequestSave()
    {
        // Error-guarded save (UE's pre-apply guard shape): saving broken WIP is
        // allowed, but only through an explicit confirm. This guard used to sit
        // inside the toolbar's Save button; it lives here now so the Ctrl+S
        // route cannot walk past it. The modal itself is still drawn by Draw
        // and still calls the unguarded Save on "Save Anyway".
        if (HasErrors())
            m_confirmSaveWithErrors = true;
        else
            Save();
    }

    bool ShaderEditorDocument::Save()
    {
        m_data.snippet = m_snippet;
        // Review M1: before the first successful bind there is no instance to
        // harvest values from -- keep the loaded params instead of wiping them.
        if (m_instance && m_boundTemplate)
        {
            m_data.params.clear();
            for (const auto& [hash, value] : m_instance->Overrides())
                if (const Arcane::ParamDecl* d = m_boundTemplate->Find(hash))
                {
                    // An INSTANCE still bound to the pre-rename base harvests
                    // the OLD decl name -- translate so Save never writes an
                    // orphan back over the propagated file. (A base's own
                    // template already carries the new name.)
                    std::string pname = d->name;
                    if (IsInstance())
                        for (const auto& [oldName, newName] : m_paramRenames)
                            if (pname == oldName)
                                pname = newName;
                    m_data.params.emplace_back(std::move(pname), value);
                }
        }
        if (!Arcane::SaveMaterialAsset(m_path, m_data))
            return false;
        // Idempotent: heals assets opened from paths the registry has never seen
        // (created outside the editor flows, or before this session).
        if (m_services.runtime)
            m_services.runtime->RegisterCreatedAsset(m_path);
        m_dirty = false;
        m_paramsBaseDirty = false;
        m_savedParamSerial = m_instance ? m_instance->EffectiveSerial() : 0;
        if (m_services.onAssetSaved)
            m_services.onAssetSaved(m_data.id);
        return true;
    }

    bool ShaderEditorDocument::ParamsDirty() const
    {
        return m_paramsBaseDirty ||
               (m_instance && m_instance->EffectiveSerial() != m_savedParamSerial);
    }

    void ShaderEditorDocument::ApplyParamEdit(std::uint32_t nameHash, bool hasValue,
                                              const Arcane::MatParamValue& value)
    {
        if (!m_instance)
            return;
        // A param the current snippet dropped is rejected by Set -- the step
        // no-ops rather than corrupting an unrelated instance.
        if (hasValue)
            m_instance->Set(nameHash, value);
        else
            m_instance->ClearOverride(nameHash);
        // Undo/redo of a TEXTURE param must re-bind the sprite preview too.
        if (m_surface == 1 && m_boundTemplate)
            if (const Arcane::ParamDecl* d = m_boundTemplate->Find(nameHash);
                d && d->type == Arcane::MatParamType::Texture)
                RefreshSpritePreviewBinding();
    }

    bool ShaderEditorDocument::PreviewReady() const
    {
        if (m_surface == 1)
            return m_previewSpriteMaterial != Arcane::Batcher2D::kInvalidMaterialId;
        if (ChainMode())
            return m_chain && m_chain->Ready();
        return m_pass && m_pass->Ready();
    }

    std::string& ShaderEditorDocument::ActiveSnippet()
    {
        if (m_editVertex)
            return m_data.vertexSnippet;   // the ONE vertex stage (doc-level)
        if (m_activePass > 0 &&
            m_activePass <= static_cast<int>(m_data.passes.size()))
            return m_data.passes[static_cast<std::size_t>(m_activePass) - 1].snippet;
        return m_snippet;
    }

    std::string ShaderEditorDocument::PassLabel(std::size_t pass) const
    {
        if (pass == 0)
            return "base";
        if (pass <= m_data.passes.size())
        {
            const std::string& n = m_data.passes[pass - 1].name;
            if (!n.empty())
                return n;
        }
        return "pass " + std::to_string(pass);
    }

    void ShaderEditorDocument::Tick(double dt)
    {
        // Before any early return below: diagnostics are published even for a
        // document whose preview is not ready (a material that fails to compile
        // is exactly the case that has something to say).
        PublishDiagnostics();
        m_animTime += dt;
        // Thumbnails displaced LAST frame are safe to release now (their
        // ImGui draws have been recorded); this frame's displacements queue up.
        m_nodePreviewRetired.clear();
        RenderNodePreviews(dt);
        if (!PreviewReady() || !m_instance || !m_preview)
            return;

        // Texture params must be GPU-resident BEFORE the canvas list records:
        // the Assets facade uploads through its own transient command list, and
        // an executeCommandList issued while ANOTHER list is open loses the
        // upload -- the texture stays empty (the [gpu] texparam test pins this
        // contract). Memoized, so steady-state cost is a hash lookup per param.
        if (m_services.runtime)
            for (const Arcane::Guid& g : m_instance->ResolveTextures())
                if (g.IsValid())
                    (void)m_services.runtime->AssetsFacade().GetTexture(
                        Arcane::AssetId::FromGuid(g));

        Arcane::GlobalParams globals;
        globals.time = static_cast<float>(m_animTime);
        globals.deltaTime = static_cast<float>(dt);
        globals.viewportWidth = static_cast<float>(m_preview->Width());
        globals.viewportHeight = static_cast<float>(m_preview->Height());

        if (m_surface == 1)
        {
            // Sprite surface: the material on a centered quad over a
            // checkerboard, through the canvas's own Batcher2D -- the SAME
            // pipeline family scene sprites use.
            m_preview->Draw(
                [&](Arcane::Batcher2D& b)
                {
                    b.SetGlobals(globals);
                    const float w = (float)m_preview->Width();
                    const float h = (float)m_preview->Height();
                    const float cell = 32.0f;
                    const glm::vec4 light(0.16f, 0.16f, 0.19f, 1.0f);
                    for (int y = 0; y * cell < h; ++y)
                        for (int x = 0; x * cell < w; ++x)
                            if ((x + y) & 1)
                                b.Rect(glm::vec2(x * cell, y * cell),
                                       glm::vec2(cell, cell), light);
                    const float s = 0.8f * (std::min)(w, h);
                    b.QuadMaterial(m_previewSpriteMaterial,
                                   glm::vec2((w - s) * 0.5f, (h - s) * 0.5f),
                                   glm::vec2(s, s), nullptr,
                                   glm::vec2(0.0f), glm::vec2(1.0f),
                                   glm::vec4(1.0f));
                },
                glm::vec4(0.09f, 0.09f, 0.11f, 1.0f));
            return;
        }

        m_preview->DrawPass(
            [&](nvrhi::ICommandList* cl, nvrhi::IFramebuffer* fb)
            {
                Arcane::Assets* assets =
                    m_services.runtime ? &m_services.runtime->AssetsFacade() : nullptr;
                if (ChainMode())
                {
                    // View-any-intermediate: the chain truncates at the viewed
                    // pass, which renders into the preview canvas itself.
                    const std::size_t view =
                        m_viewPass < 0 ? static_cast<std::size_t>(-1)
                                       : static_cast<std::size_t>(m_viewPass);
                    m_chain->Render(cl, fb, *m_instance, globals, assets, view,
                                    SceneStandIn());
                }
                else
                    m_pass->Render(cl, fb, *m_instance, globals, assets);
            },
            glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    }

    ShaderEditorDocument::LayoutPrefs& ShaderEditorDocument::Layout()
    {
        // One per process, defaulted from the class constants. Function-local so
        // there is no static-init order question with the ini handler, which is
        // registered from EditorApp::Init and may read this on its first line.
        static LayoutPrefs prefs;
        return prefs;
    }

    void ShaderEditorDocument::RegisterLayoutSettings()
    {
        // No context (headless) or already registered: nothing to do. ImGui
        // COPIES the handler into the context (imgui.cpp's AddSettingsHandler
        // does a push_back by value), so the local below may die here -- the
        // stock handlers are registered from a local exactly the same way.
        if (ImGui::GetCurrentContext() == nullptr ||
            ImGui::FindSettingsHandler(kLayoutIniType) != nullptr)
            return;

        ImGuiSettingsHandler handler;
        handler.TypeName   = kLayoutIniType;
        handler.TypeHash   = ImHashStr(kLayoutIniType);
        handler.ReadOpenFn = LayoutSettingsReadOpen;
        handler.ReadLineFn = LayoutSettingsReadLine;
        handler.WriteAllFn = LayoutSettingsWriteAll;
        ImGui::AddSettingsHandler(&handler);
    }

    void ShaderEditorDocument::Draw(bool& requestClose)
    {
        // FIRST local, so it destructs LAST -- see EditGesture::ScopeGuard. It
        // covers the early return below (Begin refused: collapsed window or a
        // background tab, where no widget inside can report its deactivation).
        const EditGesture::ScopeGuard gestureGuard{ m_services.undo, m_gesture };

        // Cleared FIRST, every frame: it is a one-frame edge, and the host
        // polls it unconditionally. Leaving a stale `true` on the early return
        // below would re-fire the focus follow every frame this document spent
        // as a background tab.
        m_tabBecameVisible = false;

        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
        ImGuiWindowFlags flags = Dirty() ? ImGuiWindowFlags_UnsavedDocument : 0;
        if (!ImGui::Begin(m_windowLabel.c_str(), &open, flags))
        {
            // Collapsed or a background tab: not focused, and it must be said
            // out loud -- a stale true here would hand Ctrl+S to a document the
            // user cannot even see.
            m_windowFocused = false;
            ImGui::End();
            requestClose = !open;
            return;
        }
        // What Ctrl+S resolves against (DocumentHost::FocusedDoc).
        // RootAndChildWindows so the canvas, the text editor and every child
        // region inside the document still count as "in this document".
        m_windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        // Ctrl+S saves -- the toolbar button this replaced is gone. Shortcut()
        // (not IsKeyChordPressed) so it ROUTES to whichever document owns focus
        // (imgui.h:1106-1114, default ImGuiInputFlags_RouteFocused): with
        // several material/sprite documents open, each one's Ctrl+S only fires
        // for the one on top. Same binding SpriteDocument uses
        // (SpriteDocument.cpp:181-187), deliberately -- one shape for "the
        // focused document saves itself".
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))
            RequestSave();

        // Latch "this tab just became visible" for the host's focus follow.
        // Read AFTER Begin returned true: a background tab is skipped by the
        // early return above, and imgui.cpp:8778-8779 asserts that a skipped
        // window is never Appearing, so the flag is only ever raised here.
        m_tabBecameVisible = ImGui::IsWindowAppearing();

        DrawToolbar();

        // ONE column. The preview and the params editor moved OUT to the
        // dockable Material panel (DrawMaterialWindow, docked beside the
        // Inspector), which is what retired this window's right column and the
        // horizontal "##splitmain" divider that used to size it: with the right
        // column gone there was nothing left for a horizontal split to divide.
        // The surviving split -- preview against params -- went with them.
        if (!IsInstance())
        {
            // Pass canvas: fullscreen base materials only (sprite chains are
            // refused; instances re-value the base's chain). Even a single-pass
            // material shows base -> Output -- the pipeline affordance and the
            // right-click Add Pass entry point.
            if (m_surface == 0)
                DrawPassCanvas(170.0f);
            if (m_activePass > static_cast<int>(m_data.passes.size()))
                m_activePass = 0;   // stale selection after an outside reload
            // The canvas serves whichever pass is active and graph-owned;
            // text-owned passes -- and the vertex stage -- get the text editor.
            // Both fill the rest of the tab themselves (each measures the
            // region at the point it opens): the space the errors panel used to
            // reserve just falls to the canvas / text.
            if (ActiveGraphOwned() && !m_showGeneratedText && !m_editVertex)
                DrawGraphPanel();
            else
                DrawSnippetEditor();
        }
        else
        {
            // INSTANCE mode: an instance authors no source -- that belongs to
            // its base -- and its params now live in the Material panel, which
            // would leave this tab empty. So the preview takes the whole tab:
            // an instance IS its values, and the large preview is the one thing
            // this window can still say about them that the side panel cannot
            // (the panel's preview is Inspector-column narrow). The toolbar
            // above keeps the parent-chain affordances reachable; saving is
            // Ctrl+S, which needs no toolbar room at all.
            DrawPreviewPanel(ImGui::GetContentRegionAvail().y);
        }

        ImGui::End();
        requestClose = !open;
    }

    void ShaderEditorDocument::DrawMaterialWindow()
    {
        // FIRST local, so it destructs LAST -- see EditGesture::ScopeGuard.
        // The param rows below open gestures against m_gesture, and this scope
        // has the same early-return/collapsed-window hazards Draw's guard
        // covers (Begin refusing on a background tab, with no widget inside
        // able to report its own deactivation).
        const EditGesture::ScopeGuard gestureGuard{ m_services.undo, m_gesture };

        if (ImGui::Begin("Material"))
        {
            // Which material this is: the panel is a shared surface, so it has
            // to name its subject the way the Inspector names the entity.
            // m_title, NOT m_windowLabel -- the latter carries the "###matdoc_"
            // id suffix that only ImGui::Begin strips, so a Text* call would
            // print it verbatim.
            ImGui::TextUnformatted(m_title.c_str());
            if (IsInstance())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(Instance)");
            }
            ImGui::Separator();

            // The one surviving draggable split (PaneSplitter) over the SHARED
            // layout preference -- every open shader document reads the same
            // ratio, and a drag is the layout all of them use (Layout(), which
            // the ini handler persists). Preview on top, params below; the
            // divider sits BETWEEN them, so the height it occupies comes off
            // the span the fraction divides.
            LayoutPrefs& layout = Layout();
            const ImVec2 avail  = ImGui::GetContentRegionAvail();
            const float span    = (std::max)(avail.y - kSplitBarPx, 1.0f);
            DrawPreviewPanel(span * ClampSplit(layout.previewSplit, span));
            PaneSplitter("##splitpreview", /*dragX=*/false, avail.x, span,
                         layout.previewSplit, kPreviewSplitDefault);
            DrawParamsPanel();   // fills whatever the preview left
        }
        ImGui::End();
    }

    void DrawMaterialPanel(ShaderEditorDocument* active)
    {
        // No material document open: submit NOTHING. The panel is absent, not
        // empty -- a tab that only ever says "nothing here" is a permanent
        // reminder of a feature you are not using.
        //
        // Hiding a docked window this way does NOT lose its slot, which is the
        // whole reason it is safe. When a docked window stops being submitted,
        // ImGui removes it from the node with the node's OWN id as the
        // save-dock-id (imgui.cpp:18936-18949, on `window->WasActive == false`),
        // and DockNodeRemoveWindow writes that back as `window->DockId`
        // (imgui.cpp:18730). Begin then re-binds on the next submission
        // (imgui.cpp:7888 tests `window->DockId != 0 || window->DockNode`), so
        // the window returns to the same node -- including a node the user
        // re-docked it into. Across restarts the same id rides imgui.ini and is
        // restored at imgui.cpp:6938 (`window->DockId = settings->DockId`).
        if (!active)
            return;
        active->DrawMaterialWindow();
    }

    void ShaderEditorDocument::DrawToolbar()
    {
        // NO Save button: saving is Ctrl+S (routed to the focused document by
        // EditorAppFrame). The error guard that button carried did not go with
        // it -- it moved to RequestSave, which is what the shortcut runs.
        //
        // Losing the button cost the toolbar its guaranteed first item, which
        // the SameLine chain below was leaning on: an INSTANCE whose parent
        // chain has not resolved now draws nothing before the surface selector,
        // and a SameLine as a window's first call pulls the cursor up onto the
        // line above. Hence the explicit flag rather than an unconditional
        // SameLine.
        bool anyBefore = false;
        if (!IsInstance())
        {
            // Structural (snippet) controls are base-material-only; an instance
            // recompiles nothing -- it only re-values the parent's shader.
            ImGui::Checkbox("Live", &m_live);
            ImGui::SameLine();
            if (ImGui::Button("Compile"))
                RegenerateFromGraph();   // regenerates graph passes, then Rebuild
            if (ActiveGraphOwned())
            {
                // Read-only generated-code view (UE's HLSL window / SG's View
                // Generated Shader). No convert-out: graphs are THE authoring
                // tier; freeform HLSL lives in Custom nodes.
                ImGui::SameLine();
                ImGui::Checkbox("HLSL", &m_showGeneratedText);
                ImGui::SameLine();
                if (ImGui::Checkbox("Thumbs", &m_showNodePreviews))
                    RefreshNodePreviews();   // off clears; on resubmits
            }
            // The vertex stage (%{VERTEX_BODY}): graph-owned materials author
            // it with the Vertex Output NODE and view it inside the HLSL
            // toggle (UE's shape: one graph, one read-only code viewer). Only
            // repair mode -- a graphless base -- hand-edits the text.
            if (!IsGraphOwned())
            {
                ImGui::SameLine();
                ImGui::Checkbox("Vertex", &m_editVertex);
            }
            else
                m_editVertex = false;
            anyBefore = true;   // the Live checkbox always draws
        }
        else if (!m_parentChain.empty())
        {
            ImGui::TextDisabled("instance of '%s'", m_parentChain.back().name.c_str());
            anyBefore = true;
        }
        if (anyBefore)
            ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        // Preview-surface selector (Slice 8). On a base material this is a
        // STRUCTURAL edit: it re-kinds the asset (the surface is what the
        // material is FOR) and recompiles under the other template. Instances
        // preview under the switched surface without touching the base.
        // Pass chains are fullscreen-only: the selector LOCKS while extra
        // passes exist (no refusal path can lose data).
        const bool surfaceLocked =
            !IsInstance() &&
            (!m_data.passes.empty() || !m_data.baseInputs.empty());
        if (surfaceLocked)
            ImGui::BeginDisabled();
        int surface = m_surface;
        const bool surfacePicked =
            ImGui::Combo("##surface", &surface, "Fullscreen\0Sprite\0");
        if (surfaceLocked)
        {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("pass chains are fullscreen-only -- remove the "
                                  "extra passes to change the surface");
        }
        if (surfacePicked && surface != m_surface)
        {
            m_surface = surface;
            m_previewSpriteMaterial = Arcane::Batcher2D::kInvalidMaterialId;
            m_previewVs = nullptr;
            m_previewPs = nullptr;
            if (!IsInstance())
            {
                m_data.kind = m_surface == 1 ? "sprite" : "fullscreen";
                m_dirty = true;
            }
            // Graph docs must re-CODEGEN, not just restitch -- the surface
            // gates node validity (VertexColor/SpriteTexture are sprite-only).
            RegenerateFromGraph();
        }
        ImGui::SameLine();
        if (HasErrors())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "errors");
        else if (PreviewReady())
            ImGui::TextDisabled("ok");
        else
            ImGui::TextDisabled("compiling...");
        ImGui::Separator();

        if (m_confirmSaveWithErrors)
        {
            ImGui::OpenPopup("Save With Errors?##matdoc");
            m_confirmSaveWithErrors = false;
        }
        if (ImGui::BeginPopupModal("Save With Errors?##matdoc", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            // "see the Console" because the rows no longer live in this window
            // (PublishDiagnostics), matching the shell's other deferrals.
            ImGui::TextUnformatted("This material has compile errors (see the "
                                   "Console). Save anyway?");
            ImGui::Separator();
            if (ImGui::Button("Save Anyway"))
            {
                Save();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

    }

    void ShaderEditorDocument::DrawSnippetEditor()
    {
        // Graph-owned + HLSL toggle = UE's code viewer: ONE read-only window
        // with everything this pass generates -- the pixel body, plus the
        // material's vertex body under the base.
        const bool generatedView =
            ActiveGraphOwned() && m_showGeneratedText && !m_editVertex;

        // Say which buffer this is (the pass canvas / repair toggles select it).
        if (m_editVertex)
            ImGui::TextDisabled("repair mode: vertex stage (hand-edit; graph-owned "
                                "materials author this with a Vertex Output node)");
        else if (!ActiveGraphOwned())
            ImGui::TextDisabled("repair mode: no graph on this %s -- hand-edit "
                                "the snippet, or revert the file",
                                m_activePass == 0 ? "material" : "pass");
        else if (ChainMode())
            ImGui::TextDisabled("%s (generated)",
                                PassLabel(static_cast<std::size_t>(m_activePass)).c_str());
        if (m_jumpToLine > 0)
        {
            m_callbackJumpLine = m_jumpToLine;
            m_jumpToLine = 0;
            m_focusSnippet = true;
        }

        if (m_focusSnippet)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusSnippet = false;
        }
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput |
                                    ImGuiInputTextFlags_CallbackResize |
                                    ImGuiInputTextFlags_CallbackAlways;
        // Text is a VIEW of generated output, never an editing surface --
        // edits would be silently stomped by the next regeneration; freeform
        // HLSL goes through the Custom (HLSL) node. The ONE exception is the
        // disaster case: a material whose graph is corrupt/missing loads
        // graphless, and hand-editing its snippet is the in-tool repair.
        if (m_editVertex ? IsGraphOwned() : ActiveGraphOwned())
            flags |= ImGuiInputTextFlags_ReadOnly;
        // Per-buffer widget identity: switching passes (or to the vertex
        // stage) swaps the buffer under the widget, which must not inherit
        // the previous buffer's edit state.
        ImGui::PushID(m_editVertex ? -2 : m_activePass);
        std::string* buf = &ActiveSnippet();
        if (generatedView)
        {
            m_generatedView = *buf;
            if (m_activePass == 0 && !m_data.vertexSnippet.empty())
            {
                m_generatedView += "\n// ---- vertex stage (Vertex Output node) ----\n";
                m_generatedView += m_data.vertexSnippet;
            }
            buf = &m_generatedView;
        }
        // +1 capacity: ImGui writes the terminator into the buffer it is given.
        // Height measured HERE, after the optional mode banner above -- the
        // input takes exactly what is left of the column.
        if (ImGui::InputTextMultiline("##snippet", buf->data(), buf->capacity() + 1,
                                      ImVec2(-1.0f, ImGui::GetContentRegionAvail().y),
                                      flags,
                                      &SnippetCallbackForwarder::Callback, this))
        {
            m_dirty = true;
            if (m_live)
                Rebuild();
        }
        ImGui::PopID();
    }

    bool ShaderEditorDocument::PassWireWouldCycle(std::uint32_t source,
                                                  std::uint32_t consumer) const
    {
        // Adding consumer.inputs += source cycles iff `consumer` is already
        // upstream of `source` (walk source's input ancestry).
        if (source == consumer)
            return true;
        std::vector<std::uint32_t> stack{ source };
        std::unordered_set<std::uint32_t> seen;
        while (!stack.empty())
        {
            const std::uint32_t c = stack.back();
            stack.pop_back();
            if (c == consumer)
                return true;
            if (!seen.insert(c).second || c == 0)
                continue;
            if (c - 1 < m_data.passes.size())
                for (std::uint32_t in : m_data.passes[c - 1].inputs)
                    stack.push_back(in);
        }
        return false;
    }

    bool ShaderEditorDocument::TopoSortPasses()
    {
        // Stable Kahn over chain indices (base = 0 is always first). Positions
        // ride each MaterialPass; active/view indices remap.
        const std::size_t n = m_data.passes.size();
        if (n == 0)
            return true;
        std::vector<std::uint32_t> order;   // new sequence of OLD chain indices
        std::vector<bool> placed(n + 1, false);
        placed[0] = true;
        bool progress = true;
        while (order.size() < n && progress)
        {
            progress = false;
            for (std::uint32_t c = 1; c <= n; ++c)
            {
                if (placed[c])
                    continue;
                bool ready = true;
                for (std::uint32_t in : m_data.passes[c - 1].inputs)
                    ready = ready &&
                            (in == Arcane::kSceneInput || (in <= n && placed[in]));
                if (!ready)
                    continue;
                placed[c] = true;
                order.push_back(c);
                progress = true;
            }
        }
        if (order.size() < n)
            return false;   // cycle -- the canvas refuses these at wire time

        std::vector<std::uint32_t> remap(n + 1, 0);
        for (std::size_t i = 0; i < order.size(); ++i)
            remap[order[i]] = static_cast<std::uint32_t>(i) + 1;
        std::vector<Arcane::MaterialPass> sorted;
        sorted.reserve(n);
        for (std::uint32_t old : order)
            sorted.push_back(std::move(m_data.passes[old - 1]));
        for (Arcane::MaterialPass& p : sorted)
            for (std::uint32_t& in : p.inputs)
                if (in != Arcane::kSceneInput)   // the scene is not a pass
                    in = remap[in];
        m_data.passes = std::move(sorted);
        if (m_activePass > 0 && m_activePass <= static_cast<int>(n))
            m_activePass = static_cast<int>(remap[static_cast<std::uint32_t>(m_activePass)]);
        if (m_viewPass > 0 && m_viewPass <= static_cast<int>(n))
            m_viewPass = static_cast<int>(remap[static_cast<std::uint32_t>(m_viewPass)]);
        return true;
    }

    void ShaderEditorDocument::DrawPassCanvas(float height)
    {
        // The pass DAG as a canvas (replaces the pass bar): chain index c is
        // node id c+1, the Output node is kPassOutputNodeId, and the WIRES ARE
        // THE DATA -- a link into input pin s of a pass IS inputs[s]. Every
        // structural gesture (wire/unwire/add/remove/reorder/rename) is ONE
        // undo step (whole pass-list before/after through PassListCommand).
        //
        // NO RENDERING LOD HERE, deliberately. The graph canvas degrades with
        // zoom (NodeLOD) because its nodes carry pin labels, inline literals,
        // payload widgets and a live thumbnail; a pass node carries a name, a
        // few pins and nothing else, so every tier below DefaultDetail would
        // degrade to what it already draws. UE's own tiers bottom out the same
        // way -- MediumDetail and up degrade NOTHING even in the Blueprint
        // graph (SNodePanel.h:70-90 calls MediumDetail "still drawn", and no
        // consumer in the engine tests for FullyZoomedIn at all). The zoom
        // TABLE is shared because navigation feel should not differ per canvas.
        if (!m_passCanvasCtx)
        {
            ed::Config cfg;
            cfg.SettingsFile = nullptr;
            // Same stops as the graph canvas: the zoom TABLE is a navigation
            // feel and belongs on every canvas in the editor. (The LOD tiers
            // built on top of it are not -- see DrawPassCanvas's note below.)
            ApplyZoomLevels(cfg);
            m_passCanvasCtx = ed::CreateEditor(&cfg);
            m_passCanvasSeeded = false;
        }
        const std::size_t total = 1 + m_data.passes.size();
        auto nodeOf = [](std::size_t chain) { return static_cast<std::uint32_t>(chain) + 1; };

        ed::SetCurrentEditor(m_passCanvasCtx);
        ed::Begin("##passcanvas", ImVec2(0.0f, height));

        const bool seededThisFrame = !m_passCanvasSeeded;
        if (seededThisFrame)
        {
            // Never-laid-out data (all zeros, incl. pre-canvas files): a simple
            // left-to-right row.
            bool anyPos = m_data.chainBaseX != 0.0f || m_data.chainBaseY != 0.0f ||
                          m_data.chainOutX != 0.0f || m_data.chainOutY != 0.0f;
            for (const Arcane::MaterialPass& p : m_data.passes)
                anyPos = anyPos || p.posX != 0.0f || p.posY != 0.0f;
            if (!anyPos)
            {
                m_data.chainBaseX = 40.0f;
                m_data.chainBaseY = 40.0f;
                for (std::size_t k = 0; k < m_data.passes.size(); ++k)
                {
                    m_data.passes[k].posX = 40.0f + 190.0f * static_cast<float>(k + 1);
                    m_data.passes[k].posY = 40.0f;
                }
                m_data.chainOutX = 40.0f + 190.0f * static_cast<float>(total);
                m_data.chainOutY = 40.0f;
            }
            // The Scene source sits left of the base by default (also heals
            // pre-scene files whose chainPos lacks it).
            if (m_data.chainSceneX == 0.0f && m_data.chainSceneY == 0.0f)
            {
                m_data.chainSceneX = m_data.chainBaseX - 170.0f;
                m_data.chainSceneY = m_data.chainBaseY + 90.0f;
            }
            ed::SetNodePosition(nodeOf(0), ImVec2(m_data.chainBaseX, m_data.chainBaseY));
            for (std::size_t k = 0; k < m_data.passes.size(); ++k)
                ed::SetNodePosition(nodeOf(k + 1),
                                    ImVec2(m_data.passes[k].posX, m_data.passes[k].posY));
            ed::SetNodePosition(kPassOutputNodeId,
                                ImVec2(m_data.chainOutX, m_data.chainOutY));
            ed::SetNodePosition(kPassSceneNodeId,
                                ImVec2(m_data.chainSceneX, m_data.chainSceneY));
            m_passCanvasSeeded = true;
        }

        // ---- nodes
        Arcane::FullscreenMaterialChain* chain = ChainMode() ? m_chain.get() : nullptr;
        for (std::size_t c = 0; c < total; ++c)
        {
            const std::uint32_t nodeId = nodeOf(c);
            ed::BeginNode(ed::NodeId(nodeId));
            ImGui::PushID(static_cast<int>(nodeId));

            bool passError = false;
            if (c < m_passJobs.size())
                for (const Arcane::ShaderDiag& d : m_passJobs[c].diags)
                    passError = passError ||
                                d.severity == Arcane::ShaderDiagSeverity::Error;
            const std::string title =
                (m_activePass == static_cast<int>(c) ? "> " : "") + PassLabel(c);
            if (passError)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "(!) %s", title.c_str());
            else
                ImGui::TextUnformatted(title.c_str());

            // Extra passes rename in-node (StableTextEdit's stable-buffer
            // commit; one undo step on deactivate-after-edit -- renames are not
            // structural, so the step pushes here rather than riding the
            // structural block).
            if (c >= 1)
            {
                Arcane::MaterialPass& pass = m_data.passes[c - 1];
                StableTextEdit("##passname", m_textEdit,
                               TextKey(TextEditKind::PassName, c),
                               pass.name, 120.0f,
                               [&](const char* text)
                               {
                                   PassListState before = CapturePassListState();
                                   pass.name = text;
                                   m_dirty = true;
                                   PushPassUndo("Rename Pass", std::move(before));
                               });
            }

            // Input pins: one per wired slot + a spare that accepts a new
            // wire. The BASE has pins too -- its slots are its scene inputs
            // (the base may read ONLY the Scene source; enforced at connect).
            const std::vector<std::uint32_t>& nodeInputs =
                c >= 1 ? m_data.passes[c - 1].inputs : m_data.baseInputs;
            for (std::size_t s = 0; s < nodeInputs.size(); ++s)
            {
                ed::BeginPin(InPin(nodeId, static_cast<std::uint32_t>(s)),
                             ed::PinKind::Input);
                ImGui::Text("-> in%zu", s);
                ed::EndPin();
            }
            if (nodeInputs.size() < Arcane::kMaxPassInputs)
            {
                ed::BeginPin(InPin(nodeId,
                                   static_cast<std::uint32_t>(nodeInputs.size())),
                             ed::PinKind::Input);
                ImGui::TextDisabled("-> +");
                ed::EndPin();
            }

            // Live thumbnail: the pass's own intermediate (chain mode; LINEAR,
            // so HDR clamps -- fine for a thumbnail). Single-pass materials
            // show the tonemapped preview on the base node instead.
            nvrhi::ITexture* thumb = chain ? chain->PassOutput(c) : nullptr;
            ImTextureID thumbId = 0;
            if (thumb)
                thumbId = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(thumb));
            else if (c == 0 && m_preview && PreviewReady())
                thumbId = static_cast<ImTextureID>(m_preview->TextureId());
            if (thumbId)
                ImGui::Image(thumbId, ImVec2(72.0f, 72.0f));

            ed::BeginPin(OutPin(nodeId, 0), ed::PinKind::Output);
            ImGui::TextUnformatted("out ->");
            ed::EndPin();

            ImGui::PopID();
            ed::EndNode();
        }

        // The Scene source: the EXTERNAL scene color (bound by the runtime
        // post hook; the checkerboard stand-in in the preview). Output pin
        // only; wiring it writes the kSceneInput sentinel.
        ed::BeginNode(ed::NodeId(kPassSceneNodeId));
        ImGui::TextUnformatted("Scene");
        if (nvrhi::ITexture* standIn = SceneStandIn())
            ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(standIn)),
                         ImVec2(72.0f, 72.0f));
        ed::BeginPin(OutPin(kPassSceneNodeId, 0), ed::PinKind::Output);
        ImGui::TextUnformatted("scene ->");
        ed::EndPin();
        ed::EndNode();

        // The Output node: shows the final image; its wire marks the LAST pass
        // (execution order's tail = what single-material consumers see).
        ed::BeginNode(ed::NodeId(kPassOutputNodeId));
        ImGui::TextUnformatted("Output");
        ed::BeginPin(InPin(kPassOutputNodeId, 0), ed::PinKind::Input);
        ImGui::TextUnformatted("-> final");
        ed::EndPin();
        if (nvrhi::ITexture* finalTex = chain ? chain->PassOutput(total - 1) : nullptr)
            ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(finalTex)),
                         ImVec2(72.0f, 72.0f));
        ed::EndNode();

        // ---- links (derived from the data each frame; ids = list index + 1).
        // Sentinel entries draw from the Scene source; the base (c == 0) only
        // ever has those.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> linkSlots;   // consumer, slot
        for (std::size_t c = 0; c < total; ++c)
        {
            const std::vector<std::uint32_t>& ins =
                c >= 1 ? m_data.passes[c - 1].inputs : m_data.baseInputs;
            for (std::size_t s = 0; s < ins.size(); ++s)
            {
                const std::uint32_t src = ins[s];
                linkSlots.emplace_back(static_cast<std::uint32_t>(c),
                                       static_cast<std::uint32_t>(s));
                ed::Link(ed::LinkId(linkSlots.size()),
                         src == Arcane::kSceneInput ? OutPin(kPassSceneNodeId, 0)
                                                    : OutPin(nodeOf(src), 0),
                         InPin(nodeOf(c), static_cast<std::uint32_t>(s)));
            }
        }
        ed::Link(ed::LinkId(kPassOutputLinkId), OutPin(nodeOf(total - 1), 0),
                 InPin(kPassOutputNodeId, 0));

        // ---- wire edits
        // Structural gestures land on the undo stack as whole pass-list
        // before/after (the before captures lazily at the FIRST mutation of
        // the frame; the push rides the structural block at the end).
        bool structural = false;
        std::optional<PassListState> passBefore;
        const char* passEditLabel = "Edit Passes";
        auto capturePassBefore = [&](const char* label)
        {
            if (!passBefore)
                passBefore = CapturePassListState();
            passEditLabel = label;
        };
        if (ed::BeginCreate())
        {
            ed::PinId aId, bId;
            if (ed::QueryNewLink(&aId, &bId))
            {
                const DecodedPin a = DecodePin(aId);
                const DecodedPin b = DecodePin(bId);
                const DecodedPin& out = a.isInput ? b : a;
                const DecodedPin& in = a.isInput ? a : b;
                const bool sceneSource = out.node == kPassSceneNodeId;
                bool valid = a.valid && b.valid && a.isInput != b.isInput &&
                             out.node != kPassOutputNodeId &&
                             (sceneSource ||
                              (out.node >= 1 && out.node <= total));
                const std::uint32_t source =
                    sceneSource ? Arcane::kSceneInput : out.node - 1;
                if (valid && in.node == kPassOutputNodeId)
                {
                    // Make `source` the final pass: legal only when nothing
                    // reads it (a consumer must execute after it). The Scene
                    // is a source, never the final image.
                    bool hasDependent = false;
                    for (const Arcane::MaterialPass& p : m_data.passes)
                        for (std::uint32_t pin : p.inputs)
                            hasDependent = hasDependent || pin == source;
                    if (sceneSource || source == 0 || source == total - 1 ||
                        hasDependent)
                        ed::RejectNewItem();
                    else if (ed::AcceptNewItem())
                    {
                        capturePassBefore("Reorder Passes");
                        Arcane::MaterialPass moved =
                            std::move(m_data.passes[source - 1]);
                        m_data.passes.erase(m_data.passes.begin() +
                                            static_cast<std::ptrdiff_t>(source - 1));
                        m_data.passes.push_back(std::move(moved));
                        for (Arcane::MaterialPass& p : m_data.passes)
                            for (std::uint32_t& pin : p.inputs)
                            {
                                if (pin == Arcane::kSceneInput)
                                    continue;   // the scene is not a pass
                                pin = pin == source
                                          ? static_cast<std::uint32_t>(total - 1)
                                          : pin > source ? pin - 1 : pin;
                            }
                        if (m_activePass == static_cast<int>(source))
                            m_activePass = static_cast<int>(total - 1);
                        else if (m_activePass > static_cast<int>(source))
                            --m_activePass;
                        m_viewPass = -1;
                        structural = true;
                    }
                }
                else
                {
                    // The BASE (in.node == 1) accepts ONLY the Scene source;
                    // scene wires skip the cycle check (the scene is external,
                    // it cannot depend on any pass).
                    const std::uint32_t consumer = in.node - 1;
                    valid = valid && in.node >= 1 && in.node <= total &&
                            (consumer > 0 || sceneSource);
                    if (valid)
                    {
                        const std::vector<std::uint32_t>& ins =
                            consumer == 0 ? m_data.baseInputs
                                          : m_data.passes[consumer - 1].inputs;
                        valid = in.pin <= ins.size() &&
                                in.pin < Arcane::kMaxPassInputs &&
                                (sceneSource ||
                                 !PassWireWouldCycle(source, consumer));
                    }
                    if (!valid)
                        ed::RejectNewItem();
                    else if (ed::AcceptNewItem())
                    {
                        capturePassBefore("Wire Pass");
                        std::vector<std::uint32_t>& ins =
                            consumer == 0 ? m_data.baseInputs
                                          : m_data.passes[consumer - 1].inputs;
                        if (in.pin < ins.size())
                            ins[in.pin] = source;   // silent replace
                        else
                            ins.push_back(source);  // the spare pin
                        TopoSortPasses();
                        structural = true;
                    }
                }
            }
        }
        ed::EndCreate();   // UNCONDITIONAL (the material-canvas lesson)

        // ---- deletions: links = unwire a slot; nodes = remove the pass
        std::vector<std::pair<std::uint32_t, std::uint32_t>> unwire;
        std::vector<std::uint32_t> removePasses;   // chain indices
        if (ed::BeginDelete())
        {
            ed::LinkId lid;
            while (ed::QueryDeletedLink(&lid))
            {
                const std::size_t idx = static_cast<std::size_t>(lid.Get()) - 1;
                if (lid.Get() == kPassOutputLinkId || idx >= linkSlots.size())
                    ed::RejectDeletedItem();   // the final wire is structural
                else if (ed::AcceptDeletedItem())
                    unwire.push_back(linkSlots[idx]);
            }
            ed::NodeId nid;
            while (ed::QueryDeletedNode(&nid))
            {
                const std::uint32_t id = static_cast<std::uint32_t>(nid.Get());
                if (id < 2 || id > total)   // base + Output are fixed
                {
                    ed::RejectDeletedItem();
                    continue;
                }
                if (ed::AcceptDeletedItem())
                    removePasses.push_back(id - 1);
            }
        }
        ed::EndDelete();   // UNCONDITIONAL

        if (!unwire.empty() || !removePasses.empty())
        {
            capturePassBefore(removePasses.empty() ? "Unwire Pass" : "Remove Pass");
            // Unwire first (descending slot so indices stay valid), then remove
            // passes (descending chain index), fixing every reference.
            std::sort(unwire.rbegin(), unwire.rend());
            for (const auto& [consumer, slot] : unwire)
            {
                std::vector<std::uint32_t>& ins =
                    consumer == 0 ? m_data.baseInputs
                                  : m_data.passes[consumer - 1].inputs;
                if (slot < ins.size())
                    ins.erase(ins.begin() + slot);
            }
            std::sort(removePasses.rbegin(), removePasses.rend());
            for (std::uint32_t r : removePasses)
            {
                m_data.passes.erase(m_data.passes.begin() +
                                    static_cast<std::ptrdiff_t>(r - 1));
                for (Arcane::MaterialPass& p : m_data.passes)
                {
                    std::erase(p.inputs, r);
                    for (std::uint32_t& in : p.inputs)
                        if (in != Arcane::kSceneInput && in > r)
                            --in;
                }
                if (m_activePass >= static_cast<int>(r))
                    --m_activePass;
            }
            m_activePass = std::clamp(m_activePass, 0,
                                      static_cast<int>(m_data.passes.size()));
            m_viewPass = -1;
            structural = true;
        }

        // F = frame, exactly like the graph canvas.
        if (ImGui::IsWindowHovered() && !ImGui::GetIO().WantTextInput &&
            ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            if (ed::GetSelectedObjectCount() > 0)
                ed::NavigateToSelection(true);
            else
                ed::NavigateToContent();
        }

        // ---- selection -> edited pass; double-click -> viewed pass. Only on
        // selection CHANGES -- re-asserting every frame would stomp any other
        // writer of m_activePass (the pass canvas, and whatever navigation the
        // Problems panel eventually adds).
        {
            if (ed::HasSelectionChanged())
            {
                std::vector<ed::NodeId> sel(
                    static_cast<std::size_t>(std::max(0, ed::GetSelectedObjectCount())));
                if (!sel.empty())
                {
                    const int count = ed::GetSelectedNodes(sel.data(),
                                                           static_cast<int>(sel.size()));
                    if (count == 1)
                    {
                        const std::uint32_t id =
                            static_cast<std::uint32_t>(sel[0].Get());
                        if (id >= 1 && id <= total)
                            m_activePass = static_cast<int>(id - 1);
                    }
                }
            }
            const std::uint32_t dbl =
                static_cast<std::uint32_t>(ed::GetDoubleClickedNode().Get());
            if (dbl == kPassOutputNodeId)
                m_viewPass = -1;
            else if (dbl >= 1 && dbl <= total)
                m_viewPass = dbl == total ? -1 : static_cast<int>(dbl - 1);
        }

        // ---- context menus (Suspend: popups live in screen space)
        ed::Suspend();
        {
            ed::NodeId ctxNode;
            if (ed::ShowNodeContextMenu(&ctxNode))
            {
                m_passCtxNode = static_cast<std::uint32_t>(ctxNode.Get());
                ImGui::OpenPopup("##passnodemenu");
            }
            else if (ed::ShowBackgroundContextMenu())
            {
                const ImVec2 p = ImGui::GetMousePos();
                m_passPopupX = p.x;
                m_passPopupY = p.y;
                ImGui::OpenPopup("##passbgmenu");
            }
            if (ImGui::BeginPopup("##passnodemenu"))
            {
                const std::uint32_t id = m_passCtxNode;
                if (id == kPassOutputNodeId)
                {
                    if (ImGui::MenuItem("View Final"))
                        m_viewPass = -1;
                }
                else if (id == kPassSceneNodeId)
                {
                    ImGui::TextDisabled("the scene color (bound by the runtime "
                                        "post hook; checkerboard here)");
                }
                else if (id >= 1 && id <= total)
                {
                    if (ImGui::MenuItem("View This Pass"))
                        m_viewPass = id == total ? -1 : static_cast<int>(id - 1);
                    if (id >= 2 && ImGui::MenuItem("Remove Pass"))
                    {
                        capturePassBefore("Remove Pass");
                        const std::uint32_t r = id - 1;
                        m_data.passes.erase(m_data.passes.begin() +
                                            static_cast<std::ptrdiff_t>(r - 1));
                        for (Arcane::MaterialPass& p : m_data.passes)
                        {
                            std::erase(p.inputs, r);
                            for (std::uint32_t& in : p.inputs)
                                if (in != Arcane::kSceneInput && in > r)
                                    --in;
                        }
                        if (m_activePass >= static_cast<int>(r))
                            --m_activePass;
                        m_activePass = std::clamp(
                            m_activePass, 0, static_cast<int>(m_data.passes.size()));
                        m_viewPass = -1;
                        structural = true;
                    }
                }
                ImGui::EndPopup();
            }
            if (ImGui::BeginPopup("##passbgmenu"))
            {
                if (ImGui::MenuItem("Add Pass"))
                {
                    capturePassBefore("Add Pass");
                    Arcane::MaterialPass p;
                    p.name = "pass " + std::to_string(m_data.passes.size() + 1);
                    // UE model: new passes are GRAPH-owned. Starter = Pass
                    // Input (slot 0) wired to Output -- a visible passthrough,
                    // never an empty canvas. The snippet regenerates from it.
                    Arcane::MaterialGraph pg;
                    Arcane::GraphNode out;
                    out.id = 1;
                    out.type = Arcane::GraphNodeType::Output;
                    out.posX = 360.0f;
                    out.posY = 120.0f;
                    Arcane::GraphNode in;
                    in.id = 2;
                    in.type = Arcane::GraphNodeType::PassInput;
                    in.posX = 100.0f;
                    in.posY = 120.0f;
                    pg.nodes = { out, in };
                    Arcane::GraphLink link;
                    link.fromNode = 2;
                    link.toNode = 1;
                    pg.links.push_back(link);
                    pg.nextId = 3;
                    p.graph = std::move(pg);
                    // Default wiring: read the current final (linear extend).
                    p.inputs = { static_cast<std::uint32_t>(total - 1) };
                    const ImVec2 canvasPos =
                        ed::ScreenToCanvas(ImVec2(m_passPopupX, m_passPopupY));
                    p.posX = canvasPos.x;
                    p.posY = canvasPos.y;
                    m_data.passes.push_back(std::move(p));
                    m_activePass = static_cast<int>(m_data.passes.size());
                    structural = true;
                }
                ImGui::EndPopup();
            }
        }
        ed::Resume();

        // ---- position readback (skip the seed frame)
        if (!seededThisFrame && !structural)
        {
            auto readback = [&](std::uint32_t nodeId, float& x, float& y)
            {
                const ImVec2 p = ed::GetNodePosition(ed::NodeId(nodeId));
                if (p.x != x || p.y != y)
                {
                    x = p.x;
                    y = p.y;
                    m_dirty = true;
                }
            };
            readback(nodeOf(0), m_data.chainBaseX, m_data.chainBaseY);
            for (std::size_t k = 0; k < m_data.passes.size(); ++k)
                readback(nodeOf(k + 1), m_data.passes[k].posX, m_data.passes[k].posY);
            readback(kPassOutputNodeId, m_data.chainOutX, m_data.chainOutY);
            readback(kPassSceneNodeId, m_data.chainSceneX, m_data.chainSceneY);
        }

        ed::End();
        ed::SetCurrentEditor(nullptr);

        if (structural)
        {
            // Indices moved under the canvas: re-seed node ids from the data
            // next frame, then re-CODEGEN (rewires change which PassInput
            // slots are valid) and recompile the chain.
            m_passCanvasSeeded = false;
            m_dirty = true;
            if (m_live)
                RegenerateFromGraph();
            if (passBefore)
                PushPassUndo(passEditLabel, std::move(*passBefore));
        }
    }

    void ShaderEditorDocument::ForEachDiagnosticRow(
        Arcane::FunctionRef<void(bool, const std::string&)> fn)
    {
        // Graph-level codegen errors first, for EVERY pass's graph -- these are
        // the ones the canvas also badges (NodeBadged/RebuildDiagBadges).
        for (std::size_t c = 0; c < m_passGraphErrors.size(); ++c)
        {
            for (const Arcane::GraphError& e : m_passGraphErrors[c])
            {
                std::string row = m_data.passes.empty() ? std::string("graph: ")
                                                        : PassLabel(c) + " graph: ";
                if (e.nodeId != 0)
                {
                    std::optional<Arcane::MaterialGraph>& g = GraphOptAt(c);
                    const Arcane::GraphNode* n = g ? g->FindNode(e.nodeId) : nullptr;
                    row += (n ? std::string(Arcane::GraphNodeInfo(n->type).display)
                              : std::string("node")) +
                           " #" + std::to_string(e.nodeId) + ": ";
                }
                fn(true, row + e.message);
            }
        }
        // Stitch/parse failures (instance parent-chain resolution included).
        for (const std::string& e : m_parseErrors)
            fn(true, "parse: " + e);
        // Vertex-stage rows: ONLY diags whose line falls inside the vertex
        // body -- both stages compile the same TU, so pixel-body errors appear
        // in the vs result too and are already carried by the compile rows
        // below. Same filter as HasErrors.
        if (!m_data.vertexSnippet.empty())
        {
            const int vsLines = 1 + static_cast<int>(std::count(
                m_data.vertexSnippet.begin(), m_data.vertexSnippet.end(), '\n'));
            for (const Arcane::ShaderDiag& d : m_vsDiags)
            {
                const int rel = d.line - m_vsLineOffset;
                if (rel < 1 || rel > vsLines)
                    continue;
                const bool isError = d.severity == Arcane::ShaderDiagSeverity::Error;
                fn(isError, "vertex: " + std::string(isError ? "error" : "warning") +
                                "(" + std::to_string(rel) + "): " + d.message);
            }
        }
        // Compile diags. Diag lines arrive in STITCHED-source space; the pass's
        // line offset maps them back into the buffer the designer sees.
        auto compileRow = [&](const Arcane::ShaderDiag& d, int offset,
                              const std::string& prefix)
        {
            const bool isError = d.severity == Arcane::ShaderDiagSeverity::Error;
            const int line = d.line > offset ? d.line - offset : 1;
            fn(isError, prefix + (isError ? "error" : "warning") +
                            "(" + std::to_string(line) + "): " + d.message);
        };
        if (ChainMode())
        {
            // Chain mode owns them per pass; m_diags only MIRRORS pass 0 there
            // (for the badges), so the single-path loop must not also run.
            for (std::size_t p = 0; p < m_passJobs.size(); ++p)
            {
                const int offset = p < m_passLineOffsets.size() ? m_passLineOffsets[p] : 0;
                for (const Arcane::ShaderDiag& d : m_passJobs[p].diags)
                    compileRow(d, offset, PassLabel(p) + ": ");
            }
            return;
        }
        for (const Arcane::ShaderDiag& d : m_diags)
            compileRow(d, m_snippetLineOffset, std::string());
    }

    void ShaderEditorDocument::PublishDiagnostics()
    {
        // ANTI-SPAM POLICY (the reason this is not a plain "log on compile").
        // The editor regenerates and recompiles on EVERY edit -- one drag of a
        // param is dozens of Rebuilds, each landing the SAME diagnostics -- so
        // emission is gated on the CONTENT of the row set, never on the
        // compile/regenerate events that produce it. An FNV-1a signature over
        // (severity, row text) in traversal order is compared against the last
        // EMITTED one; an identical set is silence. By design:
        //   - dragging on a broken material emits nothing after the first frame;
        //   - clean -> broken emits every row, once;
        //   - broken -> clean emits ONE "diagnostics cleared" line: the console
        //     is append-only, so the rows above it need retracting;
        //   - broken -> DIFFERENTLY broken emits the new set once;
        //   - a set that flaps back to an earlier shape DOES re-emit -- the
        //     comparison is against the last emission, not a history.
        // The empty set signs as 0, which is also the initial value, so opening
        // an already-clean material says nothing.
        //
        // Running this from Tick (rather than from the mutation sites) is what
        // makes the gate total: every path that can change a diagnostic --
        // ConsumeResult, Rebuild, RegenerateFromGraph, ReloadFromDisk, the
        // parent-chain resolver -- is covered without enumerating them. Idle
        // cost is one traversal of a set that is normally empty.
        std::uint64_t sig = 0xcbf29ce484222325ull;
        std::size_t rows = 0;
        ForEachDiagnosticRow([&](bool isError, const std::string& row)
        {
            ++rows;
            sig = (sig ^ (isError ? 1ull : 0ull)) * 0x100000001b3ull;
            for (const char ch : row)
                sig = (sig ^ static_cast<std::uint8_t>(ch)) * 0x100000001b3ull;
        });
        if (rows == 0)
            sig = 0;
        else if (sig == 0)
            sig = 1;   // 0 is reserved for "clean"; a real set hashing to 0 is a lottery win
        if (sig == m_emittedDiagSig)
            return;
        m_emittedDiagSig = sig;
        if (rows == 0)
        {
            // Reached only from a non-zero signature (equal signatures returned
            // above), so this really is a broken -> clean transition.
            ARC_INFO("'{}': shader diagnostics cleared", m_title);
            return;
        }
        // Severity rides BOTH the log level and the row text: the Console panel
        // renders the sink's payload only (EditorApp.cpp:494-497, and the panel
        // at EditorPanels.cpp:288-297 prints it verbatim), so a level
        // alone would be invisible there. The material name is the source tag.
        ForEachDiagnosticRow([&](bool isError, const std::string& row)
        {
            if (isError) ARC_ERROR("'{}': {}", m_title, row);
            else         ARC_WARN("'{}': {}", m_title, row);
        });
    }

    void ShaderEditorDocument::DrawPreviewPanel(float height)
    {
        ImGui::BeginChild("##preview", ImVec2(0, height), ImGuiChildFlags_Borders);
        if (m_preview && PreviewReady())
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float texW = static_cast<float>(m_preview->Width());
            const float texH = static_cast<float>(m_preview->Height());
            const float scale = (std::min)(avail.x > 0 ? avail.x / texW : 1.0f,
                                           avail.y > 0 ? avail.y / texH : 1.0f);
            const float s = scale > 0.0f ? scale : 1.0f;
            ImGui::Image(static_cast<ImTextureID>(m_preview->TextureId()),
                         ImVec2(texW * s, texH * s));
        }
        else
        {
            ImGui::TextDisabled("compiling...");
        }
        ImGui::EndChild();
    }

    // ------------------------------------------------------------ graph mode
    std::optional<Arcane::MaterialGraph>& ShaderEditorDocument::GraphOptAt(std::size_t pass)
    {
        if (pass > 0 && pass <= m_data.passes.size())
            return m_data.passes[pass - 1].graph;
        return m_data.graph;
    }

    std::optional<Arcane::MaterialGraph>& ShaderEditorDocument::ActiveGraphOpt()
    {
        return GraphOptAt(static_cast<std::size_t>(std::max(0, m_activePass)));
    }

    void ShaderEditorDocument::RegenerateFromGraph()
    {
        const std::size_t total = 1 + m_data.passes.size();
        m_passGraphErrors.assign(total, std::vector<Arcane::GraphError>{});
        m_passLineNodeIds.assign(total, std::vector<std::uint32_t>{});
        bool anyError = false;
        for (std::size_t c = 0; c < total; ++c)
        {
            std::optional<Arcane::MaterialGraph>& g = GraphOptAt(c);
            if (!g)
                continue;
            // The wired-slot context: PassInput nodes may only read slots the
            // pass canvas actually wired (the base's slots are its scene
            // inputs).
            const std::uint32_t avail = static_cast<std::uint32_t>(
                c == 0 ? m_data.baseInputs.size()
                       : m_data.passes[c - 1].inputs.size());
            Arcane::GraphCodegenResult r = Arcane::GenerateGraphSnippet(
                *g, SurfaceOf(m_surface), avail, /*passGraph=*/c > 0);
            if (!r.Ok())
            {
                m_passGraphErrors[c] = std::move(r.errors);
                anyError = true;
                continue;
            }
            m_passLineNodeIds[c] = std::move(r.lineNodeIds);
            (c == 0 ? m_snippet : m_data.passes[c - 1].snippet) = std::move(r.snippet);
            // The base graph OWNS the vertex stage: its Vertex Output node
            // generates the body (absent node = empty = passthrough).
            if (c == 0)
                m_data.vertexSnippet = std::move(r.vertexSnippet);
        }
        if (!anyError)
            Rebuild();   // any codegen error keeps last-good bound; badges show why
        // Per-node thumbnails ride every regeneration; the hash gate keeps
        // untouched subgraphs from recompiling.
        RefreshNodePreviews();
    }

    void ShaderEditorDocument::ApplyGraphState(std::size_t pass,
                                               std::optional<Arcane::MaterialGraph> state)
    {
        if (pass > m_data.passes.size())
            return;   // the pass was removed since the step was pushed
        GraphOptAt(pass) = std::move(state);
        if (static_cast<int>(pass) == m_activePass)
            m_graphPositionsApplied = false;   // re-seed the canvas from the data
        m_dirty = true;
        RegenerateFromGraph();
    }

    void ShaderEditorDocument::PushGraphUndo(const char* label,
                                             std::optional<Arcane::MaterialGraph> before)
    {
        PushGraphUndo(label, std::move(before),
                      static_cast<std::size_t>(std::max(0, m_activePass)));
    }

    void ShaderEditorDocument::PushGraphUndo(const char* label,
                                             std::optional<Arcane::MaterialGraph> before,
                                             std::size_t pass)
    {
        // GraphOptAt range-checks (an out-of-range pass falls back to the base,
        // exactly as ActiveGraphOpt's own clamp does), and ApplyGraphState
        // refuses a pass that has since been removed -- so an OUT-OF-RANGE
        // pinned index degrades to an inert step rather than a misdirected
        // write. An index that is still IN RANGE but now names a DIFFERENT
        // pass (the list was reordered or an earlier pass removed) is not
        // covered by either check and would write to that other pass: a
        // pre-existing GraphEditCommand weakness (every step stores a bare
        // index), not one this bracket introduces.
        if (m_services.undo)
            m_services.undo->Push(std::make_unique<GraphEditCommand>(
                m_anchor, label, pass, std::move(before), GraphOptAt(pass)));
    }

    // --------------------------------------------- external file changes
    void ShaderEditorDocument::ReloadFromDisk()
    {
        auto data = Arcane::LoadMaterialAsset(m_path);
        if (!data)
        {
            ARC_WARN("'{}': reload from disk failed -- keeping the in-memory copy",
                     m_title);
            return;
        }
        m_data = std::move(*data);
        m_snippet = m_data.snippet;
        m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name;
        m_windowLabel = m_title + (m_data.IsInstance() ? " (Instance)###matdoc_"
                                                       : " (Material)###matdoc_") +
                        m_data.id.ToString();
        m_dirty = false;
        m_paramsBaseDirty = false;
        m_savedParamSerial = 0;
        m_instance.reset();      // the file's values are truth again (rebind reapplies)
        m_paramRenames.clear();
        m_parentChain.clear();
        m_parseErrors.clear();
        m_activePass = 0;
        m_viewPass = -1;
        m_passCanvasSeeded = false;
        m_graphPositionsApplied = false;
        m_graphShownPass = -1;
        if (!IsInstance() || ResolveParentChain())
        {
            const std::string& kind =
                IsInstance() && !m_parentChain.empty() ? m_parentChain.back().kind
                                                       : m_data.kind;
            m_surface = Arcane::MaterialSurfaceForKind(kind) ==
                                Arcane::MaterialSurface::Sprite ? 1 : 0;
            // Sprite preview re-registers fresh at the next bind (the surface-
            // switch pattern).
            m_previewSpriteMaterial = Arcane::Batcher2D::kInvalidMaterialId;
            m_previewVs = nullptr;
            m_previewPs = nullptr;
            RegenerateFromGraph();
        }
    }

    bool ShaderEditorDocument::DependsOn(const Arcane::Guid& id) const
    {
        for (const Arcane::MaterialAssetData& p : m_parentChain)
            if (p.id == id)
                return true;
        return false;
    }

    void ShaderEditorDocument::RefreshParentChain()
    {
        if (!IsInstance())
            return;
        m_parseErrors.clear();
        if (ResolveParentChain())
            Rebuild();   // failure keeps last-good bound + the errors visible
    }

    // --------------------------------------------- assisted param rename
    void ShaderEditorDocument::BeginParamRename(const std::string& oldName,
                                                const std::string& newName)
    {
        if (IsInstance() || oldName.empty() || newName.empty() || oldName == newName)
            return;
        // Sole-declarer guard: if ANOTHER node (any pass's graph) still
        // declares the old name, this edit was a decl SPLIT, not a rename --
        // the shared declaration lives on, nothing orphans.
        for (std::size_t c = 0; c <= m_data.passes.size(); ++c)
        {
            const std::optional<Arcane::MaterialGraph>& opt = GraphOptAt(c);
            if (!opt)
                continue;
            for (const Arcane::GraphNode& gn : opt->nodes)
                if ((gn.type == Arcane::GraphNodeType::Param ||
                     gn.type == Arcane::GraphNodeType::TextureSample) &&
                    gn.paramName == oldName)
                    return;
        }

        // Local fix (unconditional): this document's own saved value follows
        // the rename; the live override migrates at the next rebind through
        // the pending-rename translation.
        RekeySavedParam(m_data.params, oldName, newName);
        m_paramRenames.emplace_back(oldName, newName);

        // Discovery: every registered instance whose parent chain reaches
        // THIS base (any depth, cycle-guarded) and whose saved params carry
        // the old name. GUID identity is what makes this possible at all --
        // Unity structurally cannot find these files.
        m_renameTargets.clear();
        const Arcane::Project* project =
            m_services.runtime ? m_services.runtime->CurrentProject() : nullptr;
        if (!project)
            return;
        for (const AssetEntry& e : BuildAssetEntries(project->Registry()))
        {
            if (e.kind != AssetKind::Material || e.guid == m_data.id)
                continue;
            const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(e.guid));
            if (!path)
                continue;
            const auto data = Arcane::LoadMaterialAsset(*path);
            if (!data || !data->IsInstance())
                continue;

            bool reaches = false;
            Arcane::Guid cursor = data->parent;
            std::vector<Arcane::Guid> visited{ data->id };
            while (cursor.IsValid())
            {
                if (cursor == m_data.id)
                {
                    reaches = true;
                    break;
                }
                bool seen = false;
                for (const Arcane::Guid& v : visited)
                    seen = seen || v == cursor;
                if (seen)
                    break;   // cycle -- refuse quietly
                visited.push_back(cursor);
                const auto hopPath = project->ResolveAsset(Arcane::AssetId::FromGuid(cursor));
                if (!hopPath)
                    break;
                const auto hop = Arcane::LoadMaterialAsset(*hopPath);
                if (!hop)
                    break;
                cursor = hop->parent;
            }
            if (!reaches)
                continue;
            bool carries = false;
            for (const auto& [pname, pvalue] : data->params)
                carries = carries || pname == oldName;
            if (!carries)
                continue;
            m_renameTargets.push_back(
                { e.guid, *path, data->name.empty() ? e.name : data->name });
        }
        if (m_renameTargets.empty())
            return;   // nothing to propagate -- no modal
        m_renameOld = oldName;
        m_renameNew = newName;
        m_renameRequest = true;
    }

    void ShaderEditorDocument::PatchParamRename(const std::string& oldName,
                                                const std::string& newName)
    {
        // The base's propagation rewrote this instance's FILE; re-key the open
        // document's memory to match (unsaved edits stay untouched) and queue
        // the override migration for whenever the renamed base rebinds here.
        RekeySavedParam(m_data.params, oldName, newName);
        m_paramRenames.emplace_back(oldName, newName);
    }

    std::uint32_t ShaderEditorDocument::TranslateOverrideHash(
        std::uint32_t hash, const Arcane::MaterialTemplate& templ) const
    {
        // A pending rename migrates an override across the template boundary
        // the moment it MATERIALIZES (the target declares new, not old) -- and
        // in reverse when an undo rolled the template back. Inert otherwise,
        // so a stale pair can never mistranslate.
        for (const auto& [oldName, newName] : m_paramRenames)
        {
            const std::uint32_t oh = Arcane::HashParamName(oldName);
            const std::uint32_t nh = Arcane::HashParamName(newName);
            if (hash == oh && templ.Find(nh) && !templ.Find(oh))
                hash = nh;
            else if (hash == nh && templ.Find(oh) && !templ.Find(nh))
                hash = oh;
        }
        return hash;
    }

    ShaderEditorDocument::PassListState ShaderEditorDocument::CapturePassListState() const
    {
        return { m_data.passes, m_data.baseInputs, m_activePass, m_viewPass };
    }

    void ShaderEditorDocument::ApplyPassListState(PassListState state)
    {
        if (IsInstance())
            return;   // instances never carry passes
        m_data.passes = std::move(state.passes);
        m_data.baseInputs = std::move(state.baseInputs);
        const int count = static_cast<int>(m_data.passes.size());
        m_activePass = std::clamp(state.activePass, 0, count);
        m_viewPass = std::clamp(state.viewPass, -1, count);
        // Chain indices moved under BOTH canvases: re-seed the pass canvas,
        // and force the graph canvas through its pass-switch path (positions,
        // selection, badges, thumbnails).
        m_passCanvasSeeded = false;
        m_graphPositionsApplied = false;
        m_graphShownPass = -1;
        m_dirty = true;
        RegenerateFromGraph();
    }

    void ShaderEditorDocument::PushPassUndo(const char* label, PassListState before)
    {
        if (m_services.undo)
            m_services.undo->Push(std::make_unique<PassListCommand>(
                m_anchor, label, std::move(before), CapturePassListState()));
    }

    bool ShaderEditorDocument::NodeBadged(std::uint32_t nodeId) const
    {
        const std::size_t c = static_cast<std::size_t>(std::max(0, m_activePass));
        if (c < m_passGraphErrors.size())
            for (const Arcane::GraphError& e : m_passGraphErrors[c])
                if (e.nodeId == nodeId)
                    return true;
        for (std::uint32_t id : m_diagBadgeNodes)
            if (id == nodeId)
                return true;
        return false;
    }

    void ShaderEditorDocument::RebuildDiagBadges()
    {
        // Compile-diag badges for the ACTIVE pass's canvas: that pass's diags,
        // line offset, and line map (single-path docs are pass 0 throughout).
        m_diagBadgeNodes.clear();
        const std::size_t c = static_cast<std::size_t>(std::max(0, m_activePass));
        if (c >= m_passLineNodeIds.size() || m_passLineNodeIds[c].empty())
            return;
        const std::vector<Arcane::ShaderDiag>* diags = &m_diags;
        int offset = m_snippetLineOffset;
        if (ChainMode() && c < m_passJobs.size())
        {
            diags = &m_passJobs[c].diags;
            if (c < m_passLineOffsets.size())
                offset = m_passLineOffsets[c];
        }
        const std::vector<std::uint32_t>& lineMap = m_passLineNodeIds[c];
        for (const Arcane::ShaderDiag& d : *diags)
        {
            if (d.severity != Arcane::ShaderDiagSeverity::Error)
                continue;
            // stitched line -> snippet line -> statement's node (the line map).
            const int snippetLine = d.line - offset;
            const std::size_t idx = static_cast<std::size_t>(snippetLine) - 1;
            if (snippetLine >= 1 && idx < lineMap.size() && lineMap[idx] != 0)
                m_diagBadgeNodes.push_back(lineMap[idx]);
        }
    }

    // ------------------------------------------------- node preview thumbnails
    void ShaderEditorDocument::RefreshNodePreviews()
    {
        const bool eligible = m_services.device && m_services.compiler &&
                              m_services.sources && !IsInstance() &&
                              m_showNodePreviews && ActiveGraphOwned();
        if (!eligible)
        {
            for (auto& [id, np] : m_nodePreviews)
                if (np.tex)
                    m_nodePreviewRetired.push_back(np.tex);
            m_nodePreviews.clear();
            m_nodePreviewsPass = -1;
            return;
        }
        std::size_t pass = static_cast<std::size_t>(std::max(0, m_activePass));
        if (pass > m_data.passes.size())
            pass = 0;   // stale selection after an outside reload
        if (m_nodePreviewsPass != static_cast<int>(pass))
        {
            for (auto& [id, np] : m_nodePreviews)
                if (np.tex)
                    m_nodePreviewRetired.push_back(np.tex);
            m_nodePreviews.clear();
            m_nodePreviewsPass = static_cast<int>(pass);
        }

        // Thumbnails always render the FULLSCREEN surface (see
        // GenerateNodePreviewSnippet) -- sprite-gated subgraphs simply refuse.
        const auto templateText = m_services.sources->Get(
            Arcane::MaterialTemplateFile(Arcane::MaterialSurface::Fullscreen));
        if (!templateText)
            return;

        // The ONE passthrough VS every thumbnail shares (register assignments
        // are explicit in the generated source, so a VS compiled without a
        // preview's texture declarations still matches its layout).
        if (!m_nodePreviewVs && m_nodePreviewVsJob == 0)
        {
            Arcane::MaterialBuildResult vsBuild = Arcane::BuildMaterialShaderSource(
                *templateText, "float4 shade(Varyings v) { return float4(0,0,0,1); }\n",
                m_title + "_thumbvs");
            Arcane::ShaderCompileRequest req;
            req.debugName = m_title + "_thumbvs.hlsl";
            req.sourceUtf8 = std::move(vsBuild.hlsl);
            req.entry = Arcane::kVsEntry;
            req.profile = Arcane::kVsProfile;
            req.coalesceKey = NodePreviewKey(m_data.id, 0xFFFF, 0);
            m_nodePreviewVsJob = m_services.compiler->Submit(std::move(req), Now());
        }

        const Arcane::MaterialGraph& g = *ActiveGraphOpt();
        const std::vector<std::uint32_t> wired =
            pass == 0 ? m_data.baseInputs : m_data.passes[pass - 1].inputs;
        const std::uint32_t avail = static_cast<std::uint32_t>(wired.size());

        std::unordered_set<std::uint32_t> live;
        for (const Arcane::GraphNode& n : g.nodes)
        {
            if (Arcane::GraphNodeOutputCount(n) == 0)
                continue;   // Output / Vertex Output show no thumbnail
            live.insert(n.id);
            NodePreview& np = m_nodePreviews[n.id];

            Arcane::GraphCodegenResult r =
                Arcane::GenerateNodePreviewSnippet(g, n.id, avail);
            const std::uint64_t hash =
                r.Ok() ? std::hash<std::string>{}(r.snippet) | 1ull : 0ull;
            if (hash == np.snippetHash)
                continue;   // upstream unchanged -- keep whatever is bound
            np.snippetHash = hash;
            np.psJob = 0;   // orphan any in-flight compile of the older source
            np.psBytes.clear();
            np.pendingTempl.reset();
            if (!r.Ok())
                continue;   // no preview for this node (last-good keeps showing)

            Arcane::MaterialBuildResult build = Arcane::BuildMaterialShaderSource(
                *templateText, r.snippet, m_title + "_n" + std::to_string(n.id),
                Arcane::MaterialSurface::Fullscreen, {}, avail);
            if (!build.errors.empty())
                continue;
            np.pendingTempl =
                std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
            np.pendingInputs = avail;
            np.pendingSources = wired;

            Arcane::ShaderCompileRequest req;
            req.debugName = m_title + "_n" + std::to_string(n.id) + ".hlsl";
            req.sourceUtf8 = std::move(build.hlsl);
            req.entry = Arcane::kPsEntry;
            req.profile = Arcane::kPsProfile;
            req.coalesceKey = NodePreviewKey(m_data.id, pass, n.id);
            np.psJob = m_services.compiler->Submit(std::move(req), Now());
        }

        // Nodes deleted since the last refresh.
        for (auto it = m_nodePreviews.begin(); it != m_nodePreviews.end();)
        {
            if (!live.contains(it->first))
            {
                if (it->second.tex)
                    m_nodePreviewRetired.push_back(it->second.tex);
                it = m_nodePreviews.erase(it);
            }
            else
                ++it;
        }
    }

    void ShaderEditorDocument::BindNodePreview(NodePreview& np, nvrhi::ShaderHandle ps)
    {
        if (!np.pendingTempl || !m_nodePreviewVs || !ps)
            return;
        if (!np.pass)
            np.pass = Arcane::FullscreenMaterialPass::Create(m_services.device);
        if (!np.pass)
            return;
        if (!np.pass->SetMaterial(np.pendingTempl, m_nodePreviewVs, ps,
                                  np.pendingInputs))
            return;
        np.inst = std::make_shared<Arcane::MaterialInstance>(
            std::shared_ptr<const Arcane::MaterialTemplate>(np.pendingTempl));
        np.boundInputs = np.pendingInputs;
        np.boundSources = np.pendingSources;
        if (!np.tex)
        {
            constexpr std::uint32_t kThumbSize = 128;
            auto desc = nvrhi::TextureDesc()
                .setWidth(kThumbSize).setHeight(kThumbSize)
                .setFormat(nvrhi::Format::RGBA16_FLOAT)
                .setIsRenderTarget(true)
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setKeepInitialState(true)
                .setDebugName("NodePreview");
            np.tex = m_services.device->createTexture(desc);
            np.fb = np.tex ? m_services.device->createFramebuffer(
                                 nvrhi::FramebufferDesc().addColorAttachment(np.tex))
                           : nullptr;
        }
        np.ready = np.tex && np.fb;
    }

    void ShaderEditorDocument::RenderNodePreviews(double dt)
    {
        // Thumbnails record only while the canvas is the visible editing
        // surface; params sync from the doc instance first (redundant Sets
        // don't bump serials, so the steady state is hash lookups).
        // m_nodePreviewsInLod adds the zoom tier to that list: below
        // DefaultDetail the thumbnails are not drawn, so they are not rendered
        // either (see the member's comment for the one-frame lag).
        if (!m_showNodePreviews || !m_nodePreviewsInLod || !m_services.device ||
            IsInstance() || !ActiveGraphOwned() || m_showGeneratedText || m_editVertex)
            return;
        bool any = false;
        for (auto& [id, np] : m_nodePreviews)
            any = any || np.ready;
        if (!any)
            return;

        for (auto& [id, np] : m_nodePreviews)
        {
            if (!np.ready || !np.inst)
                continue;
            if (m_instance)
                for (const Arcane::ParamDecl& d : np.inst->Template().Params())
                {
                    Arcane::MatParamValue v;
                    if (m_instance->GetParam(d.nameHash, v))
                        np.inst->Set(d.nameHash, v);
                }
            // Texture params must be resident BEFORE the list opens (the same
            // upload-loss contract as the main preview).
            if (m_services.runtime)
                for (const Arcane::Guid& tg : np.inst->ResolveTextures())
                    if (tg.IsValid())
                        (void)m_services.runtime->AssetsFacade().GetTexture(
                            Arcane::AssetId::FromGuid(tg));
        }

        if (!m_nodePreviewCl)
            m_nodePreviewCl = m_services.device->createCommandList();
        if (!m_nodePreviewCl)
            return;

        Arcane::GlobalParams globals;
        globals.time = static_cast<float>(m_animTime);
        globals.deltaTime = static_cast<float>(dt);
        globals.viewportWidth = 128.0f;
        globals.viewportHeight = 128.0f;
        Arcane::Assets* assets =
            m_services.runtime ? &m_services.runtime->AssetsFacade() : nullptr;

        m_nodePreviewCl->open();
        for (auto& [id, np] : m_nodePreviews)
        {
            if (!np.ready || !np.inst)
                continue;
            // Pass-graph thumbnails read the chain's LIVE intermediates (one
            // frame stale -- invisible at thumbnail scale); missing entries
            // fall back to the pass's 1x1 black.
            std::vector<nvrhi::ITexture*> ins;
            ins.reserve(np.boundSources.size());
            for (std::uint32_t src : np.boundSources)
                ins.push_back(src == Arcane::kSceneInput
                                  ? SceneStandIn()
                                  : m_chain ? m_chain->PassOutput(src) : nullptr);
            np.pass->Render(m_nodePreviewCl, np.fb, *np.inst, globals, assets, ins);
        }
        m_nodePreviewCl->close();
        m_services.device->executeCommandList(m_nodePreviewCl);
    }

    nvrhi::ITexture* ShaderEditorDocument::SceneStandIn()
    {
        if (m_sceneStandIn || !m_services.device)
            return m_sceneStandIn.Get();
        constexpr std::uint32_t kSize = 64;
        auto desc = nvrhi::TextureDesc()
            .setWidth(kSize).setHeight(kSize)
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setDebugName("SceneStandIn");
        m_sceneStandIn = m_services.device->createTexture(desc);
        if (!m_sceneStandIn)
            return nullptr;
        std::vector<std::uint32_t> px(kSize * kSize);
        for (std::uint32_t y = 0; y < kSize; ++y)
            for (std::uint32_t x = 0; x < kSize; ++x)
                px[y * kSize + x] =
                    (((x >> 3) + (y >> 3)) & 1) ? 0xFF4A4A52u : 0xFF26262Cu;
        nvrhi::CommandListHandle cl = m_services.device->createCommandList();
        if (!cl)
        {
            m_sceneStandIn = nullptr;
            return nullptr;
        }
        cl->open();
        cl->writeTexture(m_sceneStandIn, 0, 0, px.data(),
                         kSize * sizeof(std::uint32_t));
        cl->close();
        m_services.device->executeCommandList(cl);
        return m_sceneStandIn.Get();
    }

    void ShaderEditorDocument::DrawNodePreviewImage(const Arcane::GraphNode& n, float width)
    {
        if (!m_showNodePreviews)
            return;
        // SG parity: the thumbnail is square and spans the node, sitting below
        // the port rows. `width` is last frame's measured content width -- a
        // node drawing for the first time has none and gets the floor, which is
        // also what keeps a narrow node from collapsing the thumbnail.
        constexpr float kThumbMin = 96.0f;
        const float kThumbDraw = width > kThumbMin ? width : kThumbMin;
        if (n.type == Arcane::GraphNodeType::Output)
        {
            // The Output node shows the material's own preview (the pass
            // canvas's base-node convention).
            if (m_preview && PreviewReady())
                ImGui::Image(static_cast<ImTextureID>(m_preview->TextureId()),
                             ImVec2(kThumbDraw, kThumbDraw));
            return;
        }
        const auto it = m_nodePreviews.find(n.id);
        if (it == m_nodePreviews.end() || !it->second.ready || !it->second.tex)
            return;
        ImGui::Image(static_cast<ImTextureID>(
                         reinterpret_cast<uintptr_t>(it->second.tex.Get())),
                     ImVec2(kThumbDraw, kThumbDraw));
    }

    void ShaderEditorDocument::DrawGraphPanel()
    {
        if (!ActiveGraphOwned())
            return;
        // One context per SHOWN graph: node ids are only unique per graph and
        // the context keeps per-id state (a Comment's group record re-types
        // the id), so a pass switch rebuilds the context wholesale -- which
        // also drops the selection and stale view for free.
        const bool switchedPass = m_graphShownPass != m_activePass;
        if (switchedPass && m_graphCtx)
        {
            ed::DestroyEditor(m_graphCtx);
            m_graphCtx = nullptr;
        }
        if (!m_graphCtx)
        {
            ed::Config cfg;
            cfg.SettingsFile = nullptr;   // layout persists in the .arcmat, not an ini
            ApplyZoomLevels(cfg);         // UE's 20 stops (see kZoomLevels)
            m_graphCtx = ed::CreateEditor(&cfg);
            // The style is per-context state, so a rebuilt context re-applies
            // it -- including the switch that kills the vendored grid.
            ed::SetCurrentEditor(m_graphCtx);
            ApplyGraphCanvasStyle();
            ed::SetCurrentEditor(nullptr);
        }
        if (switchedPass)
        {
            m_graphShownPass = m_activePass;
            m_graphPositionsApplied = false;
            m_nodeWidths.clear();   // ids are only unique per graph
            RebuildDiagBadges();
            RefreshNodePreviews();   // thumbnails belong to the shown graph
        }
        Arcane::MaterialGraph& g = *ActiveGraphOpt();

        ed::SetCurrentEditor(m_graphCtx);
        // ---- Shader-rendered backdrop, UNDER the canvas content ----
        // The node editor offers no public way to draw beneath its own
        // background/grid layer: everything it emits lands in channels the API
        // does not expose, and the two it does expose (the per-node background
        // draw list, the group-hint lists) sit ABOVE links. So the backdrop is
        // blitted before ed::Begin, which puts it in the window draw list ahead
        // of every channel the editor merges in afterwards.
        //
        // DISCLOSED CONSEQUENCE: the transform read here is the one ed::Begin
        // installed LAST frame. The editor computes the new view in End()
        // (imgui_node_editor.cpp:1357) and installs it in the next Begin()
        // (:1257), so a frame that is actively panning or zooming draws the
        // backdrop one frame behind the nodes.
        //
        // What that costs is CONTINUITY, not correctness -- and continuity is
        // the property that matters now that the grid's phase is STATE
        // (GraphGridPass::UpdatePhase). The pass is fed the same sequence of
        // views, just one frame late, so it accumulates the same phase; no
        // error builds up over a gesture, and the final view of a gesture does
        // arrive on the following frame, so the grid settles onto its exact
        // position without a jump. Only the moving frames are offset.
        //
        // Reading it after ed::Begin would remove even that, but there is no
        // channel under the content to put the blit in; the fix, if the lag
        // ever reads badly, is to host the canvas in a child window and blit
        // into the PARENT's draw list after ed::End (parent draw lists render
        // first).
        const ImVec2 canvasMin  = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (!m_grid && m_services.device && m_services.shaders)
            m_grid = GraphGridPass::Create(m_services.device, m_services.shaders);
        if (m_grid && canvasSize.x > 0.0f && canvasSize.y > 0.0f)
        {
            GraphGridView view;
            view.width  = static_cast<std::uint32_t>(canvasSize.x);
            view.height = static_cast<std::uint32_t>(canvasSize.y);
            // RAW view state only -- the grid derives its own phase from the
            // history of these (GraphGridPass::UpdatePhase), because a
            // sublinearly-scaled lattice has no canvas-space anchor to be read
            // off any single frame.
            //
            // ViewScale() owns the GetCurrentZoom-returns-the-reciprocal flip
            // (see its comment). ScreenToCanvas is safe HERE and only here:
            // inside ed::Begin/End the editor moves ImGui itself into canvas
            // space (imgui_canvas.cpp:476-487), so this must stay ahead of it.
            view.scale = ViewScale();
            const ImVec2 originCanvas = ed::ScreenToCanvas(canvasMin);
            view.originX = originCanvas.x;
            view.originY = originCanvas.y;

            GraphGridColors colors;
            FillRgba(colors.canvas, kCanvasColor);
            FillRgba(colors.minor,  kGridMinorColor);
            FillRgba(colors.major,  kGridMajorColor);

            if (nvrhi::ITexture* tex = m_grid->Update(view, colors))
            {
                // The target is over-allocated to a quantum, so only its
                // top-left corner belongs to this region -- hence the UVs.
                const float u = canvasSize.x / static_cast<float>(m_grid->AllocatedWidth());
                const float v = canvasSize.y / static_cast<float>(m_grid->AllocatedHeight());
                ImGui::GetWindowDrawList()->AddImage(
                    static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex)),
                    canvasMin, ImVec2(canvasMin.x + canvasSize.x,
                                      canvasMin.y + canvasSize.y),
                    ImVec2(0.0f, 0.0f), ImVec2(u, v));
            }
        }
        // Nothing is drawn above the canvas inside this function, so the
        // remaining region IS the canvas's height.
        ed::Begin("##graphcanvas", ImVec2(0.0f, ImGui::GetContentRegionAvail().y));
        if (switchedPass)
            ed::ClearSelection();

        const bool seededThisFrame = !m_graphPositionsApplied;
        if (seededThisFrame)
        {
            for (const Arcane::GraphNode& n : g.nodes)
            {
                ed::SetNodePosition(n.id, ImVec2(n.posX, n.posY));
                if (n.type == Arcane::GraphNodeType::Comment)
                    ed::SetGroupSize(n.id, ImVec2((std::max)(80.0f, n.value[0]),
                                                  (std::max)(60.0f, n.value[1])));
            }
            m_graphPositionsApplied = true;
        }

        // ---- Rendering LOD: ONE read of the zoom, ONE tier, per frame ----
        // Read inside Begin/End on purpose: this is the view the editor
        // installed for THIS frame's submission (imgui_node_editor.cpp:1258),
        // so the tier and the geometry it degrades agree exactly. The grid's
        // read above is the same number -- the navigate action only re-derives
        // the view during End -- but it is taken before Begin because
        // ScreenToCanvas has to be, so the two calls stay separate.
        const NodeLOD lod = NodeLODForScale(ViewScale());
        // Thumbnails are the one degradation with a GPU cost behind it, so the
        // tier also reaches back into Tick's render loop (see m_nodePreviewsInLod).
        m_nodePreviewsInLod = lod >= NodeLOD::DefaultDetail;

        // Wire anchors are per-frame: nodes move, the view moves, and a pin
        // that stops being submitted must stop having an anchor (see
        // DrawGradientWire's miss path). Cleared here, refilled by the pin
        // rows below, consumed by the link loop after them.
        m_pinPivots.clear();

        for (Arcane::GraphNode& n : g.nodes)
            DrawGraphNode(n, lod);

        // Links: ids are the vector index + 1, stable within this frame (the
        // deletion pass collects indices and erases after the queries).
        for (std::size_t i = 0; i < g.links.size(); ++i)
        {
            const Arcane::GraphLink& l = g.links[i];
            // A wire now carries BOTH its endpoints' types: source colour at
            // the tail, destination colour at the head. That makes an adapting
            // connection (float -> float4, or anything into a dynamic pin)
            // legible as a transition rather than as a wire that lies about one
            // of its ends. A dangling endpoint (should not survive an edit, but
            // the draw must not depend on that) falls back to the neutral
            // dynamic colour.
            const Arcane::GraphNode* src = g.FindNode(l.fromNode);
            const bool srcPinValid =
                src && l.fromPin < Arcane::GraphNodeOutputCount(*src);
            const ImVec4 srcTint =
                srcPinValid ? PinColorForWidth(
                                  Arcane::GraphNodeOutputPin(*src, l.fromPin).width)
                            : kPinDynamicColor;

            const Arcane::GraphNode* dst = g.FindNode(l.toNode);
            const bool dstPinValid =
                dst && l.toPin < Arcane::GraphNodeInputCount(*dst);
            const ImVec4 dstTint =
                dstPinValid ? PinColorForWidth(
                                  Arcane::GraphNodeInputPin(*dst, l.toPin).width)
                            : kPinDynamicColor;

            const ed::LinkId linkId(i + 1);
            const ed::PinId fromPin = OutPin(l.fromNode, l.fromPin);
            const ed::PinId toPin   = InPin(l.toNode, l.toPin);

            // Interaction only -- transparent, so the library tessellates
            // nothing (see kLinkChannelLinks). The thickness is the real one:
            // it is still the hit radius.
            ed::Link(linkId, fromPin, toPin, ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
                     kWireThickness);

            // GetHoveredLink reports 0 while any action is running (the
            // m_CurrentAction guard, imgui_node_editor.cpp:1280), so a wire
            // does not flicker bright while it is being dragged past.
            const bool emphasize = ed::IsLinkSelected(linkId) ||
                                   ed::GetHoveredLink() == linkId;
            DrawGradientWire(fromPin.Get(), toPin.Get(), srcTint, dstTint,
                             emphasize);
        }

        HandleGraphEdits();

        // Requested focus: select + frame the offending node. Nothing writes
        // m_focusNode today -- the errors panel's rows did, and console lines
        // are not clickable; the consumer stays for the Problems panel.
        if (m_focusNode != 0)
        {
            ed::SelectNode(ed::NodeId(m_focusNode));
            ed::NavigateToSelection(true);
            m_focusNode = 0;
        }

        // F = frame (the UE/SG muscle memory): zoom to the selection, or to
        // everything when nothing is selected.
        if (ImGui::IsWindowHovered() && !ImGui::GetIO().WantTextInput &&
            ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            if (ed::GetSelectedObjectCount() > 0)
                ed::NavigateToSelection(true);
            else
                ed::NavigateToContent();
        }

        // Node context menu -> alignment over the current selection.
        ed::Suspend();
        {
            ed::NodeId ctxNode;
            if (ed::ShowNodeContextMenu(&ctxNode))
                ImGui::OpenPopup("##graphnodemenu");
            if (ImGui::BeginPopup("##graphnodemenu"))
            {
                std::vector<ed::NodeId> sel(
                    static_cast<std::size_t>(std::max(0, ed::GetSelectedObjectCount())));
                const int count = sel.empty() ? 0
                    : ed::GetSelectedNodes(sel.data(), static_cast<int>(sel.size()));
                std::vector<Arcane::GraphNode*> picked;
                for (int i = 0; i < count; ++i)
                    if (Arcane::GraphNode* node = g.FindNode(static_cast<std::uint32_t>(
                            sel[static_cast<std::size_t>(i)].Get())))
                        picked.push_back(node);
                const bool can = picked.size() >= 2;

                // One undo step per alignment; positions write BOTH the canvas
                // and the data, so the later readback sees no delta.
                auto align = [&](const char* label, auto&& place)
                {
                    if (!ImGui::MenuItem(label, nullptr, false, can))
                        return;
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    for (Arcane::GraphNode* node : picked)
                    {
                        const ImVec2 size = ed::GetNodeSize(ed::NodeId(node->id));
                        ImVec2 p(node->posX, node->posY);
                        place(p, size);
                        node->posX = p.x;
                        node->posY = p.y;
                        ed::SetNodePosition(ed::NodeId(node->id), p);
                    }
                    m_dirty = true;
                    PushGraphUndo(label, std::move(before));
                };

                float minX = FLT_MAX, minY = FLT_MAX, maxR = -FLT_MAX, maxB = -FLT_MAX;
                float sumCX = 0.0f, sumCY = 0.0f;
                for (const Arcane::GraphNode* node : picked)
                {
                    const ImVec2 size = ed::GetNodeSize(ed::NodeId(node->id));
                    minX = (std::min)(minX, node->posX);
                    minY = (std::min)(minY, node->posY);
                    maxR = (std::max)(maxR, node->posX + size.x);
                    maxB = (std::max)(maxB, node->posY + size.y);
                    sumCX += node->posX + size.x * 0.5f;
                    sumCY += node->posY + size.y * 0.5f;
                }
                const float n = picked.empty() ? 1.0f : static_cast<float>(picked.size());
                const float avgCX = sumCX / n, avgCY = sumCY / n;

                align("Align Left",   [&](ImVec2& p, const ImVec2&)  { p.x = minX; });
                align("Align Right",  [&](ImVec2& p, const ImVec2& s){ p.x = maxR - s.x; });
                align("Align Top",    [&](ImVec2& p, const ImVec2&)  { p.y = minY; });
                align("Align Bottom", [&](ImVec2& p, const ImVec2& s){ p.y = maxB - s.y; });
                align("Center Column",[&](ImVec2& p, const ImVec2& s){ p.x = avgCX - s.x * 0.5f; });
                align("Center Row",   [&](ImVec2& p, const ImVec2& s){ p.y = avgCY - s.y * 0.5f; });
                if (!can)
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("select 2+ nodes to align");
                }
                ImGui::EndPopup();
            }
        }
        ed::Resume();

        // Background context menu -> create node (Suspend: popups live in
        // normal ImGui space, not canvas space).
        ed::Suspend();
        if (ed::ShowBackgroundContextMenu())
        {
            m_wireActive = false;   // plain create -- no wire to connect
            const ImVec2 p = ImGui::GetMousePos();
            m_graphPopupX = p.x;
            m_graphPopupY = p.y;
            ImGui::OpenPopup("##graphcreate");
        }
        if (m_wireCreateRequest)
        {
            m_wireCreateRequest = false;
            m_wireActive = true;
            const ImVec2 p = ImGui::GetMousePos();
            m_graphPopupX = p.x;
            m_graphPopupY = p.y;
            ImGui::OpenPopup("##graphcreate");
        }

        // Custom-node body editor: a MODAL in suspended (screen) space -- the
        // in-node widget can only be a preview (child windows drift under the
        // canvas transform). Apply commits ONE undo step.
        if (m_bodyEditRequest != 0)
        {
            if (const Arcane::GraphNode* n = g.FindNode(m_bodyEditRequest))
            {
                m_bodyEditNode = m_bodyEditRequest;
                std::snprintf(m_bodyBuf, sizeof(m_bodyBuf), "%s", n->customBody.c_str());
                ImGui::OpenPopup("Edit HLSL##graphbody");
            }
            m_bodyEditRequest = 0;
        }
        ImGui::SetNextWindowSize(ImVec2(560.0f, 380.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Edit HLSL##graphbody", nullptr))
        {
            ImGui::TextDisabled("Function body. Inputs arrive as the node's pins; params "
                                "and Time are directly visible. End with a return.");
            ImGui::InputTextMultiline("##bodyedit", m_bodyBuf, sizeof(m_bodyBuf),
                                      ImVec2(-1.0f, ImGui::GetContentRegionAvail().y - 34.0f),
                                      ImGuiInputTextFlags_AllowTabInput);
            if (ImGui::Button("Apply"))
            {
                if (Arcane::GraphNode* n = g.FindNode(m_bodyEditNode);
                    n && n->customBody != m_bodyBuf)
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    n->customBody = m_bodyBuf;
                    m_dirty = true;
                    if (m_live)
                        RegenerateFromGraph();
                    PushGraphUndo("Edit HLSL Body", std::move(before));
                }
                m_bodyEditNode = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                m_bodyEditNode = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        // Assisted param rename: the consent modal (cross-FILE writes are not
        // undoable -- this gate is their structural-edit standing).
        if (m_renameRequest)
        {
            m_renameRequest = false;
            ImGui::OpenPopup("Rename Param Everywhere?##prename");
        }
        if (ImGui::BeginPopupModal("Rename Param Everywhere?##prename", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Renamed '%s' -> '%s'.", m_renameOld.c_str(), m_renameNew.c_str());
            ImGui::Text("%zu instance file(s) carry a saved value under the old name:",
                        m_renameTargets.size());
            for (std::size_t i = 0; i < m_renameTargets.size() && i < 8; ++i)
                ImGui::BulletText("%s", m_renameTargets[i].name.c_str());
            if (m_renameTargets.size() > 8)
                ImGui::TextDisabled("...and %zu more", m_renameTargets.size() - 8);
            ImGui::TextDisabled("Files that already have a '%s' value keep it; the "
                                "old entry drops.", m_renameNew.c_str());
            ImGui::Separator();
            if (ImGui::Button("Rename everywhere"))
            {
                for (const RenameTarget& t : m_renameTargets)
                {
                    auto data = Arcane::LoadMaterialAsset(t.path);
                    if (!data)
                    {
                        ARC_ERROR("param rename: '{}' failed to load -- skipped",
                                  t.path.generic_string());
                        continue;
                    }
                    RekeySavedParam(data->params, m_renameOld, m_renameNew);
                    if (!Arcane::SaveMaterialAsset(t.path, *data))
                    {
                        ARC_ERROR("param rename: '{}' failed to save -- skipped",
                                  t.path.generic_string());
                        continue;
                    }
                    if (m_services.onAssetSaved)
                        m_services.onAssetSaved(t.id);   // sprite-cache invalidate
                    if (m_services.onParamRenamed)
                        m_services.onParamRenamed(t.id, m_renameOld, m_renameNew);
                }
                m_renameTargets.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Just here"))
            {
                m_renameTargets.clear();   // today's behavior: the wart, chosen
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("##graphcreate"))
        {
            // The searcher: type-to-filter, Enter creates the first match.
            if (ImGui::IsWindowAppearing())
            {
                m_createSearch[0] = '\0';
                ImGui::SetKeyboardFocusHere();
            }
            const bool enter = ImGui::InputTextWithHint(
                "##nodesearch", "Search...", m_createSearch, sizeof(m_createSearch),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Separator();

            const Arcane::GraphNodeTypeInfo* chosen = nullptr;
            const Arcane::GraphNodeTypeInfo* first = nullptr;
            // CONTEXT eligibility (surface, pass context, wire side) -- asked
            // in one place so the flat and the categorised pass below cannot
            // drift apart. The search filter is deliberately NOT in here: the
            // two passes differ precisely in whether they apply it.
            auto eligible = [&](const Arcane::GraphNodeTypeInfo& info)
            {
                if (info.type == Arcane::GraphNodeType::Output)
                    return false;   // exactly one, seeded at creation, undeletable
                const bool spriteOnly = info.type == Arcane::GraphNodeType::VertexColor ||
                                        info.type == Arcane::GraphNodeType::SpriteTexture;
                if (spriteOnly && m_surface != 1)
                    return false;
                // Pass Input samples wired input slots -- available wherever
                // the active pass has any (the base's are its scene wires).
                if (info.type == Arcane::GraphNodeType::PassInput &&
                    m_activePass == 0 && m_data.baseInputs.empty())
                    return false;
                // The vertex context lives on the BASE graph, at most once.
                if (info.type == Arcane::GraphNodeType::VertexOutput &&
                    (m_activePass != 0 ||
                     [&g] {
                         for (const Arcane::GraphNode& n : g.nodes)
                             if (n.type == Arcane::GraphNodeType::VertexOutput)
                                 return true;
                         return false;
                     }()))
                    return false;
                // Wire-invoked: only types with a pin on the wire's far side.
                // Every pin is numeric (adaptation absorbs widths), so
                // compatibility is purely structural. A fresh Custom node has
                // no inputs, so it only appears for input-side drags.
                if (m_wireActive)
                {
                    const bool hasFarPin = m_wireIsInput ? !info.outputs.empty()
                                                         : !info.inputs.empty();
                    if (!hasFarPin)
                        return false;
                }
                return true;
            };
            auto emit = [&](const Arcane::GraphNodeTypeInfo& info)
            {
                if (!first)
                    first = &info;   // Enter creates this one
                if (ImGui::MenuItem(info.display))
                    chosen = &info;
            };
            // A SEARCH FLATTENS the list: with a filter typed, headings would
            // strand one or two items under each and push the first match --
            // what Enter creates -- further down. Unfiltered, ~49 types need
            // the structure, so they group by GraphNodeCategory in ENUM order
            // (Uncategorized, Input, Math, Vector, Procedural, Output,
            // Utility). Empty categories draw no heading, so a sprite-only or
            // wire-invoked popup never shows a bare title -- and Uncategorized
            // is empty for any healthy table, which is why the menu looks the
            // same as it did before that enumerator existed. A row that ever
            // lands there gets a heading saying so, rather than hiding among
            // the Input nodes.
            if (m_createSearch[0] != '\0')
            {
                for (const Arcane::GraphNodeTypeInfo& info : Arcane::AllGraphNodeInfos())
                    if (eligible(info) && ContainsInsensitive(info.display, m_createSearch))
                        emit(info);
            }
            else
            {
                // The bound comes from the TABLE, not from the last
                // enumerator: a category appended after Utility then gets its
                // own heading here instead of silently hiding its nodes.
                int maxCat = 0;
                for (const Arcane::GraphNodeTypeInfo& info : Arcane::AllGraphNodeInfos())
                    maxCat = std::max(maxCat, static_cast<int>(info.category));
                for (int c = 0; c <= maxCat; ++c)
                {
                    const auto cat = static_cast<Arcane::GraphNodeCategory>(c);
                    bool headed = false;
                    for (const Arcane::GraphNodeTypeInfo& info : Arcane::AllGraphNodeInfos())
                    {
                        if (info.category != cat || !eligible(info))
                            continue;
                        if (!headed)
                        {
                            ImGui::SeparatorText(Arcane::GraphNodeCategoryName(cat));
                            headed = true;
                        }
                        emit(info);
                    }
                }
            }
            if (enter && first)
            {
                chosen = first;
                ImGui::CloseCurrentPopup();
            }
            if (chosen)
            {
                const Arcane::GraphNodeTypeInfo& info = *chosen;
                std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                Arcane::GraphNode n;
                n.id = g.MintId();
                n.type = info.type;
                if (info.type == Arcane::GraphNodeType::ConstColor ||
                    info.type == Arcane::GraphNodeType::ConstFloat4)
                {
                    n.value[0] = n.value[1] = n.value[2] = n.value[3] = 1.0f;
                }
                if (info.type == Arcane::GraphNodeType::Param ||
                    info.type == Arcane::GraphNodeType::TextureSample)
                {
                    // Unique default name ("Param3"/"Tex3") -- dup names are a
                    // shared decl, which a fresh node should not silently join.
                    const char* stem =
                        info.type == Arcane::GraphNodeType::Param ? "Param" : "Tex";
                    std::string name;
                    for (std::uint32_t k = n.id;; ++k)
                    {
                        name = stem + std::to_string(k);
                        bool taken = false;
                        for (const Arcane::GraphNode& other : g.nodes)
                            taken = taken || other.paramName == name;
                        if (!taken)
                            break;
                    }
                    n.paramName = name;
                    n.paramType = info.type == Arcane::GraphNodeType::Param
                                      ? Arcane::MatParamType::Float
                                      : Arcane::MatParamType::Texture;
                }
                if (info.type == Arcane::GraphNodeType::Custom)
                {
                    // Magenta until written -- the classic "custom shader
                    // pending" placeholder. Bodies can read params/Time
                    // directly (they land after the cbuffer declarations).
                    n.customBody = "return float4(1.0, 0.0, 1.0, 1.0);";
                    n.customOutWidth = 4;
                }
                if (info.type == Arcane::GraphNodeType::Comment)
                {
                    n.paramName = "Comment";
                    n.value[0] = 280.0f;   // starter box size
                    n.value[1] = 160.0f;
                    ed::SetGroupSize(n.id, ImVec2(n.value[0], n.value[1]));
                }
                const ImVec2 canvasPos =
                    ed::ScreenToCanvas(ImVec2(m_graphPopupX, m_graphPopupY));
                n.posX = canvasPos.x;
                n.posY = canvasPos.y;
                ed::SetNodePosition(n.id, canvasPos);
                const std::uint32_t newId = n.id;
                g.nodes.push_back(std::move(n));
                // Wire-invoked: auto-connect the first far-side pin. Input
                // drags replace silently, exactly like a hand-drawn wire; a
                // fresh node's single edge can never cycle.
                if (m_wireActive && g.FindNode(m_wireNode))
                {
                    Arcane::GraphLink l;
                    if (m_wireIsInput)
                    {
                        std::erase_if(g.links, [&](const Arcane::GraphLink& x)
                                      { return x.toNode == m_wireNode &&
                                               x.toPin == m_wirePin; });
                        l.fromNode = newId;
                        l.fromPin = 0;
                        l.toNode = m_wireNode;
                        l.toPin = m_wirePin;
                    }
                    else
                    {
                        l.fromNode = m_wireNode;
                        l.fromPin = m_wirePin;
                        l.toNode = newId;
                        l.toPin = 0;
                    }
                    g.links.push_back(l);
                }
                m_dirty = true;
                if (m_live)
                    RegenerateFromGraph();
                PushGraphUndo("Add Node", std::move(before));
            }
            ImGui::EndPopup();
        }
        else
            m_wireActive = false;   // popup closed without a pick
        ed::Resume();

        // Position readback (skipped the seeding frame -- the canvas would
        // report pre-seed positions). Pure moves dirty the asset, no recompile.
        if (!seededThisFrame)
        {
            for (Arcane::GraphNode& n : g.nodes)
            {
                const ImVec2 p = ed::GetNodePosition(ed::NodeId(n.id));
                if (p.x != n.posX || p.y != n.posY)
                {
                    n.posX = p.x;
                    n.posY = p.y;
                    m_dirty = true;
                }
            }
        }

        ed::End();
        ed::SetCurrentEditor(nullptr);
    }

    void ShaderEditorDocument::DrawGraphNode(Arcane::GraphNode& n, NodeLOD canvasLod)
    {
        if (n.type == Arcane::GraphNodeType::Comment)
        {
            // Comment/group box (UE comment, SG group): the editor's NATIVE
            // group node, so dragging the box carries contained nodes. The
            // size persists exactly: ed::Group records its bounds from the
            // Dummy it draws, so GetItemRectSize right after IS the live
            // (possibly user-resized) box.
            //
            // EXEMPT FROM THE LOD, and that is UE's rule, not a shortcut: a
            // comment's own bubble uses InvertLODCulling
            // (SGraphNodeComment.cpp:238), so SCommentBubble::IsBubbleVisible
            // shows it exactly when `CurrLOD <= MediumDetail`
            // (SCommentBubble.cpp:387-396) -- comment text is the thing UE
            // turns ON as you zoom out, because it is what you are navigating
            // BY. Degrading it here would delete the map.
            ed::BeginNode(ed::NodeId(n.id));
            ImGui::PushID(static_cast<int>(n.id));
            StableTextEdit("##ctitle", m_textEdit, TextKey(TextEditKind::Comment, n.id),
                           n.paramName, (std::max)(120.0f, n.value[0] - 16.0f),
                           [&](const char* text)
                           {
                               std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                               n.paramName = text;
                               m_dirty = true;   // annotation only -- no recompile
                               PushGraphUndo("Edit Comment", std::move(before));
                           });
            ed::Group(ImVec2((std::max)(80.0f, n.value[0]),
                             (std::max)(60.0f, n.value[1])));
            const ImVec2 gs = ImGui::GetItemRectSize();
            if (std::abs(gs.x - n.value[0]) > 0.5f || std::abs(gs.y - n.value[1]) > 0.5f)
            {
                n.value[0] = gs.x;
                n.value[1] = gs.y;
                m_dirty = true;   // resize has move standing (dirty, no undo step)
            }
            ImGui::PopID();
            ed::EndNode();
            return;
        }

        const Arcane::GraphNodeTypeInfo& info = Arcane::GraphNodeInfo(n.type);

        // ---- Effective tier for THIS node ----
        // UE's rename guard, ported onto the same risk it guards: a node whose
        // title is in edit mode refuses the low-detail swap
        // (SGraphNode.cpp:1596-1607 -- the `&& !InlineEditableText->IsInEditMode()`
        // term). It matters MORE here than it does in UE. StableTextEdit parks
        // the typed text in m_textEdit and only commits on a
        // deactivate-after-edit it can SEE (EditorWidgets.cpp:479-488); a field
        // that stops being submitted mid-edit never reports one, so the typed
        // text would be silently dropped when the node came back. Wheel-zoom
        // while a name field has focus is exactly that gesture.
        //
        // The KIND is checked, not just the id: m_textEdit is one buffer shared
        // with the pass canvas (ShaderEditorDocument.hpp:549-559), and
        // a PassName key carries a chain INDEX that can collide with a node id.
        //
        // Value drags need no guard -- an abandoned gesture is closed by
        // EditGesture's ScopeGuard, which pushes whatever the drag did
        // (EditGesture.cpp:90-97, armed at :1347 above).
        NodeLOD lod = canvasLod;
        if (lod < NodeLOD::DefaultDetail)
        {
            const std::uint64_t kindBits = m_textEdit.activeKey >> 56;
            const std::uint64_t idBits   = m_textEdit.activeKey & ((1ull << 56) - 1);
            if ((kindBits == static_cast<std::uint64_t>(TextEditKind::NodeName) ||
                 kindBits == static_cast<std::uint64_t>(TextEditKind::Swizzle)) &&
                idBits == n.id)
                lod = NodeLOD::DefaultDetail;
        }

        // The three degradation switches, in UE's own comparison form (the enum
        // ascends with detail, so a degradation is `lod <= Tier`). UE has only
        // two real thresholds across its whole graph editor -- `<= LowestDetail`
        // for structural widget swaps and `<= LowDetail` for text and small
        // controls -- and these are those two, plus one for the thumbnail:
        //
        //   showPinRows  (> LowestDetail)  pin rows exist at all. UE's harshest
        //       swap replaces a node's ENTIRE content area with a spacer at
        //       `<= LowestDetail` (SAnimationGraphNode.cpp:244-272); this is
        //       that, and the node reads as a labelled colored block.
        //   showPinText  (> LowDetail)     pin labels, inline pin literals and
        //       the per-type payload widgets. UE drops exactly this class at
        //       `<= LowDetail`: pin label + default-value widget, keeping the
        //       pin ICON (SGraphPin.cpp:354-364, :1463-1479); the "+ Add pin"
        //       button (SGraphNode.cpp:1681-1692); description text and badges
        //       (SGraphNodeAI.cpp:103-108).
        //   showPreview  (>= DefaultDetail) the live thumbnail.
        //
        // DISCLOSED DIVERGENCE on that last one. UE's material node does NOT
        // LOD out its 96x96 live preview -- SGraphNodeMaterialBase.cpp:695-700
        // gates it on user preference only and the file contains no LOD
        // reference at all, so Epic renders a realtime material RT per node at
        // 0.10x zoom. Every OTHER live preview in the engine is gated: the
        // closest analog, SBlendSpacePreview, is swapped for a fixed-size
        // spacer at `<= LowestDetail` (SGraphNodeBlendSpacePlayer.cpp:61-72).
        // We take the gate one tier further out, to `<= MediumDetail`, because
        // the cost is ours in a way it is not Epic's: each thumbnail is an
        // offscreen material pass per node per FRAME (RenderNodePreviews), the
        // only per-node cost on this canvas that scales with how many nodes are
        // on screen -- and at 0.675x a 96 px thumbnail is already under 65 px.
        // MediumDetail's own charter allows it: SNodePanel.h:70-90 calls it the
        // tier where content "starts to get hard to read".
        //
        // The thumbnail is also the one thing here that gets NO same-size
        // placeholder, where UE always substitutes one. UE needs it because a
        // Slate graph LAYS OUT its nodes and a size change reflows neighbours;
        // ours are absolutely positioned from posX/posY, so a shorter node
        // moves nothing but itself -- and reclaiming the 96 px is the entire
        // point of hiding it. The block at LowestDetail is the case where size
        // preservation still buys something (see there), and it keeps its
        // width.
        const bool showPinRows = lod > NodeLOD::LowestDetail;
        const bool showPinText = lod > NodeLOD::LowDetail;
        const bool showPreview = lod >= NodeLOD::DefaultDetail;

        // The node's measured width from the LAST frame (see m_nodeWidths):
        // output rows right-align to it and the preview spans it. Zero on a
        // node's first frame, which both consumers treat as "no alignment".
        const auto widthIt = m_nodeWidths.find(n.id);
        const float contentW = widthIt == m_nodeWidths.end()
                                   ? 0.0f
                                   : widthIt->second - 2.0f * kNodePadX;

        ed::BeginNode(ed::NodeId(n.id));
        ImGui::PushID(static_cast<int>(n.id));

        // Title row. The BAND behind it is a rectangle drawn after ed::EndNode
        // (it spans the node's final width, which does not exist yet); this is
        // only the text, and headerMaxY is the band's bottom edge.
        //
        // NOT LOD-GATED, at either end. The title is the last thing a block
        // has left to be identified by, and the "(!)" prefix is the ERROR
        // BADGE -- the thing you zoom out to FIND. UE never LODs its error
        // reporting out either: SetupErrorReporting's widgets go into the node
        // unconditionally (SGraphNode.cpp:1001-1013), with no
        // SLevelOfDetailBranchNode around them and no LOD term in their
        // visibility, and the same holds for the panel's overlay badges
        // (SGraphPanel.cpp:414-466). (UE does swap the title itself for a flat
        // colored border at <= LowestDetail, SGraphNode.cpp:941-953 -- but its
        // low-detail node still has a body and pin icons to be read by. Ours
        // collapses to the band, so the band has to carry the identity.)
        if (NodeBadged(n.id))
            ImGui::TextColored(kNodeBadgeText, "(!) %s", info.display);
        else
            ImGui::TextColored(kNodeTitleText, "%s", info.display);
        const float headerMaxY = ImGui::GetItemRectMax().y;

        // Reserve the gap under the band (kNodeHeaderGap). Solved rather than
        // guessed, because ImGui's automatic spacing is already in play at both
        // ends of the dummy: the next real item lands at
        // headerMaxY + 2*ItemSpacing.y + fill, and it needs to land at the
        // band's bottom edge (headerMaxY + kNodePadY) plus the gap.
        //
        // Clamped at zero: a theme with generous ItemSpacing may already place
        // the row far enough down, and a negative dummy would be nonsense.
        //
        // GATED ON showPinRows, i.e. skipped at LowestDetail. There the band IS
        // the node -- the pin rows are gone and the node's whole height is the
        // header -- so reserving body space below it would open an empty strip
        // under the bar with nothing to put in it, and would inflate the block's
        // height for no reading. The band-only tier wants no gap at all.
        if (showPinRows)
        {
            const float fill = (kNodePadY + kNodeHeaderGap) -
                               2.0f * ImGui::GetStyle().ItemSpacing.y;
            if (fill > 0.0f)
                ImGui::Dummy(ImVec2(0.0f, fill));
        }

        // Gesture helpers (used by pin rows AND payload widgets below). Value
        // drags bracket a whole-graph gesture through EditGesture (before on
        // activation, one undo step at close); popup-widgets (combos, color
        // pickers) cannot live inside the canvas, so types use cycle buttons.
        // The label rides the OPEN call because the transaction carries it --
        // CommandStack::Commit stamps the step with Begin's label, not the
        // pushed command's.
        auto gestureBegin = [&](const char* label)
        {
            EditGesture::BeginOnActivate(m_services.undo, m_gesture,
                [&] { return std::string(label); },
                [&]
                {
                    // Whole-graph before AND the pass it belongs to, both
                    // pinned at activation. The command builds at CLOSE from
                    // this plus whatever the drag did -- which is why an
                    // abandoned drag now lands on the stack instead of
                    // vanishing. Pinning the PASS is what keeps that safe: a
                    // close can land after the active pass moved (a ctrl+click
                    // text entry parks a gesture without deactivating it, the
                    // pass canvas is submitted before this panel, and the
                    // abandoned close runs later still at the ScopeGuard), and
                    // a command pairing pass B's index with pass A's `before`
                    // would have Undo overwrite B's graph with A's.
                    //
                    // NO-OP GUARD: the close runs on EVERY close path, including
                    // the abandonment ones (stale-close, collapsed window,
                    // document teardown) where the gesture never edited
                    // anything. Pushing there would leave a junk step whose
                    // before == after AND clear the redo stack
                    // (CommandStack.cpp:70) -- a generic Push is its own
                    // transaction, so it never meets Commit's empty-transaction
                    // drop at :61-62. So compare first; an EDITED gesture still
                    // differs and still pushes exactly one step.
                    return std::function<void()>(
                        [this, label = std::string(label),
                         pass = static_cast<std::size_t>((std::max)(0, m_activePass)),
                         before = ActiveGraphOpt()]() mutable
                        {
                            if (GraphOptEqual(before, GraphOptAt(pass)))
                                return;   // nothing changed -- no step, redo intact
                            PushGraphUndo(label.c_str(), std::move(before), pass);
                        });
                });
        };
        auto gestureEnd = [&] { EditGesture::EndOnDeactivate(m_services.undo, m_gesture); };
        auto valueEdited = [&]
        {
            m_dirty = true;
            if (m_live)
                RegenerateFromGraph();
        };

        // An inline literal is hidden while a wire feeds the pin. One edge per
        // input is a canvas invariant (HandleGraphEdits replaces silently), so
        // a single scan answers it.
        const Arcane::MaterialGraph& graph = *ActiveGraphOpt();
        auto pinWired = [&](std::uint32_t pin)
        {
            for (const Arcane::GraphLink& l : graph.links)
                if (l.toNode == n.id && l.toPin == pin)
                    return true;
            return false;
        };
        // Output side of the same question: an output FANS OUT, so any edge
        // leaving it counts.
        auto pinFanout = [&](std::uint32_t pin)
        {
            for (const Arcane::GraphLink& l : graph.links)
                if (l.fromNode == n.id && l.fromPin == pin)
                    return true;
            return false;
        };

        // ================= LowestDetail: the node as a block =================
        // THE CONSTRAINT THAT SHAPES THIS: every pin must still be SUBMITTED,
        // or the wires touching it disappear. ed::Link is refused outright
        // unless BOTH endpoints are live this frame -- DoLink returns false at
        // imgui_node_editor.cpp:1639-1640 -- and m_IsLive is set only by
        // BeginPin (:5366). So dropping the pin rows cannot mean dropping the
        // pins. UE has the same rule for free (its wires are drawn by the panel
        // from the graph's own connectivity, with no LOD gate anywhere in
        // SGraphPanel's connection block or ConnectionDrawingPolicy) and its
        // nodes keep drawing pin ICONS even at LowestDetail; we get there by
        // submitting each pin as a zero-size anchor instead.
        //
        // Geometry: all inputs collapse to one point at the content's left
        // edge, all outputs to one point at its right edge, on a single
        // zero-height row under the title. The row is spaced out to the node's
        // LAST MEASURED width, so the block keeps the footprint the node had at
        // full detail instead of shrink-snapping to its title -- UE preserves
        // size across every one of its swaps, three different ways, and says so
        // (SGraphNode.cpp:947 "Saving enough space for a 'typical' title so the
        // transition isn't quite so abrupt"; SAnimationGraphNode's cached
        // GetLowDetailDesiredSize is the same idea done properly). That is also
        // what makes the width a fixed point: the row reproduces contentW, so
        // the block re-measures to the same number every frame.
        //
        // INTERACTION: a zero-size pin has a zero-size hot zone, so the pins
        // stop being grabbable -- which is the intent behind UE turning pins
        // HitTestInvisible at low LOD ("The pin becomes too small to use at low
        // LOD, so disable the hit test", SGraphPin.cpp:1481-1489). Selecting,
        // hovering, dragging and the node context menu all come off the NODE's
        // bounds and keep working. Nothing invisible is left behind: hidden
        // widgets are not submitted at all, so there are no dead hit zones.
        if (!showPinRows)
        {
            for (std::uint32_t pin = 0; pin < Arcane::GraphNodeInputCount(n); ++pin)
            {
                ed::BeginPin(InPin(n.id, pin), ed::PinKind::Input);
                // Zero-size anchor: the pivot the alignment path would have
                // produced from a zero-size rect IS the cursor, so naming it
                // outright changes no geometry and gives the gradient wires an
                // anchor at the one tier that has no dot to hang off.
                SetPinPivot(InPin(n.id, pin).Get(), ImGui::GetCursorScreenPos());
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ed::EndPin();
                ImGui::SameLine(0.0f, 0.0f);
            }
            RightAlignRow(contentW, 0.0f);
            for (std::uint32_t pin = 0; pin < Arcane::GraphNodeOutputCount(n); ++pin)
            {
                ed::BeginPin(OutPin(n.id, pin), ed::PinKind::Output);
                SetPinPivot(OutPin(n.id, pin).Get(), ImGui::GetCursorScreenPos());
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ed::EndPin();
                ImGui::SameLine(0.0f, 0.0f);
            }
            ImGui::Dummy(ImVec2(0.0f, 0.0f));   // terminate the SameLine chain
        }

        for (std::uint32_t pin = 0; showPinRows && pin < Arcane::GraphNodeInputCount(n); ++pin)
        {
            // Read the descriptor BEFORE any of the widgets below can mutate
            // the node: for a Custom pin, GraphPinDesc::name points into
            // n.customPins (MaterialGraph.hpp:376-377), so it must be consumed
            // ahead of the remove/rename controls.
            const Arcane::GraphPinDesc inDesc = Arcane::GraphNodeInputPin(n, pin);
            ed::BeginPin(InPin(n.id, pin), ed::PinKind::Input);
            // Wires land on the dot, not on the row's bounding corner: pivot at
            // the row's left edge, vertically centred, with a zero-size pivot
            // so the anchor is that single point.
            // LowDetail keeps the DOT and drops the label -- UE's split
            // exactly: the low-detail slot of a pin holds PinWidgetRef, the pin
            // icon, and drops PinContent, the label and value editor
            // (SGraphPin.cpp:354-364). The dot advances the cursor by a full
            // text line either way (DrawPinDot), so the row keeps its height
            // and the node keeps its shape across the transition.
            const ImVec2 inDot = DrawPinDot(PinColorForWidth(inDesc.width), pinWired(pin));
            // One radius OUTBOARD of the dot's centre -- the row's left edge,
            // which is exactly where the (0, 0.5) alignment used to put the
            // pivot: the dot is the row's first item, so pinRect.Min.x is its
            // left edge and the row's vertical centre is the dot's centre
            // (the dot's dummy is the full text line height). Same point as
            // before, now stated instead of inferred.
            SetPinPivot(InPin(n.id, pin).Get(),
                        ImVec2(inDot.x - kPinDotRadius, inDot.y));
            if (showPinText)
            {
                ImGui::SameLine();
                ImGui::TextUnformatted(inDesc.name);
            }
            ed::EndPin();
            // Custom pins are user-authored: width cycle + remove beside each.
            // Small per-pin controls, so they go with the labels -- UE collapses
            // its "+ Add pin" button at the same threshold
            // (SGraphNode.cpp:1681-1692).
            if (showPinText && n.type == Arcane::GraphNodeType::Custom)
            {
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(pin));
                Arcane::GraphCustomPin& cp = n.customPins[pin];
                const char* wname = cp.width == 1 ? "f1" : cp.width == 2 ? "f2" : "f4";
                if (ImGui::SmallButton(wname))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    cp.width = cp.width == 1 ? 2 : cp.width == 2 ? 4 : 1;
                    valueEdited();
                    PushGraphUndo("Pin Width", std::move(before));
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                {
                    // Remove the pin: drop its links, re-index links to later
                    // pins (toPin is a bare index into this node's pin list).
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    Arcane::MaterialGraph& gg = *ActiveGraphOpt();
                    std::erase_if(gg.links, [&](const Arcane::GraphLink& l)
                                  { return l.toNode == n.id && l.toPin == pin; });
                    for (Arcane::GraphLink& l : gg.links)
                        if (l.toNode == n.id && l.toPin > pin)
                            --l.toPin;
                    n.customPins.erase(n.customPins.begin() + pin);
                    // Pin literals index pins exactly like links do, so they
                    // need the same re-index -- otherwise a removed pin's
                    // value would resurface on whatever pin slid into its
                    // index (silently, since the reader only range-checks).
                    std::erase_if(n.pinLiterals,
                                  [&](const Arcane::GraphPinLiteral& pl)
                                  { return pl.pin == pin; });
                    for (Arcane::GraphPinLiteral& pl : n.pinLiterals)
                        if (pl.pin > pin)
                            --pl.pin;
                    valueEdited();
                    PushGraphUndo("Remove Pin", std::move(before));
                    ImGui::PopID();
                    break;   // pin list changed under this loop -- redraw next frame
                }
                ImGui::PopID();
            }

            // Inline literal on an UNWIRED input pin (SG/UE parity: a pin
            // carries a value with no Const node feeding it). Codegen's argOr
            // checks `connected` FIRST (MaterialGraph.cpp:694-701), so a wire
            // hides the literal without destroying it -- which is why this
            // widget only has to disappear, never clear anything.
            // It is also the pin's default-VALUE widget, which is the other
            // half of what UE's low-detail pin slot drops (SGraphPin.cpp:304-341
            // builds LabelAndValue; :354-364 swaps the whole thing out).
            if (showPinText && !pinWired(pin) && Arcane::GraphPinAcceptsLiteral(n, pin))
            {
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(pin));
                const int lanes =
                    Arcane::GraphPinLiteralLanes(Arcane::GraphNodeInputPin(n, pin).width);
                float buf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                const Arcane::GraphPinLiteral* lit = n.FindPinLiteral(pin);
                bool numericDefault = true;
                if (lit)
                    std::memcpy(buf, lit->v, sizeof(buf));
                else
                    numericDefault = PinNeutralDefault(n, pin, buf);
                // A non-constant neutral (Panner's v.uv) prints as ITSELF: a
                // format string carrying no conversion is explicitly tolerated
                // by ImGui -- RoundScalarWithFormatT returns the value
                // untouched when "the value is not visible in the format
                // string" (ThirdParty/imgui/imgui_widgets.cpp:2496).
                const char* fmt = numericDefault ? "%.3f" : "v.uv";
                ImGui::SetNextItemWidth(lanes == 1 ? 64.0f : lanes == 2 ? 106.0f : 190.0f);
                const bool changed =
                    lanes == 1 ? ImGui::DragFloat("##lit", buf, 0.01f, 0.0f, 0.0f, fmt)
                    : lanes == 2 ? ImGui::DragFloat2("##lit", buf, 0.01f, 0.0f, 0.0f, fmt)
                                 : ImGui::DragFloat4("##lit", buf, 0.01f, 0.0f, 0.0f, fmt);
                // Same bracketing as the Const payload drags below, and STRICTLY
                // safer: the drag wrote `buf`, not the graph, so the snapshot
                // this takes on the activation frame is always pre-edit.
                gestureBegin("Pin Value");
                if (changed)
                {
                    // ONE entry per pin, updated IN PLACE. A duplicate would
                    // make serialization non-deterministic: the writer sorts by
                    // pin with std::sort, which is unstable
                    // (MaterialGraph.cpp:1382-1384), and the reader keeps the
                    // FIRST entry for a pin (:1561-1562).
                    Arcane::GraphPinLiteral* slot = nullptr;
                    for (Arcane::GraphPinLiteral& pl : n.pinLiterals)
                        if (pl.pin == pin)
                        {
                            slot = &pl;
                            break;
                        }
                    if (!slot)
                    {
                        // Absent-until-touched: the entry is BORN here, seeded
                        // with what the field was already showing (the neutral),
                        // so the first nudge moves the material by one drag step
                        // instead of jumping to zero.
                        Arcane::GraphPinLiteral fresh;
                        fresh.pin = pin;
                        n.pinLiterals.push_back(fresh);
                        slot = &n.pinLiterals.back();
                    }
                    for (int i = 0; i < 4; ++i)
                        slot->v[i] = i < lanes ? buf[i] : 0.0f;
                    valueEdited();
                }
                gestureEnd();
                ImGui::PopID();
            }
        }

        // Per-type payload: value drags, cycle buttons, name fields, the Custom
        // node's HLSL preview. All of it is text and small controls, so it goes
        // at the same threshold as the pin labels -- UE drops its whole
        // equivalent band there (sequence-player scrub slider ->  16x16 spacer,
        // SGraphNodeSequencePlayer.cpp:159-180; anim function/tag chips,
        // SAnimationGraphNode.cpp:209-212; AI node description text,
        // SGraphNodeAI.cpp:103-108). A node being renamed has already been
        // pulled back to DefaultDetail above, so nothing is yanked mid-edit.
        //
        // Skipped by dispatching to a value with no enumerator rather than by
        // wrapping 250 lines in an `if` -- GraphNodeType has a fixed uint8_t
        // base (MaterialGraph.hpp:43), so 0xFF is a valid VALUE that no case
        // labels and every case list that grows will keep not labelling. It can
        // only reach `default:`.
        switch (showPinText ? n.type : static_cast<Arcane::GraphNodeType>(0xFF))
        {
            case Arcane::GraphNodeType::ConstFloat:
            {
                ImGui::SetNextItemWidth(90.0f);
                const bool changed = ImGui::DragFloat("##v", &n.value[0], 0.01f);
                gestureBegin("Edit Value");
                if (changed) valueEdited();
                gestureEnd();
                break;
            }
            case Arcane::GraphNodeType::ConstFloat2:
            {
                ImGui::SetNextItemWidth(140.0f);
                const bool changed = ImGui::DragFloat2("##v", n.value, 0.01f);
                gestureBegin("Edit Value");
                if (changed) valueEdited();
                gestureEnd();
                break;
            }
            case Arcane::GraphNodeType::ConstFloat4:
            case Arcane::GraphNodeType::ConstColor:
            {
                ImGui::SetNextItemWidth(220.0f);
                const bool changed = ImGui::DragFloat4("##v", n.value, 0.01f);
                gestureBegin("Edit Value");
                if (changed) valueEdited();
                gestureEnd();
                if (n.type == Arcane::GraphNodeType::ConstColor)
                {
                    ImGui::SameLine();
                    // Preview swatch only (LINEAR floats; the full picker is a
                    // popup and popups cannot open inside the canvas).
                    ImGui::ColorButton("##swatch",
                                       ImVec4(n.value[0], n.value[1], n.value[2], n.value[3]),
                                       ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
                }
                break;
            }
            case Arcane::GraphNodeType::Param:
            case Arcane::GraphNodeType::TextureSample:
            {
                // Name: StableTextEdit holds the typed text while the InputText
                // is active; committed as ONE undoable edit on deactivate-
                // after-edit.
                StableTextEdit("##pname", m_textEdit,
                               TextKey(TextEditKind::NodeName, n.id),
                               n.paramName, 110.0f,
                               [&](const char* text)
                               {
                                   std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                                   const std::string oldName = n.paramName;
                                   const std::string newName = text;
                                   n.paramName = newName;
                                   valueEdited();
                                   PushGraphUndo("Rename Param", std::move(before));
                                   // Assisted rename: local override fix + the
                                   // dependent-instance walk (arms the
                                   // propagation modal on hits). Both names are
                                   // independent COPIES: BeginParamRename takes
                                   // const refs and re-scans every graph's nodes
                                   // for the old name -- THIS node included --
                                   // so neither argument should alias the node
                                   // it is reasoning about. (The pre-widget-
                                   // layer code passed m_nameBuf for the same
                                   // reason.)
                                   BeginParamRename(oldName, newName);
                               });

                if (n.type == Arcane::GraphNodeType::Param)
                {
                    // Type cycle button (popup-free combo stand-in).
                    static constexpr const char* kTypeNames[] = { "float", "float2",
                                                                  "float4", "color" };
                    static constexpr Arcane::MatParamType kTypes[] = {
                        Arcane::MatParamType::Float, Arcane::MatParamType::Float2,
                        Arcane::MatParamType::Float4, Arcane::MatParamType::Color,
                    };
                    int typeIdx = 0;
                    for (int t = 0; t < 4; ++t)
                        if (kTypes[t] == n.paramType)
                            typeIdx = t;
                    if (ImGui::SmallButton(kTypeNames[typeIdx]))
                    {
                        std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                        n.paramType = kTypes[(typeIdx + 1) % 4];
                        n.paramDefault.type = n.paramType;
                        valueEdited();
                        PushGraphUndo("Param Type", std::move(before));
                    }

                    // Default value at the decl's width.
                    const int lanes =
                        static_cast<int>(Arcane::ComponentCount(n.paramType));
                    ImGui::SetNextItemWidth(lanes == 1 ? 90.0f : lanes == 2 ? 140.0f : 220.0f);
                    bool changed = false;
                    if (lanes == 1)
                        changed = ImGui::DragFloat("##pdef", &n.paramDefault.f[0], 0.01f);
                    else if (lanes == 2)
                        changed = ImGui::DragFloat2("##pdef", n.paramDefault.f, 0.01f);
                    else
                        changed = ImGui::DragFloat4("##pdef", n.paramDefault.f, 0.01f);
                    gestureBegin("Param Default");
                    if (changed) valueEdited();
                    gestureEnd();

                    bool ranged = n.hasRange;
                    if (ImGui::Checkbox("range", &ranged))
                    {
                        std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                        n.hasRange = ranged;
                        valueEdited();
                        PushGraphUndo("Param Range", std::move(before));
                    }
                    if (n.hasRange)
                    {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(120.0f);
                        float mm[2] = { n.rangeMin, n.rangeMax };
                        const bool rchanged = ImGui::DragFloat2("##prange", mm, 0.05f);
                        gestureBegin("Param Range");
                        if (rchanged)
                        {
                            n.rangeMin = mm[0];
                            n.rangeMax = mm[1];
                            valueEdited();
                        }
                        gestureEnd();
                    }
                }
                break;
            }
            case Arcane::GraphNodeType::PassInput:
            {
                // Which wired slot to sample: cycle button (validity against
                // the pass's actual wiring is codegen's job -- the badge says
                // when a slot is not wired).
                const char* slotName[4] = { "in0", "in1", "in2", "in3" };
                if (ImGui::SmallButton(slotName[n.passInputSlot %
                                                Arcane::kMaxPassInputs]))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    n.passInputSlot = (n.passInputSlot + 1) % Arcane::kMaxPassInputs;
                    valueEdited();
                    PushGraphUndo("Input Slot", std::move(before));
                }
                break;
            }
            case Arcane::GraphNodeType::Panner:
            {
                // UE's bFractionalPart: wrap the Time*speed offset in frac().
                // A checkbox is a DISCRETE edit -- one click IS the whole
                // change -- so it takes this file's discrete-edit shape
                // (snapshot inline, mutate, push immediately), copied from the
                // "range" checkbox in the Param case above, which is the same
                // widget doing the same job. The gestureBegin/gestureEnd
                // bracket beside it exists to coalesce a MULTI-FRAME drag into
                // one undo step; a click has nothing to coalesce, and routing
                // it through the bracket would push the step a frame late for
                // no benefit. Reading the flip through a local `frac` keeps the
                // graph unmutated until after the snapshot is taken.
                bool frac = n.pannerFractional;
                if (ImGui::Checkbox("frac", &frac))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    n.pannerFractional = frac;
                    valueEdited();
                    PushGraphUndo("Panner Fraction", std::move(before));
                }
                break;
            }
            case Arcane::GraphNodeType::Swizzle:
            {
                // Mask edit: same StableTextEdit commit as param names (one
                // shared TextCommitState -- only one InputText is active at a
                // time; the keys are namespaced per site kind).
                StableTextEdit("##mask", m_textEdit, TextKey(TextEditKind::Swizzle, n.id),
                               n.swizzleMask, 70.0f,
                               [&](const char* text)
                               {
                                   std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                                   n.swizzleMask = text;
                                   valueEdited();
                                   PushGraphUndo("Edit Swizzle", std::move(before));
                               });
                break;
            }
            case Arcane::GraphNodeType::Custom:
            {
                // Add-pin + output width; the pin rows above carry the per-pin
                // width/remove controls.
                if (ImGui::SmallButton("+ pin"))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    Arcane::GraphCustomPin p;
                    for (std::uint32_t k = 1;; ++k)
                    {
                        p.name = "p" + std::to_string(k);
                        bool taken = false;
                        for (const Arcane::GraphCustomPin& other : n.customPins)
                            taken = taken || other.name == p.name;
                        if (!taken)
                            break;
                    }
                    n.customPins.push_back(std::move(p));
                    valueEdited();
                    PushGraphUndo("Add Pin", std::move(before));
                }
                ImGui::SameLine();
                const char* ow = n.customOutWidth == 1 ? "out: f1"
                                : n.customOutWidth == 2 ? "out: f2" : "out: f4";
                if (ImGui::SmallButton(ow))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    n.customOutWidth = n.customOutWidth == 1 ? 2
                                       : n.customOutWidth == 2 ? 4 : 1;
                    valueEdited();
                    PushGraphUndo("Output Width", std::move(before));
                }

                // Body PREVIEW only, as plain draw-list text (an in-node
                // InputTextMultiline is a CHILD WINDOW -- it doesn't ride the
                // canvas transform, so its text drifts while the node drags and
                // ignores zoom). Editing happens in a Suspend'ed popup (normal
                // ImGui space), opened by the button below.
                {
                    std::string_view bodyText = n.customBody;
                    int shown = 0;
                    while (!bodyText.empty() && shown < 8)
                    {
                        const std::size_t nl = bodyText.find('\n');
                        std::string_view lineText = bodyText.substr(0, nl);
                        if (!lineText.empty() && lineText.back() == '\r')
                            lineText.remove_suffix(1);
                        std::string display(lineText.substr(0, 48));
                        if (lineText.size() > 48)
                            display += "...";
                        ImGui::TextDisabled("%s", display.c_str());
                        ++shown;
                        if (nl == std::string_view::npos)
                            break;
                        bodyText.remove_prefix(nl + 1);
                    }
                    if (!bodyText.empty() && shown == 8)
                        ImGui::TextDisabled("...");
                }
                if (ImGui::SmallButton("Edit HLSL..."))
                    m_bodyEditRequest = n.id;
                break;
            }
            default:
                break;
        }

        for (std::uint32_t pin = 0; showPinRows && pin < Arcane::GraphNodeOutputCount(n); ++pin)
        {
            const Arcane::GraphPinDesc outDesc = Arcane::GraphNodeOutputPin(n, pin);
            // Label dropped with the input labels; the row still measures to the
            // dot, so the dot stays welded to the node's right edge either way.
            const float rowW = showPinText
                                   ? ImGui::CalcTextSize(outDesc.name).x +
                                         ImGui::GetStyle().ItemSpacing.x +
                                         kPinDotRadius * 2.0f
                                   : kPinDotRadius * 2.0f;
            RightAlignRow(contentW, rowW);
            ed::BeginPin(OutPin(n.id, pin), ed::PinKind::Output);
            if (showPinText)
            {
                ImGui::TextUnformatted(outDesc.name);
                ImGui::SameLine();
            }
            const ImVec2 outDot = DrawPinDot(PinColorForWidth(outDesc.width), pinFanout(pin));
            // Mirror of the input row: the dot is the row's LAST item, so the
            // (1, 0.5) alignment's pinRect.Max.x was the dot's right edge.
            SetPinPivot(OutPin(n.id, pin).Get(),
                        ImVec2(outDot.x + kPinDotRadius, outDot.y));
            ed::EndPin();
        }

        if (showPreview)
            DrawNodePreviewImage(n, contentW);

        ImGui::PopID();
        ed::EndNode();

        // TITLE BAND + width measurement, both of which need the node's final
        // laid-out rect and so can only happen here. GetNodeBackgroundDrawList
        // paints into the node's own user-background channel -- above its body
        // fill, below its content and pin chrome
        // (imgui_node_editor.cpp:135-140) -- which is exactly where a header
        // band belongs. Coordinates are canvas space, the space both
        // GetNodePosition and plain ImGui use inside ed::Begin/End.
        //
        // Runs at EVERY tier, unguarded, and both halves want it to. The band
        // is what a LowestDetail node IS -- with the pin rows gone the node's
        // whole height is the header, so the band fills it and the block reads
        // as one colored bar with a title. And the measurement is what makes
        // the block keep its full-detail WIDTH: the anchor row above reproduces
        // contentW, so nodeSize.x comes back the same number it went in as, and
        // the cache neither drifts nor forgets across a zoom-out/zoom-in round
        // trip. (The one case it does move is a node born while zoomed out --
        // there is no earlier width to remember, so the block sizes to its
        // title and re-measures on the way back in.)
        //
        // The band's bottom edge is pinned to the TITLE's rect (headerMaxY,
        // captured before anything else is submitted) and NOT to the node's
        // content extent. That is what keeps kNodeHeaderGap readable as body
        // space: the gap dummy is laid out after headerMaxY is taken, so it
        // pushes the first row down without dragging the band with it. Deriving
        // this edge from the laid-out content instead would silently swallow the
        // gap and put the whole fix back where it started.
        const ImVec2 nodePos  = ed::GetNodePosition(ed::NodeId(n.id));
        const ImVec2 nodeSize = ed::GetNodeSize(ed::NodeId(n.id));
        if (nodeSize.x > 0.0f)
        {
            m_nodeWidths[n.id] = nodeSize.x;
            if (ImDrawList* bg = ed::GetNodeBackgroundDrawList(ed::NodeId(n.id)))
                bg->AddRectFilled(
                    ImVec2(nodePos.x + kNodeBorderWidth, nodePos.y + kNodeBorderWidth),
                    ImVec2(nodePos.x + nodeSize.x - kNodeBorderWidth,
                           headerMaxY + kNodePadY),
                    ImGui::GetColorU32(kNodeTitleColor),
                    kNodeRounding, ImDrawFlags_RoundCornersTop);
        }
    }

    void ShaderEditorDocument::SetPinPivot(std::uint64_t pinId, ImVec2 p)
    {
        // PinPivotRect writes Pin::m_Pivot directly and clears m_ResolvePivot
        // (imgui_node_editor.cpp:5443-5448), so EndPin's alignment/size path
        // (:5412-5423) is skipped and the pivot IS this point. That is the
        // whole trick: the endpoint stops being something we infer from the
        // row's item rect and becomes something we hand over, so the curve we
        // draw and the curve the library hit-tests cannot drift apart.
        //
        // The pivot is a degenerate rect. With PinRadius and PinArrowSize both
        // 0 and SnapLinkToPinDir off (all defaults, imgui_node_editor.h:253-259
        // -- this document never overrides them), GetClosestLine's extents are
        // 0 and ImRect_ClosestLine of two points returns those two points
        // (imgui_node_editor.cpp:612-636), so Link::m_Start / m_End land
        // exactly here.
        ed::PinPivotRect(p, p);
        m_pinPivots[pinId] = p;
    }

    void ShaderEditorDocument::DrawGradientWire(std::uint64_t fromPinId,
                                                std::uint64_t toPinId,
                                                const ImVec4& fromColor,
                                                const ImVec4& toColor,
                                                bool emphasize) const
    {
        const auto itA = m_pinPivots.find(fromPinId);
        const auto itB = m_pinPivots.find(toPinId);
        // A pin that did not draw this frame has no anchor. ed::Link refuses
        // the same link for the same reason (DoLink bails on a non-live pin,
        // imgui_node_editor.cpp:1639-1640), so drawing nothing matches what the
        // interaction layer already decided.
        if (itA == m_pinPivots.end() || itB == m_pinPivots.end())
            return;

        const ImVec2 p0 = itA->second;
        const ImVec2 p3 = itB->second;

        // Reproduce Link::GetCurve (imgui_node_editor.cpp:955-982) exactly.
        // Style is READ rather than assumed, so a later LinkStrength or
        // direction change moves our curve and the library's together.
        const ed::Style& st = ed::GetStyle();
        const float dx = p3.x - p0.x;
        const float dy = p3.y - p0.y;
        const float halfDistance = std::sqrt(dx * dx + dy * dy) * 0.5f;
        auto ease = [halfDistance](float strength)
        {
            // Guarded against a zero strength the library never divides by
            // (its own branch is only entered when halfDistance < strength,
            // which a zero strength cannot satisfy).
            constexpr float kPi = 3.14159265358979323846f;
            if (strength > 0.0f && halfDistance < strength)
                return strength * std::sin(kPi * 0.5f * halfDistance / strength);
            return strength;
        };
        const float startStrength = ease(st.LinkStrength);
        const float endStrength   = ease(st.LinkStrength);
        const ImVec2 p1(p0.x + st.SourceDirection.x * startStrength,
                        p0.y + st.SourceDirection.y * startStrength);
        const ImVec2 p2(p3.x + st.TargetDirection.x * endStrength,
                        p3.y + st.TargetDirection.y * endStrength);

        const ImVec4 a = emphasize ? BrightenColor(fromColor) : fromColor;
        const ImVec4 b = emphasize ? BrightenColor(toColor)   : toColor;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Defensive: the link channels exist from Begin, but never index past
        // a splitter that has not been grown.
        if (dl->_Splitter._Count <= kLinkChannelLinks)
            return;
        const int prevChannel = dl->_Splitter._Current;
        dl->ChannelsSetCurrent(kLinkChannelLinks);

        const ImU32 colA = ImGui::GetColorU32(a);
        if (colA == ImGui::GetColorU32(b))
        {
            // Same type both ends -- the overwhelmingly common case. One call,
            // and ImGui's own adaptive tessellation, which is what the flat
            // wire used to get (imgui_node_editor.cpp:501).
            dl->AddBezierCubic(p0, p1, p2, p3, colA, kWireThickness);
        }
        else
        {
            // Segment count tracks the curve's length ON SCREEN, so a wire
            // stays smooth zoomed in without spending verts zoomed out. The
            // control polygon is a cheap upper bound on arc length.
            auto len = [](float ax, float ay) { return std::sqrt(ax * ax + ay * ay); };
            const float polyLen = len(p1.x - p0.x, p1.y - p0.y) +
                                  len(p2.x - p1.x, p2.y - p1.y) +
                                  len(p3.x - p2.x, p3.y - p2.y);
            const float screenLen = polyLen * ViewScale();
            const int segments = static_cast<int>(
                (std::min)(64.0f, (std::max)(12.0f, screenLen / 6.0f)));

            // Per-segment colour means per-segment stroke. Consecutive segments
            // are near-collinear on a curve this smooth, so butt caps meet
            // without visible notches; a shared PathStroke cannot be used
            // because it takes ONE colour for the whole path.
            ImVec2 prev = p0;
            for (int i = 1; i <= segments; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(segments);
                const ImVec2 cur = CubicBezierAt(p0, p1, p2, p3, t);
                // Colour sampled at the segment's MIDPOINT so the two ends of
                // the run land on the pure endpoint colours.
                const float mid = (t + static_cast<float>(i - 1) /
                                       static_cast<float>(segments)) * 0.5f;
                dl->AddLine(prev, cur, ImGui::GetColorU32(LerpColor(a, b, mid)),
                            kWireThickness);
                prev = cur;
            }
        }

        dl->ChannelsSetCurrent(prevChannel);
    }

    void ShaderEditorDocument::HandleGraphEdits()
    {
        Arcane::MaterialGraph& g = *ActiveGraphOpt();

        // Wire creation: one edge per input (silent replace), outputs fan out,
        // cycles refused silently at connect time (all SG rules). Every numeric
        // pin connects to every numeric pin -- the adaptation table absorbs
        // width differences, so validity is purely structural.
        if (ed::BeginCreate())
        {
            ed::PinId aId, bId;
            if (ed::QueryNewLink(&aId, &bId))
            {
                const DecodedPin a = DecodePin(aId);
                const DecodedPin b = DecodePin(bId);
                bool valid = a.valid && b.valid && a.isInput != b.isInput;
                const DecodedPin& out = a.isInput ? b : a;
                const DecodedPin& in = a.isInput ? a : b;
                if (valid)
                    valid = g.FindNode(out.node) && g.FindNode(in.node) &&
                            !WouldCycle(g, out.node, in.node);
                if (!valid)
                    ed::RejectNewItem();
                else if (ed::AcceptNewItem())
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    std::erase_if(g.links, [&](const Arcane::GraphLink& l)
                                  { return l.toNode == in.node && l.toPin == in.pin; });
                    Arcane::GraphLink l;
                    l.fromNode = out.node;
                    l.fromPin = out.pin;
                    l.toNode = in.node;
                    l.toPin = in.pin;
                    g.links.push_back(l);
                    m_dirty = true;
                    if (m_live)
                        RegenerateFromGraph();
                    PushGraphUndo("Connect", std::move(before));
                }
            }
            else if (ed::QueryNewNode(&aId))
            {
                // Wire released over empty canvas -> the create searcher.
                // Popups cannot open here (canvas space); stash the dragged
                // pin and let DrawGraphPanel's Suspend block open it.
                const DecodedPin from = DecodePin(aId);
                if (!from.valid || !g.FindNode(from.node))
                    ed::RejectNewItem();
                else if (ed::AcceptNewItem())
                {
                    m_wireNode = from.node;
                    m_wirePin = from.pin;
                    m_wireIsInput = from.isInput;
                    m_wireCreateRequest = true;
                }
            }
        }
        // UNCONDITIONAL: CreateItemAction::Begin() arms m_InActive even when it
        // returns false (idle frame); a skipped EndCreate() asserts on the NEXT
        // frame's BeginCreate() (the desk crash -- frame 1 fine, frame 2 abort).
        ed::EndCreate();

        // Deletion (multi-select = one undo step). Link ids are this frame's
        // indices -- collect first, erase in descending order after the
        // queries. The Output node refuses deletion (SG: blocks are fixed).
        std::vector<std::size_t> linkIdxs;
        std::vector<std::uint32_t> nodeIds;
        if (ed::BeginDelete())
        {
            ed::LinkId lid;
            while (ed::QueryDeletedLink(&lid))
            {
                const std::size_t idx = static_cast<std::size_t>(lid.Get()) - 1;
                if (idx < g.links.size() && ed::AcceptDeletedItem())
                    linkIdxs.push_back(idx);
                else if (idx >= g.links.size())
                    ed::RejectDeletedItem();
            }
            ed::NodeId nid;
            while (ed::QueryDeletedNode(&nid))
            {
                const std::uint32_t id = static_cast<std::uint32_t>(nid.Get());
                const Arcane::GraphNode* n = g.FindNode(id);
                if (!n || n->type == Arcane::GraphNodeType::Output)
                {
                    ed::RejectDeletedItem();
                    continue;
                }
                if (ed::AcceptDeletedItem())
                    nodeIds.push_back(id);
            }
        }
        // UNCONDITIONAL for the same reason as EndCreate() above.
        ed::EndDelete();

        if (!linkIdxs.empty() || !nodeIds.empty())
        {
            std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
            std::sort(linkIdxs.rbegin(), linkIdxs.rend());
            for (std::size_t idx : linkIdxs)
                g.links.erase(g.links.begin() + static_cast<std::ptrdiff_t>(idx));
            for (std::uint32_t id : nodeIds)
            {
                std::erase_if(g.nodes, [&](const Arcane::GraphNode& n)
                              { return n.id == id; });
                std::erase_if(g.links, [&](const Arcane::GraphLink& l)
                              { return l.fromNode == id || l.toNode == id; });
            }
            m_dirty = true;
            if (m_live)
                RegenerateFromGraph();
            PushGraphUndo("Delete", std::move(before));
        }

        // Copy/paste/cut/duplicate: ed's shortcut actions (canvas focus only).
        // WantTextInput keeps in-canvas text edits (param names, masks) from
        // being hijacked. Cut deletes through ed::DeleteNode so the delete
        // pass above owns the model erase + its undo step (next frame).
        if (ed::BeginShortcut())
        {
            if (!ImGui::GetIO().WantTextInput)
            {
                if (ed::AcceptCopy())
                {
                    const std::string clip = BuildGraphClipJson();
                    if (!clip.empty())
                        ImGui::SetClipboardText(clip.c_str());
                }
                else if (ed::AcceptCut())
                {
                    const std::string clip = BuildGraphClipJson();
                    if (!clip.empty())
                    {
                        ImGui::SetClipboardText(clip.c_str());
                        std::vector<ed::NodeId> sel(
                            static_cast<std::size_t>(std::max(0, ed::GetSelectedObjectCount())));
                        const int count = sel.empty() ? 0
                            : ed::GetSelectedNodes(sel.data(), static_cast<int>(sel.size()));
                        for (int i = 0; i < count; ++i)
                            ed::DeleteNode(sel[static_cast<std::size_t>(i)]);
                    }
                }
                else if (ed::AcceptPaste())
                    PasteGraphClipText(ImGui::GetClipboardText());
                else if (ed::AcceptDuplicate())
                {
                    const std::string clip = BuildGraphClipJson();
                    if (!clip.empty())
                        PasteGraphClipText(clip.c_str());
                }
            }
            ed::EndShortcut();
        }
    }

    std::string ShaderEditorDocument::BuildGraphClipJson()
    {
        const Arcane::MaterialGraph& g = *ActiveGraphOpt();
        std::vector<ed::NodeId> sel(
            static_cast<std::size_t>(std::max(0, ed::GetSelectedObjectCount())));
        if (sel.empty())
            return {};
        const int count = ed::GetSelectedNodes(sel.data(), static_cast<int>(sel.size()));

        Arcane::MaterialGraph sub;
        std::unordered_set<std::uint32_t> picked;
        for (int i = 0; i < count; ++i)
        {
            const std::uint32_t id =
                static_cast<std::uint32_t>(sel[static_cast<std::size_t>(i)].Get());
            const Arcane::GraphNode* node = g.FindNode(id);
            if (!node || node->type == Arcane::GraphNodeType::Output)
                continue;   // the one fixed node never travels
            if (picked.insert(id).second)
                sub.nodes.push_back(*node);
        }
        if (sub.nodes.empty())
            return {};
        // Internal links only -- both endpoints in the selection.
        for (const Arcane::GraphLink& l : g.links)
            if (picked.count(l.fromNode) && picked.count(l.toNode))
                sub.links.push_back(l);

        nlohmann::json j = Arcane::GraphToJson(sub);
        j["kind"] = "arcane-graph-clip";
        return j.dump();
    }

    void ShaderEditorDocument::PasteGraphClipText(const char* text)
    {
        if (!text || !*text)
            return;
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object() ||
            j.value("kind", std::string()) != "arcane-graph-clip")
            return;   // foreign clipboard content -- not ours, ignore silently
        const std::optional<Arcane::MaterialGraph> sub = Arcane::GraphFromJson(j);
        if (!sub)
            return;

        // Recenter the subgraph on the mouse (canvas space).
        float cx = 0.0f, cy = 0.0f;
        int count = 0;
        for (const Arcane::GraphNode& n : sub->nodes)
            if (n.type != Arcane::GraphNodeType::Output)
            {
                cx += n.posX;
                cy += n.posY;
                ++count;
            }
        if (count == 0)
            return;
        cx /= static_cast<float>(count);
        cy /= static_cast<float>(count);
        const ImVec2 at = ed::ScreenToCanvas(ImGui::GetMousePos());

        Arcane::MaterialGraph& g = *ActiveGraphOpt();
        std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
        std::unordered_map<std::uint32_t, std::uint32_t> remap;
        ed::ClearSelection();
        for (const Arcane::GraphNode& src : sub->nodes)
        {
            if (src.type == Arcane::GraphNodeType::Output)
                continue;
            Arcane::GraphNode n = src;
            n.id = g.MintId();   // FRESH ids -- clip ids may collide or be stale
            remap[src.id] = n.id;
            n.posX += at.x - cx;
            n.posY += at.y - cy;
            ed::SetNodePosition(n.id, ImVec2(n.posX, n.posY));
            ed::SelectNode(n.id, true);   // the paste becomes the selection
            g.nodes.push_back(std::move(n));
        }
        for (const Arcane::GraphLink& l : sub->links)
        {
            const auto f = remap.find(l.fromNode);
            const auto t = remap.find(l.toNode);
            if (f == remap.end() || t == remap.end())
                continue;   // endpoint did not paste
            Arcane::GraphLink nl;
            nl.fromNode = f->second;
            nl.fromPin = l.fromPin;
            nl.toNode = t->second;
            nl.toPin = l.toPin;
            g.links.push_back(nl);
        }
        m_dirty = true;
        if (m_live)
            RegenerateFromGraph();
        PushGraphUndo("Paste", std::move(before));
    }

    // One texture param row: current binding + [pick] popup over the project's
    // texture assets + a browser-drag drop target. All three routes land in
    // SetParamWithUndo (single-step undo, no gesture bracketing needed).
    void ShaderEditorDocument::DrawTextureParam(const Arcane::ParamDecl& d,
                                                const Arcane::MatParamValue& current)
    {
        const Arcane::Project* project =
            m_services.runtime ? m_services.runtime->CurrentProject() : nullptr;

        std::string display = "(none)";
        if (current.tex.IsValid())
        {
            display = current.tex.ToString();
            if (project)
                if (const auto mount = project->Registry().Resolve(current.tex))
                    display = *mount;
        }
        ImGui::TextUnformatted(d.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", display.c_str());

        // Drop target: accept a texture asset dragged from the browser.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetDragType))
            {
                const auto* payload = static_cast<const AssetDragPayload*>(p->Data);
                if (payload->kind == AssetKind::Texture)
                    SetParamWithUndo(d, Arcane::MatParamValue::MakeTexture(payload->guid));
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        const std::string pickId = "pick##" + d.name;
        const std::string popupId = "##texpick_" + d.name;
        if (ImGui::SmallButton(pickId.c_str()))
            ImGui::OpenPopup(popupId.c_str());
        if (ImGui::BeginPopup(popupId.c_str()))
        {
            if (!project)
            {
                ImGui::TextDisabled("no project open");
            }
            else
            {
                if (ImGui::Selectable("(none)"))
                    SetParamWithUndo(d, Arcane::MatParamValue::MakeTexture(Arcane::Guid::Nil()));
                for (const AssetEntry& e : BuildAssetEntries(project->Registry()))
                {
                    if (e.kind != AssetKind::Texture)
                        continue;
                    const std::string label = e.name + "##" + e.mountPath;
                    if (ImGui::Selectable(label.c_str(), e.guid == current.tex))
                        SetParamWithUndo(d, Arcane::MatParamValue::MakeTexture(e.guid));
                }
            }
            ImGui::EndPopup();
        }
    }

    void ShaderEditorDocument::SetParamWithUndo(const Arcane::ParamDecl& d,
                                                const Arcane::MatParamValue& value)
    {
        if (!m_instance)
            return;
        const bool hadBefore = m_instance->HasOverride(d.nameHash);
        Arcane::MatParamValue before;
        if (hadBefore)
            m_instance->GetParam(d.nameHash, before);
        if (!m_instance->Set(d.nameHash, value))
            return;
        if (m_services.undo)
            m_services.undo->Push(std::make_unique<ParamEditCommand>(
                m_anchor, d.nameHash, "Edit " + d.name,
                hadBefore, before, /*hasAfter=*/true, value));
        // Sprite surface binds texture params at registration -- a pick needs
        // a binding refresh (numeric params flow live through the CB).
        if (m_surface == 1 && d.type == Arcane::MatParamType::Texture)
            RefreshSpritePreviewBinding();
    }

    void ShaderEditorDocument::DrawParamsPanel()
    {
        ImGui::BeginChild("##params", ImVec2(0, 0), ImGuiChildFlags_Borders);
        if (!m_instance || !m_boundTemplate)
        {
            ImGui::TextDisabled("params appear after the first successful compile");
            ImGui::EndChild();
            return;
        }

        // Each param row's drag rides the document's EditGesture bracket
        // (BeginOnActivate / EndOnDeactivate around the live Set below): before-
        // state on activation, one undo step at close -- one drag = one step.
        if (IsInstance())
            ImGui::Checkbox("Only overridden", &m_showOnlyOverridden);

        const auto& params = m_boundTemplate->Params();
        for (std::size_t i = 0; i < params.size(); ++i)
        {
            const Arcane::ParamDecl& d = params[i];
            if (IsInstance() && m_showOnlyOverridden && !m_instance->HasOverride(d.nameHash))
                continue;

            // Instance mode: the per-param override checkbox (UE's model) --
            // checking materializes an override at the currently-resolved value,
            // unchecking clears it (parent/default shows through). Undoable.
            if (IsInstance())
            {
                bool ov = m_instance->HasOverride(d.nameHash);
                const std::string ovId = "##ov_" + d.name;
                if (ImGui::Checkbox(ovId.c_str(), &ov))
                {
                    if (ov)
                    {
                        Arcane::MatParamValue resolved;
                        if (m_instance->GetParam(d.nameHash, resolved))
                            SetParamWithUndo(d, resolved);
                    }
                    else
                    {
                        Arcane::MatParamValue before;
                        m_instance->GetParam(d.nameHash, before);
                        m_instance->ClearOverride(d.nameHash);
                        if (m_services.undo)
                            m_services.undo->Push(std::make_unique<ParamEditCommand>(
                                m_anchor, d.nameHash, "Reset " + d.name,
                                /*hadBefore=*/true, before, /*hasAfter=*/false,
                                Arcane::MatParamValue{}));
                    }
                }
                ImGui::SameLine();
            }
            const Arcane::ParamMeta meta = i < m_boundMetas.size() ? m_boundMetas[i]
                                                                   : Arcane::ParamMeta{};
            Arcane::MatParamValue value;
            if (!m_instance->GetParam(d.nameHash, value))
                continue;

            bool edited = false;
            switch (WidgetFor(d.type))
            {
                case ParamWidget::SliderFloat:
                    edited = ImGui::SliderFloat(d.name.c_str(), &value.f[0],
                                                meta.sliderMin, meta.sliderMax);
                    break;
                case ParamWidget::DragFloat2:
                    edited = ImGui::DragFloat2(d.name.c_str(), value.f, 0.01f);
                    break;
                case ParamWidget::DragFloat4:
                    edited = ImGui::DragFloat4(d.name.c_str(), value.f, 0.01f);
                    break;
                case ParamWidget::ColorEdit:
                    edited = ImGui::ColorEdit4(d.name.c_str(), value.f);
                    break;
                case ParamWidget::TexturePicker:
                    DrawTextureParam(d, value);
                    break;
            }

            // The override before-state is read INSIDE the open call, which runs
            // on the activation frame only -- i.e. before the live Set below has
            // touched anything. The step itself builds at close (an abandoned
            // drag lands on the stack rather than vanishing), and the
            // transaction carries the label CommandStack::Commit stamps.
            //
            // NO-OP GUARD: the close runs on EVERY close path, including the
            // abandonment ones (stale-close, collapsed window, document
            // teardown) where the gesture never edited anything. Pushing there
            // would leave a junk step whose before == after AND clear the redo
            // stack (CommandStack.cpp:70) -- a generic Push is its own
            // transaction, so it never meets Commit's empty-transaction drop at
            // :61-62. The after-state is the CLOSE-TIME override state, so
            // "no override, nothing typed" reads as unchanged; an EDITED
            // gesture still differs (its live Set both creates the override and
            // moves the value) and still pushes exactly one step.
            EditGesture::BeginOnActivate(m_services.undo, m_gesture,
                [&] { return "Edit " + d.name; },
                [&]
                {
                    const bool hadBefore = m_instance->HasOverride(d.nameHash);
                    Arcane::MatParamValue before{};
                    if (hadBefore)
                        m_instance->GetParam(d.nameHash, before);
                    return std::function<void()>(
                        [this, nameHash = d.nameHash, name = d.name, hadBefore, before]
                        {
                            Arcane::MatParamValue after;
                            if (!m_instance || !m_instance->GetParam(nameHash, after))
                                return;   // the snippet dropped the param
                            // Read the override flag, not a hardcoded true: it
                            // is what distinguishes "the drag created an
                            // override" from "nothing happened", and it is the
                            // truthful Redo target either way (ApplyParamEdit
                            // clears the override when hasAfter is false, the
                            // shape the reset button below pushes).
                            const bool hasAfter = m_instance->HasOverride(nameHash);
                            if (hadBefore == hasAfter &&
                                (!hadBefore || before == after))
                                return;   // nothing changed -- no step, redo intact
                            m_services.undo->Push(std::make_unique<ParamEditCommand>(
                                m_anchor, nameHash, "Edit " + name,
                                hadBefore, before, hasAfter, after));
                        });
                });

            if (edited)
            {
                // LIVE: straight into the instance -> next Tick packs the CB.
                // No recompile -- the whole point of the declared-param model.
                m_instance->Set(d.nameHash, value);
            }

            EditGesture::EndOnDeactivate(m_services.undo, m_gesture);

            // Reset-to-default: clears the override so the //@param default (or
            // a parent's value, Slice 7) shows through. Undoable.
            if (m_instance->HasOverride(d.nameHash))
            {
                ImGui::SameLine();
                std::string resetId = "x##reset_" + d.name;
                if (ImGui::SmallButton(resetId.c_str()))
                {
                    Arcane::MatParamValue before;
                    m_instance->GetParam(d.nameHash, before);
                    m_instance->ClearOverride(d.nameHash);
                    if (m_services.undo)
                        m_services.undo->Push(std::make_unique<ParamEditCommand>(
                            m_anchor, d.nameHash, "Reset " + d.name,
                            /*hadBefore=*/true, before, /*hasAfter=*/false,
                            Arcane::MatParamValue{}));
                }
            }
        }
        ImGui::EndChild();
    }
}

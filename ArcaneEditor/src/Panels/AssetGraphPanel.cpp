#include "Panels/AssetGraphPanel.hpp"

#include "Documents/DocumentHost.hpp"      // the open route a node's double-click hands to OpenAssetRow
#include "Panels/AssetPanelModel.hpp"      // AssetPanelModel/AssetPanelEntry -- this panel's whole read surface
#include "Panels/CreateAssetDialog.hpp"    // CreateAssetKind -- the pin-drag ghost menu's derive request
#include "Widgets/CanvasEditScope.hpp"     // CanvasCreateScope: the unconditional-EndCreate rule
#include "Widgets/CanvasPopupScope.hpp"    // ed::Suspend/Resume around the Graph canvas's node menu
#include "Widgets/EditorFonts.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/GraphCanvasBackdrop.hpp" // DrawGraphCanvasBackdrop -- the pre-ed::Begin grid blit
#include "Widgets/GraphCanvasStyle.hpp"    // node chrome metrics + grid palette + accents -- one definition, both canvases
#include "Widgets/GraphNodeLod.hpp"        // NodeLOD / NodeLODForScale -- the zoom table's third column
#include "Widgets/GraphPinDot.hpp"         // DrawGraphPinDot -- the filled/ring port dot, paint only
#include "Widgets/GraphWire.hpp"           // bezier/lerp/brighten/view-scale + the links channel
#include "Widgets/GraphZoomLevels.hpp"     // ApplyZoomLevels -- same table the shader editor's canvases use
#include "Widgets/IconsLucide.h"

#include <Arcane/Guid.hpp>

#include <imgui.h>
// Plan 3 ruling 1: THE ONE TU that may name ax::NodeEditor for this panel.
// AssetGraphPanel.hpp holds the context as a void* and EditorWidgets stays
// node-editor-free precisely so this include never has to leave this file.
#include <imgui_node_editor.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// AssetGraphPanel (panel-split arc): the "Asset Graph" window. Task 5 moved
// the ax::NodeEditor canvas BODY and its lifecycle here as pure motion out of
// AssetsPanel.cpp's DrawGraphLens (renamed DrawAssetGraphBody), together with
// that body's private helpers (node id/pin id encoding, the node chrome +
// legend painters, the dashed in-flight wire, the canvas style descriptor)
// and the Graph-only geometry constants they share, none of which any other
// view ever called.
//
// Task 7 added the SHELL at the bottom of this file -- DrawAssetGraphPanel,
// the window itself: its own ImGui::Begin("Asset Graph"), the focus-combo
// toolbar (spec s9.1's R2 minimum -- no Create, no search; the combo and its
// GraphFocusLabel/kGraphFocus* constants came across from the retired shared
// DrawToolbar, which is where they always belonged) and the bottom bar (spec
// s9.2), built on AssetPanelCommon's shared band skeleton + digest chip.
// `state` retargeted to AssetGraphPanelState& in the same task.
//
// AssetsGraphProjectionIsCurrent is DELETED as of Task 7 (spec s7.4), not
// moved: it existed only because a same-frame LENS FLIP could put the Graph
// bottom bar on screen in a frame whose body was another lens's. The bar now
// draws inside this file's own Begin/End, after this file's own body, so that
// frame is unrepresentable -- the bar prints realNodeCount when `graphBuilt`
// and the em dash when the canvas was never opened at all.
//
// BootSceneGuid, ScenesByName, DrawAssetPeekTooltip, PillWidth, OpenAssetRow,
// DrawAssetMenuItems, SubkindPillText and kTableRowHeight are NOT here: all
// are genuinely cross-panel (the Browser and/or Status panels call them too),
// so their declarations live on AssetPanelCommon.hpp and -- as of Task 7,
// which retired AssetsPanel.cpp where they used to sit -- their bodies live
// in AssetPanelCommon.cpp. This TU reaches them the same way it always has.
// DrawGraphEdgeSummary went to AssetPanelCommon.cpp WITH DrawAssetPeekTooltip,
// its only caller.
//
// Task 5 round 1 fix: the boot-scene graphFocus seed the first cut of that
// move placed in DrawAssetGraphBody's own preamble is NOT in the body -- it
// is SeedAssetGraphFocus, an inline helper exported from AssetGraphPanel.hpp
// and called by DrawAssetGraphPanel below ahead of the toolbar. See that
// helper's own comment for the one-frame toolbar/bottom-bar disagreement the
// original placement produced.
namespace Arcane::Editor
{
    namespace
    {
        // ===================================================================
        // Plan 3 (spec §10): the Graph lens's ax::NodeEditor canvas
        // ===================================================================
        // Ruling 1: spec §10's "reuses the shader editor's canvas vocabulary"
        // IS the vendored ax::NodeEditor -- the shader editor has no
        // hand-rolled canvas -- and every `ed::` call for this panel lives
        // HERE, in this one TU. Nothing about the node editor reaches
        // AssetGraphPanel.hpp (the context is a `void*` there) or the shared
        // widget layer (CanvasPopupScope.hpp:16-19 makes the same refusal).
        // The graph-framework extraction stays deferred: no schema, no undo,
        // no serialization layer is invented for this canvas.
        //
        // The shader editor is the IDIOM SOURCE, cited per helper below --
        // copied in SHAPE, never by including its header.
        namespace ed = ax::NodeEditor;

        // ---- The focus combo's own vocabulary (Task 7, moved verbatim from
        // the retired shared DrawToolbar) ----------------------------------
        // Plan 3 Task 5: the width of the focus combo. A fixed width, not a
        // content-derived one: a label-derived width would make the combo
        // jump every time the user picked a differently-named scene. 230px is
        // the BOARD's own value, read off `OptionD.dc.html`'s focus well
        // (`width: 230px`) rather than guessed -- longer names ellipsize
        // inside the combo rather than widening it.
        constexpr float kGraphFocusComboWidth = 230.0f;

        // The "no scope root" label -- spelled ONCE, because the combo's
        // preview, the combo's own first entry and the bottom bar's "focus:"
        // clause must all read identically (ruling 6's nil focus, in words).
        constexpr const char* kGraphFocusEverything = "everything";
        // ...and what the same three places say when `graphFocus` names an
        // asset the model no longer has an entry for -- a scene deleted
        // while it was the focus. NOT "everything": the projection does not
        // fall back to everything-mode there (AssetGraphViewModel::Build
        // either builds a tombstone-rooted view or, for a guid the reference
        // index cannot explain either, nothing at all), so saying
        // "everything" would describe a graph that is not on screen.
        constexpr const char* kGraphFocusMissing = "(missing)";

        // What the current scope root is CALLED -- the combo's preview text
        // and the bottom bar's "focus:" clause, one spelling so the two bands
        // can never disagree about what is on screen. The returned pointer is
        // either a literal or borrowed from the model's entry (stable for the
        // frame -- Find()'s own doc comment; nothing between here and the
        // draw mutates the model).
        //
        // fileName, not name: the render comparison against
        // `OptionD-Graph-FINAL.png` caught the stem spelling naming the SAME
        // scene two ways one band apart -- the graph's own node header says
        // "main.arcscene" (DrawGraphNode) and the Status panel's scene cards
        // say "main.arcscene" (DrawSceneCard), so a toolbar reading "main"
        // was the panel's only dissenting voice. The board agrees
        // (`focus: main.arcscene`).
        const char* GraphFocusLabel(const AssetPanelModel& model, const Arcane::Guid& focus)
        {
            if (!focus.IsValid())
                return kGraphFocusEverything;
            const AssetPanelEntry* e = model.Find(focus);
            return e ? e->fileName.c_str() : kGraphFocusMissing;
        }

        // ---- Fixed geometry: spec §11.2's "graph nodes" row, VERBATIM -----
        // "graph nodes | w 180-220, header 24px, accent bar 3px, pins 9px"
        constexpr float kGraphNodeMinWidth   = 180.0f;
        constexpr float kGraphNodeMaxWidth   = 220.0f;
        constexpr float kGraphHeaderHeight   = 24.0f;
        constexpr float kGraphAccentBarWidth = 3.0f;
        // 9px ACROSS: the radius is half the spec's diameter, exactly as the
        // plan spells out ("DrawPinDot -- radius becomes 4.5f for §11.2's
        // 9px").
        constexpr float kGraphPinRadius      = 4.5f;
        // The dot's segment count and ring width are the canvas's own language,
        // identical on both canvases, so they live once in
        // Widgets/GraphCanvasStyle.hpp (kGraphPinSegments / kGraphPinRingWidth).
        // Only the RADIUS above is this lens's -- §11.2's 9px across.

        // ---- Layout pitch (tuning values; Task 5's render comparison against
        // OptionD-Graph-FINAL.png arbitrates the final numbers).
        //
        // COLUMN PITCH is measured off the board rather than guessed: its
        // three columns sit at x = 40 / 330 / 660, i.e. pitches of 290 and
        // 330. 300 sits between them and -- unlike the plan's ~260 starting
        // suggestion -- leaves a real gutter at the §11.2 CEILING too (a
        // 220px node in a 260px column leaves 40px, which is not enough air
        // for the wire's own bulge, let alone the mid-edge label that has to
        // sit in it: at that gap the eased control points still reach ~30px
        // each way).
        //
        // ROW PITCH is the plan's 90 unchanged: a node stands 54px
        // (GraphNodeHeight), so that is 36px of air between stacked rows.
        // Tighter than the board's ~70, deliberately -- the board shows three
        // nodes in a column and a real project's "everything" mode shows
        // dozens.
        constexpr float kGraphColumnPitch = 300.0f;
        constexpr float kGraphRowPitch    = 90.0f;

        // ---- Node internals, read off the board (OptionD.dc.html) ---------
        // `.nhead { height: 24px; padding: 0 8px 0 11px; gap: 6px }` -- the
        // 11px left inset is the 3px accent bar plus 8px of air, which is why
        // it is spelled as those two terms rather than as a bare literal.
        constexpr float kGraphNodePadLeft  = kGraphAccentBarWidth + 8.0f;
        constexpr float kGraphNodePadRight = 8.0f;
        constexpr float kGraphNodeBodyPadY = 6.0f;
        constexpr float kGraphNodeIconGap  = 6.0f;
        constexpr float kGraphHeaderFontPx = 14.0f;   // §11.3: "13-14px secondary via PushFont"
        constexpr float kGraphMetaFontPx   = 13.0f;

        // ---- Canvas palette ----------------------------------------------
        // CONTROLLER RULING (Task 5 render comparison, 2026-09-08): the BOARD
        // WINS over the plan's Theme::kPanel pin. `OptionD.dc.html`'s graph
        // canvas is `background: #121212` -- which is EXACTLY Theme::kWell
        // (EditorTheme.hpp: kWell = 0.071f = #121212), so the board's value
        // and the theme's field-well token are the same colour, not merely
        // close. The plan's kPanel pin was derived from the shader editor's
        // own kCanvasColor precedent (ShaderEditorDocument.cpp:233-238), not
        // from the board; spec §11 makes the mocks the redline and the plan
        // itself appointed the render comparison as the arbiter. Measured
        // before the switch: board canvas (18,18,18) vs its chrome (30,30,30)
        // -- a recessed well; the editor's canvas was (30,30,30), identical
        // to its own toolbar and bottom bar, so the graph field had no edge
        // at all.
        //
        // One constant drives the whole surface family: the grid wash, the
        // ghost/overflow body wash and the un-emphasized wire dim all pull
        // TOWARD this colour, so moving it moves them coherently.
        //
        // FOLLOW-UP RULING (same session): the redline authority covers the
        // node-over-canvas RELATIONSHIPS too, not the canvas alone. Moving the
        // canvas by itself had left the nodes reading as more RAISED than the
        // board's -- measured: board 18 -> band 25 (+7) -> body 30 (+12),
        // against this lens's 18 -> 35 (+17) -> 45 (+27).
        //
        // So the three node surfaces below are the BOARD's, read out of
        // `OptionD.dc.html`'s own CSS rather than sampled off the render:
        //     .node  { background: #1e1e1e; border: 1px solid #0d0d0d; }
        //     .nhead { background: #191919; }
        // and every one has an EXACT EditorTheme token -- the same happy
        // accident kWell was for the canvas -- so all three are spelled as
        // TOKENS, never as literals that would drift off the ramp later:
        //     #1e1e1e = Theme::kPanel  (0.118f)
        //     #191919 = Theme::kChrome (0.098f)
        //     #0d0d0d = Theme::kBorder (0.051f)
        // kBorder landing DARKER than the canvas it outlines is not an
        // oversight: that is the token's stated job ("kBorder is DARKER than
        // every surface it outlines" -- EditorTheme.hpp), and the board draws
        // exactly this (#0d0d0d hairline on a #121212 field).
        //
        // LENS-LOCAL, deliberately. These are this file's own constants, fed
        // to the shared style applier through this lens's own
        // AssetGraphCanvasStyleDesc; the SHADER editor's canvas constants are
        // UNTOUCHED -- the desc carries the divergence rather than dissolving
        // it (Widgets/GraphCanvasStyle.hpp) -- so the ruling moves
        // the Graph lens onto its board without dragging a second canvas --
        // which has its own board, its own review history and no such ruling
        // -- along with it. The accepted cost is that the editor's two
        // canvases no longer read as identically-toned material; recorded here
        // so it reads as a decision rather than as drift.
        //
        // NOT covered by either ruling, so NOT changed: the grid colours (the
        // board's single dot grid is #242424; this lens keeps its minor/major
        // two-tier grid) and the pill/label colours. See the fix report. The
        // grid pair has since moved to Widgets/GraphCanvasStyle.hpp
        // (kGraphGridMinorColor / kGraphGridMajorColor, 2026-09-09) -- the
        // VALUES are unchanged; what changed is that the pair it was
        // byte-identical to on the shader canvas is now the same pair, so the
        // "inherited, not chosen" state has one home instead of two copies with
        // nothing between them.
        constexpr ImVec4 kGraphCanvasColor    = Theme::kWell;                          // #121212
        constexpr ImVec4 kGraphNodeBodyColor  = Theme::kPanel;                         // #1e1e1e
        constexpr ImVec4 kGraphNodeTitleColor = Theme::kChrome;                        // #191919
        constexpr ImVec4 kGraphNodeBorder     = Theme::kBorder;                        // #0d0d0d
        // Selection amber / hover cyan and the four node chrome metrics
        // (rounding + the three border widths) are the editor-wide canvas
        // language, not this lens's taste -- they were the same literals on both
        // canvases and now live once in Widgets/GraphCanvasStyle.hpp
        // (kGraphNodeSelBorderColor / kGraphNodeHovBorderColor,
        // kGraphNodeRounding, kGraphNodeBorderWidth, kGraphNodeHovBorderWidth,
        // kGraphNodeSelBorderWidth). So is the wire thickness
        // (kGraphWireThickness). Only the four SURFACE tones above stay here:
        // those are the board ruling's, and the ruling declines to drag the
        // shader canvas onto them.

        // The subtle anchor -> "+N more" connector: thinner than a data edge
        // on purpose (it is NOT one -- see DrawAssetGraphBody's own comment).
        constexpr float kGraphOverflowWireThickness = 1.5f;
        // How far a wire's colour is pulled toward the canvas when the edge
        // is NOT emphasized. The board's edges read as a mid-gray against the
        // backdrop; dimming the source kind's accent this far lands in the
        // same tonal band while still saying which kind the edge leaves.
        constexpr float kGraphWireDim         = 0.62f;
        constexpr float kGraphOverflowWireDim = 0.78f;
        // The ghost/overflow body wash: the canvas tone laid back over the
        // node body at partial alpha, which pulls a tombstone or a "+N more"
        // chip toward the backdrop without inventing a second body colour.
        constexpr float kGraphGhostWash = 0.55f;

        // ---- Task 6: the dashed in-flight wire ----------------------------
        // `stroke-dasharray: 6 5` on the board's amber drag path
        // (OptionD.dc.html / Demo.dc.html: the `M230,330 C320,330 390,402
        // 462,402` path), in SCREEN pixels -- the walk below divides by the
        // view scale so a dash keeps that reading at every zoom stop.
        constexpr float kGraphDashOnPx  = 6.0f;
        constexpr float kGraphDashOffPx = 5.0f;
        // The board's `<circle cx="462" cy="402" r="4">` -- the cursor end of
        // the drag wears a solid amber dot, which is what makes the free end
        // read as "attached to the pointer" rather than as a wire that just
        // stops.
        constexpr float kGraphDashEndDotRadius = 4.0f;
        // LOD floor. The dash walk splits the curve at every on/off boundary,
        // so the CELL COUNT -- not the segment count -- is what bounds its
        // work. Zoomed far in, 11 screen pixels is a vanishing distance in
        // canvas units and the pattern is unresolvable anyway; the cap
        // stretches the cell (ratio preserved) rather than letting the walk
        // grind. Screen length is bounded by the viewport in practice, so this
        // is a guard against a pathological view scale, not the common path.
        constexpr int kGraphDashMaxCells = 256;

        // Mid-edge labels (ruling 9) stop being legible long before the nodes
        // do, so they are the first thing the canvas drops on zoom-out. The
        // threshold is the LOD table's LowDetail ceiling -- this lens used to
        // copy that ONE NUMBER (0.250f) into a bare float compare and say so in
        // a comment; it now reads the table itself
        // (Widgets/GraphNodeLod.hpp, NodeLODForScale). At LowDetail and below,
        // labels are skipped; MediumDetail and up draw them.
        constexpr float kGraphLabelFontPx   = 12.0f;   // §11.2's pill/label text size

        // c_LinkChannel_Links (= 7) and the WHOLE two-layer rationale behind it
        // -- why a transparent ed::Link costs nothing, why hover/selection halos
        // survive, and why channel 7 is the only layer that puts a hand-drawn
        // wire where the flat one was -- now live once in Widgets/GraphWire.hpp
        // (kGraphLinkChannel). This lens already deferred to the shader
        // editor's copy of that prose rather than restating it; both now read
        // the header.

        // Spec §11.3's kind-color table, VERBATIM, as a panel-local function
        // in PinColorForWidth's shape (ShaderEditorDocument.cpp:491) --
        // ruling 5: EditorTheme.hpp:27-32 rules domain colour-coding out of
        // the theme, and the kPillAmberBorder precedent (EditorWidgets.cpp:305)
        // covers a spec-pinned hex with no token.
        //
        // KindAccentRgb (AssetPanelModel.hpp) is the table: §11.3's five rows
        // plus Model, which F2c Plan 2 Task 8 adds as an extension rather than
        // a quotation. Every OTHER kind -- Audio/Font/Data/Diagnostic/Other --
        // returns 0 and lands on the theme's neutral grab gray. That fallback
        // is also what a SYNTHETIC OVERFLOW node lands on: it carries
        // AssetKind::Other ALWAYS, never its anchor's kind, precisely so this
        // table cannot paint it as one more instance of whatever it
        // overflowed from (AssetGraphViewModel.hpp's own field comment).
        ImVec4 KindAccentColor(AssetKind kind) noexcept
        {
            const std::uint32_t rgb = KindAccentRgb(kind);
            if (rgb == 0)
                return Theme::kGrab;   // #9a9a9a -- no row, so no invented hue
            const float s = 1.0f / 255.0f;
            return ImVec4(static_cast<float>((rgb >> 16) & 0xff) * s,
                          static_cast<float>((rgb >>  8) & 0xff) * s,
                          static_cast<float>( rgb        & 0xff) * s,
                          1.0f);
        }

        // The accent a node actually WEARS -- its kind hue, except that a
        // tombstone carries the editor's amber attention language instead
        // (ruling 11; its AssetKind is Other by construction, so the §11.3
        // table has nothing to say about it either way). One definition
        // because two things must agree by construction: the node's pin dot
        // (DrawGraphPinDot's `accent`) and the END OF EVERY WIRE THAT LANDS ON
        // THAT PIN (the gradient endpoints, 2026-09-09). If a tombstone's pin
        // is amber, an edge arriving there has to end amber too, or the wire
        // would visibly miss the colour of the dot it touches.
        ImVec4 GraphNodeAccentColor(const GraphNode& n) noexcept
        {
            return n.isTombstone ? Theme::kAmber : KindAccentColor(n.kind);
        }

        // GraphLerpColor (the sRGB lerp) and GraphBrightenColor (a quarter of
        // the way to white) moved to Widgets/GraphWire.hpp, 2026-09-09: both
        // were byte-identical to the shader editor's LerpColor/BrightenColor,
        // as this file's own comment on the latter already said. GraphDimColor
        // below has NO shader counterpart -- that canvas never dims a wire, it
        // only brightens -- so it stays here, lens-local, reading the shared
        // lerp.

        // Toward the canvas: "the same hue, further back".
        ImVec4 GraphDimColor(const ImVec4& c, float t) noexcept
        {
            return GraphLerpColor(c, ImVec4(kGraphCanvasColor.x, kGraphCanvasColor.y,
                                            kGraphCanvasColor.z, c.w), t);
        }

        // ---- Id encoding --------------------------------------------------
        // Node ids are the view model's own node INDEX + 1, never the guid: a
        // synthetic overflow node REUSES its anchor's guid by construction
        // (AssetGraphViewModel.hpp), so a guid is not a unique node key here,
        // and hashing one would swap a collision-free scheme for a merely
        // improbable one. Ids therefore shuffle when the projection is
        // rebuilt -- which costs nothing: a rebuild also re-writes every node
        // position (ruling 2), and the MODEL, not the canvas, is the
        // selection authority (Task 4).
        std::uint64_t GraphNodeIdOf(std::size_t index) noexcept
        {
            return static_cast<std::uint64_t>(index) + 1ull;
        }
        // Two pins per node. LEFT is the node's OUTBOUND (refs / "what I
        // use") side and RIGHT is its INBOUND (referencers / "who uses me")
        // side -- that way round, and not the other, because the layout puts
        // sources on the LEFT (spec §10: "layered left-to-right by dependency
        // depth, sources left, scenes right", and the view model's layer() is
        // the longest OUTBOUND path to a leaf, so a target always sits in a
        // lower column than its referencer). The board agrees: uv_marker.png
        // -- a pure target -- carries a right-hand pin only, and main.arcscene
        // -- a pure referencer -- carries a left-hand pin only.
        //
        // It also falls straight out of the library's curve convention: a
        // link leaves its START pin along SourceDirection (+1,0) and arrives
        // at its END pin along TargetDirection (-1,0), so a wire has to start
        // at the LEFT node's right-hand pin and end at the RIGHT node's
        // left-hand pin. Hence RIGHT pins are ed::PinKind::Output and LEFT
        // pins are ed::PinKind::Input.
        //
        // PINS LIVE IN THEIR OWN NUMERIC RANGE, ABOVE EVERY NODE ID -- and
        // that is a correctness requirement, not tidiness (2026-09-09 desk
        // pass, both defects). imgui-node-editor hit-tests every node and
        // every pin with an ImGui item whose id is the hex of the object's RAW
        // NUMBER and nothing else -- `snprintf(idString, 32, "%p",
        // id.AsPointer())`, imgui_node_editor.cpp:2408. ObjectId's Node/Pin/
        // Link TYPE TAG is a separate member (imgui_node_editor_internal.h:
        // 152-177) and never reaches that string, so the library's own
        // type-safety does NOT extend to the ImGui id: a node and a pin that
        // share a number share an item id.
        //
        // The original `nodeId * 4 + {1,2}` put pin ids at 5,6,9,10,13,14,...
        // -- squarely INSIDE the node range (1..N) from five nodes up. Node #6
        // and node #1's right pin were then two visible items with one id,
        // which ImGui 1.92 reports outright ("Programmer error: 2 visible
        // items with conflicting ID!", imgui.cpp:5076-5081 / 11877).
        //
        // That report is also why the pin-drag gesture read as broken. ImGui
        // draws it through BeginErrorTooltip (imgui.cpp:11921-11943), the ONE
        // tooltip in the library that omits ImGuiWindowFlags_NoInputs --
        // compare BeginTooltipEx:12799, which sets it -- because it hosts a
        // clickable "Item Picker" button; and it forces itself to the display
        // AND focus front every frame, at the cursor. An input-taking,
        // always-topmost window on the pointer owns g.HoveredWindow, so the
        // click aimed at the ghost menu's "Derive Instance..." landed on the
        // error tooltip instead: "the drag works, it shows the button, but I
        // can't click it".
        //
        // 2^32 is chosen so the ranges cannot meet: a projection would need
        // four billion nodes to reach it, and the node/pin arithmetic below is
        // otherwise UNCHANGED (still *4 + {1,2}, so pinId/4 still names the
        // node and pinId%4 still names the side -- the base simply subtracts
        // out first). LINK ids are deliberately left alone: the library never
        // gives a link an ImGui item at all (it hit-tests links by hand
        // through FindLinkAt, imgui_node_editor.cpp:2495-2503), so they cannot
        // take part in this collision.
        inline constexpr std::uint64_t kGraphPinIdBase = 1ull << 32;
        // ...and the base has to SURVIVE the trip through the library, which
        // carries every id as a uintptr_t (ed::PinId is Details::SafePointerType
        // over one). On a 32-bit target 1<<32 truncates to 0, the two ranges
        // silently become one again, and the collision above returns with no
        // symptom until someone hovers the wrong node. Arcane is x64-only today
        // (bin/*-windows-x86_64), so this asserts a fact rather than adding a
        // constraint -- it just makes the fact refuse to be broken quietly.
        static_assert(sizeof(std::uintptr_t) >= 8,
                      "kGraphPinIdBase (1<<32) must survive ed::PinId's uintptr_t; "
                      "on a 32-bit build it truncates to 0 and node/pin ids collide again");
        std::uint64_t GraphLeftPinId(std::uint64_t nodeId) noexcept
        { return kGraphPinIdBase + nodeId * 4ull + 1ull; }
        std::uint64_t GraphRightPinId(std::uint64_t nodeId) noexcept
        { return kGraphPinIdBase + nodeId * 4ull + 2ull; }

        // THIS lens's answers to the shared style desc
        // (Widgets/GraphCanvasStyle.hpp). All 15 ed::Style writes -- 10 colours
        // and 5 scalars -- and their reasoning are there; what is here is only
        // what this canvas differs on, everything else taking the shared
        // default. This lens's old block wrote 13 of the 15: it omitted
        // GroupBg/GroupBorder, which the shared applier now writes as
        // Theme::kNone -- two entries this lens never reads, since it creates no
        // group nodes. See the header for that one disclosed asymmetry.
        GraphCanvasStyleDesc AssetGraphCanvasStyleDesc()
        {
            GraphCanvasStyleDesc d;
            // The board's surfaces (controller ruling 2026-09-08, above).
            d.nodeBody   = kGraphNodeBodyColor;   // #1e1e1e
            d.nodeBorder = kGraphNodeBorder;      // #0d0d0d
            // ZERO node padding, unlike the shader editor's -- and this is the
            // desc's default, so it is spelled here only to say it is a choice:
            // this lens lays its own rows out by hand (SetCursorScreenPos +
            // explicit Dummies) so the 24px header band and the node's total
            // height are EXACT rather than whatever the ambient font metrics
            // plus a padding pair happen to add up to. With no padding the
            // node's content origin IS ed::GetNodePosition, which is also what
            // lets the pin geometry be computed without a frame of readback lag.
            d.nodePadding = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            // groupBg / groupBorder are left at Theme::kNone: this lens creates
            // no group nodes (no ed::Group call anywhere in it), so those two
            // style entries are never read.
            return d;
        }

        // GraphViewScale (the ed::GetCurrentZoom reciprocal flip, "THE TRAP")
        // and GraphCubicBezierAt moved to Widgets/GraphWire.hpp, 2026-09-09:
        // both were byte-identical to the shader editor's ViewScale /
        // CubicBezierAt, guard constant and all.

        // GraphWireControlPoints (the Link::GetCurve reproduction) moved to
        // Widgets/GraphWire.hpp, 2026-09-09, along with the segment budget
        // both wire walks below used to spell out separately
        // (GraphWirePolyLength / GraphWireSegments / GraphWireScreenScale).

        // DrawGraphWire -- the gradient stroke this lens introduced from
        // DrawGradientWire's technique (user directive, 2026-09-09: "like our
        // node graph for the shader can we have our custom gradient lines") --
        // moved to Widgets/GraphWire.hpp, where the shader editor's own copy
        // now calls it too. Its call shape here is unchanged: two FINAL colours
        // (emphasis and dimming stay at the call sites below), an explicit
        // thickness so the overflow connectors can be thinner than a data edge,
        // an explicit viewScale the lens reads once per frame, and the midpoint
        // back for the mid-edge label.

        // The DASHED in-flight wire (Task 6; plan ruling 8 -- spec §11.1's
        // "one new technique" for this plan). Same curve as DrawGraphWire, in
        // the same channel and the same canvas space, walked with an on/off
        // ARC-LENGTH PHASE ACCUMULATOR so the pattern is measured along the
        // curve rather than along t (which would bunch the dashes wherever
        // the bezier is dense).
        //
        // The per-segment loop skeleton is DrawGradientWire's
        // (ShaderEditorDocument.cpp:5484-5496) -- and so is its cap
        // reasoning, which :5480-5483 states: consecutive samples on a curve
        // this smooth are near-collinear, so butt caps meet without visible
        // notches. A dash is a separate stroke by definition here (a shared
        // PathStroke cannot lift its pen), which is the same reason that one
        // could not use one either.
        //
        // ed::Flow's marching dots are deliberately NOT used: they are an
        // animation over an EXISTING link, and this curve has no link behind
        // it -- nor is a travelling dot the board's language (ruling 8).
        void DrawGraphDashedWire(const ImVec2& p0, const ImVec2& p3, const ImVec4& color,
                                 float thickness, float viewScale)
        {
            ImVec2 p1, p2;
            GraphWireControlPoints(p0, p3, p1, p2);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            // Defensive, exactly as DrawGraphWire is: never index past a
            // splitter that has not been grown.
            if (dl->_Splitter._Count <= kGraphLinkChannel)
                return;

            // The shared segment budget (Widgets/GraphWire.hpp) -- the same
            // call DrawGraphWire above makes, so both wires spend vertices the
            // same way BY CONSTRUCTION rather than by a copied expression.
            // `polyLen` and `scale` are kept because the dash walk needs them
            // for its own cell arithmetic below.
            const float polyLen = GraphWirePolyLength(p0, p1, p2, p3);
            const float scale   = GraphWireScreenScale(viewScale);
            const int segments  = GraphWireSegments(polyLen, viewScale);

            const auto len = [](float ax, float ay) { return std::sqrt(ax * ax + ay * ay); };

            // Cell lengths in CANVAS units, so the dash reads 6-on/5-off on
            // screen at any zoom -- then the LOD floor (kGraphDashMaxCells).
            float on  = kGraphDashOnPx  / scale;
            float off = kGraphDashOffPx / scale;
            if (const float floorLen = polyLen / static_cast<float>(kGraphDashMaxCells);
                on + off < floorLen && on + off > 0.0f)
            {
                const float k = floorLen / (on + off);
                on  *= k;
                off *= k;
            }

            const int prevChannel = dl->_Splitter._Current;
            dl->ChannelsSetCurrent(kGraphLinkChannel);
            const ImU32 col = ImGui::GetColorU32(color);

            // `cellLeft` is the distance still owed to the current on/off
            // cell; it carries ACROSS segment boundaries, which is the whole
            // point of accumulating phase rather than dashing each segment.
            bool  ink      = true;
            float cellLeft = on;
            ImVec2 prev = p0;
            for (int i = 1; i <= segments; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(segments);
                const ImVec2 cur = GraphCubicBezierAt(p0, p1, p2, p3, t);
                float segLeft = len(cur.x - prev.x, cur.y - prev.y);
                ImVec2 a = prev;
                // A degenerate segment (both control points coincident, or a
                // zero-length drag) has no length to spend and would divide by
                // zero below.
                while (segLeft > 0.0f && cellLeft > 0.0f)
                {
                    const float step = (std::min)(segLeft, cellLeft);
                    // `a` lies ON the straight run a->cur, so advancing by
                    // step/segLeft of what REMAINS of it is exact.
                    const float u = step / segLeft;
                    const ImVec2 b(a.x + (cur.x - a.x) * u, a.y + (cur.y - a.y) * u);
                    if (ink)
                        dl->AddLine(a, b, col, thickness);
                    a = b;
                    segLeft  -= step;
                    cellLeft -= step;
                    if (cellLeft <= 0.0f)
                    {
                        ink      = !ink;
                        cellLeft = ink ? on : off;
                    }
                }
                prev = cur;
            }

            dl->ChannelsSetCurrent(prevChannel);
        }

        // The free (pointer) end's solid dot, in the same channel as the wire.
        // Separate from the walk above because only the CALLER knows which end
        // the pointer holds -- it orients the curve, so the pin end is p0 for a
        // right-pin drag and p3 for a left-pin one.
        void DrawGraphWireEndDot(const ImVec2& centre, const ImVec4& color)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (dl->_Splitter._Count <= kGraphLinkChannel)
                return;
            const int prevChannel = dl->_Splitter._Current;
            dl->ChannelsSetCurrent(kGraphLinkChannel);
            dl->AddCircleFilled(centre, kGraphDashEndDotRadius,
                                ImGui::GetColorU32(color), kGraphPinSegments);
            dl->ChannelsSetCurrent(prevChannel);
        }

        // The port dot's PAINT moved to Widgets/GraphPinDot.hpp, 2026-09-09
        // (DrawGraphPinDot): the three draw calls were the same three inside
        // the shader editor's own DrawPinDot. What did NOT move is placement --
        // this lens positions its pins on the node's own edge by hand, so its
        // dot contributes nothing to layout, while the shader editor's advances
        // the ImGui cursor and hands back the centre it measured.

        // Trim a node label to fit `maxWidth` under the CURRENT font. The node's
        // WIDTH is pinned by §11.2 and the label has to yield to it, not the
        // other way round.
        //
        // The TRUNCATION ITSELF is EditorWidgets::EllipsisToWidth -- the shared
        // widget layer's, which this same file already calls three times in the
        // Browse lens. This lens had grown a second, hand-written copy of that
        // algorithm (2026-09-09 audit, item B14): the most avoidable duplication
        // in the arc, because there is no node-editor coupling to excuse it.
        //
        // What survives here is the two things that are genuinely this lens's:
        // the marker is the REAL ellipsis U+2026 rather than three ASCII dots
        // (a node label is chrome in a fixed-width chip, not a value button),
        // and a budget of zero or less draws NOTHING rather than a lone marker
        // -- a node whose header pill ate the whole row should show no label at
        // all. Neither is shared, because changing either would move rows the
        // golden editor-ui lane renders.
        std::string GraphEllipsize(const std::string& text, float maxWidth)
        {
            if (maxWidth <= 0.0f)
                return std::string();
            return EllipsisToWidth(text, maxWidth, "\xE2\x80\xA6");   // U+2026
        }

        // Everything one node needs, computed BEFORE submission so the pin
        // geometry (and therefore every wire endpoint) is exact from frame
        // one -- no waiting on ed::GetNodeSize to report a measurement.
        // Honest only because this lens lays the node out by hand and pins
        // its bottom-right corner with an explicit Dummy; see DrawGraphNode.
        struct GraphNodeVisual
        {
            ImVec2 pos;                 // canvas space, top-left
            float  width  = 0.0f;
            float  height = 0.0f;
            bool   hasLeftPin  = false;  // outbound / refs side
            bool   hasRightPin = false;  // inbound / referencers side
            bool   leftConnected  = false;
            bool   rightConnected = false;
        };

        // The node body's one content row: an 18px thumb (or the kind icon
        // in the same cell) plus whatever pills fit, else a dim meta line.
        struct GraphNodeBody
        {
            std::uint64_t thumb = 0;
            const char*   icon  = nullptr;
            std::vector<std::pair<const char*, int>> pills;   // text, AssetPill variant
            std::string   meta;                                // used when there are no pills
        };

        // Node body row height: the 18px thumb cell is the tallest thing in
        // it, so it sets the row.
        float GraphBodyRowHeight()
        {
            return kAssetRowThumbSize;
        }

        float GraphNodeHeight()
        {
            return kGraphHeaderHeight + kGraphNodeBodyPadY + GraphBodyRowHeight() + kGraphNodeBodyPadY;
        }

        // Chrome drawn AFTER ed::EndNode, in the node's own user-background
        // channel -- above the library's body fill, below its content and pin
        // chrome -- which is exactly where a header band and an accent bar
        // belong (DrawNodeTitleBand's rationale, ShaderEditorDocument.cpp:555).
        // Coordinates are canvas space, the space both ed::GetNodePosition
        // and plain ImGui use inside ed::Begin/End.
        //
        // `wash` > 0 lays the canvas tone back over the whole body at that
        // alpha, which is how a tombstone and a "+N more" chip read as ghosts
        // without a second body colour existing; `borderAccent` non-null
        // paints a 1px inset border INSIDE the library's own, so the
        // library's hover/selection border still shows through around it.
        void DrawGraphNodeChrome(std::uint64_t nodeId, const GraphNodeVisual& v,
                                 bool drawBand, const ImVec4* accent,
                                 float wash, const ImVec4* borderAccent)
        {
            ImDrawList* bg = ed::GetNodeBackgroundDrawList(ed::NodeId(nodeId));
            if (!bg)
                return;

            const float b = kGraphNodeBorderWidth;
            const ImVec2 innerMin(v.pos.x + b, v.pos.y + b);
            const ImVec2 innerMax(v.pos.x + v.width - b, v.pos.y + v.height - b);

            if (drawBand)
                bg->AddRectFilled(innerMin, ImVec2(innerMax.x, v.pos.y + kGraphHeaderHeight),
                                  ImGui::GetColorU32(kGraphNodeTitleColor),
                                  kGraphNodeRounding, ImDrawFlags_RoundCornersTop);

            if (accent)
                // Drawn AFTER the band so it runs the node's FULL height, the
                // way the board's `.accent { top: 0; bottom: 0 }` does -- the
                // header is not a separate region the bar stops at.
                bg->AddRectFilled(innerMin, ImVec2(innerMin.x + kGraphAccentBarWidth, innerMax.y),
                                  ImGui::GetColorU32(*accent),
                                  kGraphNodeRounding, ImDrawFlags_RoundCornersLeft);

            if (wash > 0.0f)
                bg->AddRectFilled(innerMin, innerMax,
                                  ImGui::GetColorU32(Theme::WithAlpha(kGraphCanvasColor, wash)),
                                  kGraphNodeRounding);

            if (borderAccent)
                bg->AddRect(innerMin, innerMax, ImGui::GetColorU32(*borderAccent),
                            kGraphNodeRounding, ImDrawFlags_RoundCornersAll,
                            kGraphNodeBorderWidth);
        }

        // Submit one node: the §10 anatomy (header row + 3px kind accent bar
        // + body row + the two edge pins), laid out by hand against the
        // zero-padding node style so the header band is EXACTLY 24px and the
        // node's measured size is exactly `v.width` x `v.height`.
        //
        // Every text run is pure ImDrawList overdraw rather than a real ImGui
        // item -- the RowWithThumb fix's reasoning (EditorWidgets.cpp),
        // applied for a second reason here: a text item's own extent would
        // feed the node's group rect and let a long label push the node past
        // §11.2's 220px ceiling. The only real items submitted are the two
        // width/height Dummies and the pills, all of which are sized against
        // a budget this function computed.
        void DrawGraphNode(std::uint64_t nodeId, const GraphNodeVisual& v,
                           const GraphNodeBody& body, const char* headerIcon,
                           const std::string& headerLabel, const char* headerPill,
                           const ImVec4& accent, bool ghost)
        {
            ed::BeginNode(ed::NodeId(nodeId));

            // With NodePadding zeroed, the cursor at this point IS the node's
            // top-left in canvas space.
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Pins the node's WIDTH. A zero-height item so it costs no rows.
            ImGui::Dummy(ImVec2(v.width, 0.0f));

            // ---- pins, submitted FIRST -----------------------------------
            // Order is load-bearing, not taste: ed::BeginPin opens an ImGui
            // group, and EndGroup contributes a group rect measured from the
            // cursor AT BeginGroup. Submitted after the node's last row that
            // cursor sits one ItemSpacing.y BELOW the node's bottom edge, and
            // the node silently grows by 4px. Submitted here -- with the
            // cursor explicitly parked back at `origin` -- the group is
            // degenerate and contributes exactly nothing, which is what keeps
            // the measured node size equal to `v.height` and therefore keeps
            // the pin centres (and every wire endpoint derived from them)
            // exact from frame one.
            //
            // Each pin's HIT rect straddles the node's border (the board's
            // `left: -5px` / `right: -5px`), which ed::PinRect lets us state
            // outright instead of inferring it from an item rect that would
            // drag the node's own bounds out with it. ed::PinPivotRect then
            // makes the dot's centre the wire anchor, so the curve we
            // hand-draw and the curve the library hit-tests cannot drift
            // apart (SetPinPivot's rationale, ShaderEditorDocument.cpp:5387).
            {
                const float pinY = origin.y + v.height * 0.5f;
                const auto submitPin = [&](std::uint64_t pinId, ed::PinKind kind,
                                           const ImVec2& centre, bool connected)
                {
                    ImGui::SetCursorScreenPos(origin);
                    ed::BeginPin(ed::PinId(pinId), kind);
                    ed::PinRect(ImVec2(centre.x - kGraphPinRadius, centre.y - kGraphPinRadius),
                                ImVec2(centre.x + kGraphPinRadius, centre.y + kGraphPinRadius));
                    ed::PinPivotRect(centre, centre);
                    // BeginPin's group needs one item to close over. Zero
                    // size, at the node's own origin, so it can add nothing
                    // to either rect -- PinRect already pinned the hit rect
                    // explicitly. It also clears ImGui's IsSetPos flag, which
                    // is what keeps EndGroup's
                    // "SetCursorPos to extend boundaries" check quiet.
                    ImGui::Dummy(ImVec2(0.0f, 0.0f));
                    DrawGraphPinDot(ImGui::GetWindowDrawList(), centre, accent,
                                    kGraphNodeBodyColor, kGraphPinRadius, connected);
                    ed::EndPin();
                };
                if (v.hasLeftPin)
                    submitPin(GraphLeftPinId(nodeId), ed::PinKind::Input,
                              ImVec2(origin.x, pinY), v.leftConnected);
                if (v.hasRightPin)
                    submitPin(GraphRightPinId(nodeId), ed::PinKind::Output,
                              ImVec2(origin.x + v.width, pinY), v.rightConnected);
            }

            const ImU32 textCol = ImGui::GetColorU32(ghost ? Theme::kTextDim : Theme::kText);
            const ImU32 dimCol  = ImGui::GetColorU32(Theme::kTextDim);

            // ---- header row ----
            {
                ImGui::PushFont(GetEditorFonts().interRegular, kGraphHeaderFontPx);
                const float lineH  = ImGui::GetTextLineHeight();
                const float rowY   = origin.y + (kGraphHeaderHeight - lineH) * 0.5f;
                float x = origin.x + kGraphNodePadLeft;

                float rightEdge = origin.x + v.width - kGraphNodePadRight;
                if (headerPill)
                    rightEdge -= PillWidth(headerPill) + kGraphNodeIconGap;

                if (headerIcon)
                {
                    dl->AddText(ImVec2(x, rowY), dimCol, headerIcon);
                    x += ImGui::CalcTextSize(headerIcon).x + kGraphNodeIconGap;
                }
                const std::string shown = GraphEllipsize(headerLabel, rightEdge - x);
                dl->AddText(ImVec2(x, rowY), textCol, shown.c_str());
                ImGui::PopFont();

                if (headerPill)
                {
                    // The one real item in the header. Placed by cursor, so
                    // AssetPill's own Dummy lands inside the node's width --
                    // `rightEdge` above already reserved its slot.
                    ImGui::SetCursorScreenPos(
                        ImVec2(origin.x + v.width - kGraphNodePadRight - PillWidth(headerPill),
                               origin.y + (kGraphHeaderHeight - kPillLineHeight) * 0.5f));
                    AssetPill(headerPill, 1);
                }
            }

            // ---- body row ----
            {
                const float rowTop = origin.y + kGraphHeaderHeight + kGraphNodeBodyPadY;
                const float rowH   = GraphBodyRowHeight();
                float x = origin.x + kGraphNodePadLeft;

                if (body.thumb != 0)
                {
                    dl->AddImage(static_cast<ImTextureID>(body.thumb), ImVec2(x, rowTop),
                                 ImVec2(x + kAssetRowThumbSize, rowTop + kAssetRowThumbSize));
                }
                else if (body.icon)
                {
                    const ImVec2 iconSize = ImGui::CalcTextSize(body.icon);
                    dl->AddText(ImVec2(x + (kAssetRowThumbSize - iconSize.x) * 0.5f,
                                       rowTop + (rowH - iconSize.y) * 0.5f),
                                dimCol, body.icon);
                }
                if (body.thumb != 0 || body.icon)
                    x += kAssetRowThumbSize + ImGui::GetStyle().ItemInnerSpacing.x;

                const float budgetEnd = origin.x + v.width - kGraphNodePadRight;
                if (!body.pills.empty())
                {
                    bool first = true;
                    for (const auto& [text, variant] : body.pills)
                    {
                        const float w = PillWidth(text);
                        const float gap = first ? 0.0f : ImGui::GetStyle().ItemSpacing.x;
                        if (x + gap + w > budgetEnd)
                            break;   // never let a pill push the node past §11.2's width
                        x += gap;
                        ImGui::SetCursorScreenPos(ImVec2(x, rowTop + (rowH - kPillLineHeight) * 0.5f));
                        AssetPill(text, variant);
                        x += w;
                        first = false;
                    }
                }
                else if (!body.meta.empty())
                {
                    ImGui::PushFont(GetEditorFonts().interRegular, kGraphMetaFontPx);
                    const std::string shown = GraphEllipsize(body.meta, budgetEnd - x);
                    dl->AddText(ImVec2(x, rowTop + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                                dimCol, shown.c_str());
                    ImGui::PopFont();
                }
            }

            // Pins the node's HEIGHT (and re-asserts its width), which is
            // what makes `v.height` the measured height rather than a guess.
            // LAST, so nothing after it can push the group's bottom edge
            // further down.
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + v.height));
            ImGui::Dummy(ImVec2(v.width, 0.0f));

            ed::EndNode();
        }

        // The Graph lens's node context menu. A distinct id from the shader
        // editor's "##graphnodemenu" even though ImGui scopes popup ids to the
        // current window's id stack (they could never collide): a metrics
        // window listing both by name should say which is which.
        constexpr const char* kGraphNodeMenuId = "##assetgraphnodemenu";

        // The pin-drag ghost menu (Task 6). Its own id for the same reason.
        constexpr const char* kGraphCreateMenuId = "##assetgraphderivemenu";

        // ---- The canvas legend (Task 6, controller rider) -----------------
        // TRANSCRIBED from OptionD.dc.html's `<!-- legend -->` block, value for
        // value -- not designed here:
        //   position: absolute; left: 12px; bottom: 12px;
        //   display: flex; align-items: center; gap: 14px;
        //   background: #191919; border: 1px solid #0d0d0d;
        //   padding: 5px 10px; font-size: 13px; color: #737373;
        //   each entry: inline-flex; gap: 6px
        //     [18x2 solid #5c5c5c]                "derives / samples"
        //     [18x2 solid #4a4a4a]                "used by"
        //     [18px, border-top: 2px dashed #ffa61a] "drag a pin = create"
        // Three of the four tones are exact EditorTheme tokens (#191919 =
        // kChrome, #0d0d0d = kBorder, #737373 = kTextDim, #ffa61a = kAmber),
        // so they are spelled as tokens; the two edge greys have no token and
        // are kept as board literals, the kPillAmberBorder precedent.
        //
        // TWO DELIBERATE DEPARTURES FROM THE TRANSCRIPTION, both COPY only, by
        // CONTROLLER RULING (Task 6 review): a legend must not contradict the
        // lens it describes, so where the board's wording is untrue HERE the
        // minimal truthful edit wins over the transcription. Recorded so the
        // board and the shipped strings can be reconciled at a glance:
        //   1. "used by" -> "uses". Spec §10 / ruling 9 pin References ->
        //      "uses", and that is what the mid-edge labels on this very
        //      canvas say; the legend saying otherwise about the same wire is
        //      simply wrong.
        //   2. "drag a pin = create" -> "drag a material pin = derive". The
        //      board's phrasing promises something every NON-material pin
        //      refuses (the ghost menu's one entry is disabled there), and
        //      "derive" is the word the entry itself uses.
        // The GEOMETRY, the TONES and entry 1's string stay exactly as
        // transcribed.
        //
        // ONE KNOWN MISMATCH REMAINS, deliberately, for the user's desk pass:
        // the two greys legend the board's TWO-TONE edge scheme (asset->asset
        // vs asset->scene), while this lens colours edges by the SOURCE KIND's
        // accent dimmed toward the canvas (ruling 3/§11.3) -- so no drawn edge
        // is exactly either swatch. Left as transcribed by the same ruling:
        // the swatches read as "a line", not as a colour code, and retinting
        // them would be designing rather than transcribing.
        //
        // Chrome, NOT a node: drawn after ed::End in SCREEN space, so it does
        // not pan, zoom or sort against the graph.
        constexpr float kGraphLegendInset      = 12.0f;
        constexpr float kGraphLegendPadX       = 10.0f;
        constexpr float kGraphLegendPadY       = 5.0f;
        constexpr float kGraphLegendEntryGap   = 14.0f;
        constexpr float kGraphLegendSwatchGap  = 6.0f;
        constexpr float kGraphLegendSwatchW    = 18.0f;
        constexpr float kGraphLegendSwatchH    = 2.0f;
        constexpr float kGraphLegendFontPx     = 13.0f;
        constexpr ImVec4 kGraphLegendEdgeColor   = ImVec4(0.361f, 0.361f, 0.361f, 1.0f); // #5c5c5c
        constexpr ImVec4 kGraphLegendUsedByColor = ImVec4(0.290f, 0.290f, 0.290f, 1.0f); // #4a4a4a

        void DrawGraphLegend(const ImVec2& canvasMin, const ImVec2& canvasSize)
        {
            struct Entry { const char* text; ImVec4 color; bool dashed; };
            const Entry entries[] = {
                { "derives / samples", kGraphLegendEdgeColor,   false },
                { "uses",              kGraphLegendUsedByColor, false },
                { "drag a material pin = derive", Theme::kAmber, true },
            };

            ImGui::PushFont(GetEditorFonts().interRegular, kGraphLegendFontPx);
            const float lineH = ImGui::GetTextLineHeight();

            float contentW = 0.0f;
            for (int i = 0; i < IM_ARRAYSIZE(entries); ++i)
            {
                if (i > 0)
                    contentW += kGraphLegendEntryGap;
                contentW += kGraphLegendSwatchW + kGraphLegendSwatchGap +
                            ImGui::CalcTextSize(entries[i].text).x;
            }

            // SNAPPED TO WHOLE PIXELS. A 2px rule and a 1px border are the two
            // things here a half-pixel origin visibly softens (ImGui gives a
            // fractional rect fractional coverage), and the board's are crisp.
            // Safe to snap, unlike anything inside the canvas: the legend is
            // chrome in SCREEN space, with no zoom to make the rounding lie.
            const float boxW = std::floor(contentW) + kGraphLegendPadX * 2.0f;
            const float boxH = std::floor(lineH) + kGraphLegendPadY * 2.0f;
            const ImVec2 boxMin(std::floor(canvasMin.x + kGraphLegendInset),
                                std::floor(canvasMin.y + canvasSize.y - kGraphLegendInset - boxH));
            const ImVec2 boxMax(boxMin.x + boxW, boxMin.y + boxH);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(Theme::kChrome));
            dl->AddRect(boxMin, boxMax, ImGui::GetColorU32(Theme::kBorder));

            const ImU32 textCol = ImGui::GetColorU32(Theme::kTextDim);
            const float midY = boxMin.y + boxH * 0.5f;
            float x = boxMin.x + kGraphLegendPadX;
            for (int i = 0; i < IM_ARRAYSIZE(entries); ++i)
            {
                if (i > 0)
                    x += kGraphLegendEntryGap;
                const ImU32 swatch = ImGui::GetColorU32(entries[i].color);
                x = std::floor(x);
                const float y0 = std::floor(midY - kGraphLegendSwatchH * 0.5f);
                if (entries[i].dashed)
                {
                    // The same 6-on/5-off cell the in-flight wire uses, walked
                    // straight across the 18px rule -- the legend IS the key to
                    // that wire, so it cannot pick its own pattern.
                    float cx = x;
                    bool ink = true;
                    while (cx < x + kGraphLegendSwatchW)
                    {
                        const float step = (std::min)(ink ? kGraphDashOnPx : kGraphDashOffPx,
                                                      x + kGraphLegendSwatchW - cx);
                        if (ink)
                            dl->AddRectFilled(ImVec2(cx, y0),
                                              ImVec2(cx + step, y0 + kGraphLegendSwatchH), swatch);
                        cx += step;
                        ink = !ink;
                    }
                }
                else
                {
                    dl->AddRectFilled(ImVec2(x, y0),
                                      ImVec2(x + kGraphLegendSwatchW, y0 + kGraphLegendSwatchH),
                                      swatch);
                }
                x += kGraphLegendSwatchW + kGraphLegendSwatchGap;
                dl->AddText(ImVec2(x, midY - lineH * 0.5f), textCol, entries[i].text);
                x += ImGui::CalcTextSize(entries[i].text).x;
            }
            ImGui::PopFont();
        }
    }   // end anonymous namespace: DrawAssetGraphBody below is the one
        // exported entry point (Task 5, panel-split) -- everything above it
        // stays internal-linkage; an anonymous namespace's members remain
        // visible to code that follows it in the SAME enclosing scope (the
        // implicit using-directive), so the body below still reaches every
        // helper, constant and the `ed` alias unqualified. Same technique
        // AssetStatusPanel.cpp uses for DrawAssetStatusBody.

    // ---- Task 3: the Graph lens body (spec §10) ------------------------
    // Canvas foundation + layered nodes + two-layer kind-coloured edges,
    // plus (Task 4) the interaction surface: the selection bridge, the
    // peek tooltip with its edge summary, double-click open and the
    // unified context menu. The focus combo and the lens-strip mask are
    // Task 5's; the pin-drag "Derive Instance..." gesture, its dashed
    // in-flight wire and the canvas legend are Task 6's (section 7b and
    // the ghost menu in 8e).
    //
    // THE ONE RULE THE CREATE BRACKET CARRIES -- ed::EndCreate() is called
    // UNCONDITIONALLY -- is now a TYPE rather than a comment:
    // CanvasCreateScope (Widgets/CanvasEditScope.hpp), which also carries
    // the reason (CreateItemAction::Begin arms m_InActive even on the idle
    // frame, so a skipped End asserts on the NEXT frame's Begin -- the desk
    // crash that was fine on frame 1 and aborted on frame 2). That is also
    // the crash class the device-less test below the panel exists to keep
    // closed.
    void DrawAssetGraphBody(AssetGraphPanelState& state, AssetPanelModel& model,
                            const Arcane::Project* project, DocumentHost& docs,
                            const AssetPanelServices& services,
                            AssetPanelActions& actions)
    {
        // The boot-scene graphFocus seed (spec s10) is NOT run here -- Task 5
        // round 1 fix: DrawAssetGraphPanel (this file) calls
        // SeedAssetGraphFocus right after Begin, BEFORE its focus-combo
        // toolbar, so the combo and this body's own read of
        // `state.graphFocus` agree on the SAME frame a project opens. Seeding
        // it here instead ran it AFTER the toolbar had already read the
        // unseeded value that frame -- a one-frame "focus: everything" vs.
        // "focus: <boot scene>" split between the toolbar and the bottom bar.

        // ---- 1. Rebuild the projection, and ONLY when it moved --------
        // The trigger is AssetPanelModel::entriesStamp (bumped exactly
        // when the entries map or the reference index changed content)
        // plus the focus guid. Deliberately NOT RebuildIfDirty's return
        // value, which is also true for a rows-only rebuild -- a search
        // keystroke -- that the graph does not read. A per-frame rebuild
        // is not acceptable (it is a whole BFS + layering pass).
        if (!state.graphBuilt ||
            state.graphBuiltStamp != model.entriesStamp ||
            state.graphBuiltFocus != state.graphFocus)
        {
            GraphBuildInput in;
            in.entries = &model.Entries();
            in.index   = &model.RefIndex();
            in.focus   = state.graphFocus;
            state.graph.Build(in);
            state.graphBuilt      = true;
            state.graphBuiltStamp = model.entriesStamp;
            state.graphBuiltFocus = state.graphFocus;
            state.graphLayoutDirty = true;
        }

        // ---- 2. The canvas context, created lazily -------------------
        if (!state.graphCanvas)
        {
            ed::Config cfg;
            // Ruling 2: NO canvas persistence. The library would
            // otherwise write node positions to an ini of its own, and
            // spec §10 pins the layout as computed each build, never
            // persisted.
            cfg.SettingsFile = nullptr;
            // 2026-09-09 fix: without this the config falls through to
            // the vendored library's own default zoom table (0.1-8.0,
            // imgui_node_editor.cpp:3309-3312), and wheel-zooming this
            // canvas to its ceiling bilinearly magnifies the 12-14px
            // baked glyphs 8x -- unmistakable blur. ApplyZoomLevels
            // installs the same 20-stop table (0.1-2.0) the shader
            // editor's canvases use (Widgets/GraphZoomLevels.hpp), so
            // this canvas gets the same navigation feel and the same
            // 2.0x worst case. See docs/specs/
            // 2026-09-06-asset-manager-redesign-design.md §19.
            ApplyZoomLevels(cfg);
            state.graphCanvas = ed::CreateEditor(&cfg);
            // The style is per-context state, so a freshly created
            // context applies it -- including the switch that kills the
            // vendored grid.
            ed::SetCurrentEditor(static_cast<ed::EditorContext*>(state.graphCanvas));
            ApplyGraphCanvasStyle(AssetGraphCanvasStyleDesc());
            ed::SetCurrentEditor(nullptr);
            state.graphLayoutDirty = true;
        }
        ed::SetCurrentEditor(static_cast<ed::EditorContext*>(state.graphCanvas));

        // ---- 3. The backdrop, before ed::Begin ------------------------
        // The canvas rect is measured HERE because this is the one place
        // per frame that holds it BEFORE ed::Begin -- which is where
        // ScreenToCanvas still means what it says, and where a blit lands
        // under every channel the editor merges in. Both arguments, and the
        // disclosed one-frame view lag they buy, are written out once at
        // DrawGraphCanvasBackdrop (Widgets/GraphCanvasBackdrop.hpp).
        const ImVec2 canvasMin  = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x <= 0.0f || canvasSize.y <= 0.0f)
        {
            ed::SetCurrentEditor(nullptr);
            return;
        }

        DrawGraphCanvasBackdrop(canvasMin, canvasSize,
                                kGraphCanvasColor, kGraphGridMinorColor, kGraphGridMajorColor,
                                state.graphGrid);

        if (state.graph.nodes.empty())
        {
            // Still a live canvas (it pans and zooms) -- just an empty
            // one. Drawn into the window's own draw list, on top of the
            // backdrop and before ed::Begin, so it stays in SCREEN space
            // and does not scale away with the view.
            const char* msg = state.graphFocus.IsValid()
                                  ? "nothing references, and nothing is referenced by, the focused asset"
                                  : "no assets to graph";
            const ImVec2 size = ImGui::CalcTextSize(msg);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(canvasMin.x + (canvasSize.x - size.x) * 0.5f,
                       canvasMin.y + (canvasSize.y - size.y) * 0.5f),
                ImGui::GetColorU32(Theme::kTextDim), msg);
        }

        ed::Begin("##assetgraphcanvas", ImVec2(0.0f, canvasSize.y));

        const std::vector<GraphNode>& nodes = state.graph.nodes;
        const std::vector<GraphEdge>& edges = state.graph.edges;

        // Read here rather than at its point of use in section 4 (below)
        // because the id-resolution guard needs it: nothing between
        // ed::Begin and there writes `graphLayoutDirty`, so this is the
        // same value section 4 always saw. See section 4 for what it MEANS.
        const bool applyLayout = state.graphLayoutDirty;

        // ---- Task 4: the node under the cursor ------------------------
        // Latched by the PREVIOUS frame's ed::End
        // (imgui_node_editor.cpp:1278) -- and zeroed there whenever a
        // canvas action is running (`m_CurrentAction == nullptr`), so
        // neither the highlight below nor the tooltip after ed::End
        // flickers while the view is being panned or a node dragged. One
        // frame of lag on a hover is imperceptible; reading it HERE (and
        // once) is what lets the edge pass below act on the same answer
        // the tooltip does.
        const std::uint64_t hoveredNodeId = ed::GetHoveredNode().Get();

        // A node id -> the projection index it names, or `nodes.size()`
        // for "none" (id 0) and for a stale id left over from an earlier
        // build (ids are index+1 into the CURRENT build's node vector).
        //
        // THE STALENESS GUARD, AND WHY IT LIVES HERE (fix round 1). Every
        // id fed to this function was latched by the PREVIOUS frame's
        // ed::End -- the hovered node, the double-clicked node, the
        // context-menu node. On a rebuild frame those name the OLD node
        // vector, and an old id that happens to be in range for the new
        // one resolves to whatever asset now occupies that index: a
        // different document opened, the unified menu raised about (and
        // selecting) the wrong asset, the wrong peek rendered. Bounds
        // checking cannot catch that -- only knowing the ids are from a
        // previous generation can. So a rebuild frame resolves NOTHING,
        // once, at the single point where a latched id becomes an index,
        // rather than at each of the four call sites where the next one
        // added would forget. The cost is one frame of inert
        // hover/open/menu, which is exactly the (safe) shape of the
        // in-flight click 8c already drops for the same reason.
        const auto nodeIndexOf = [&nodes, applyLayout](std::uint64_t id) -> std::size_t
        {
            if (applyLayout)
                return nodes.size();
            return (id >= 1ull && id <= nodes.size())
                       ? static_cast<std::size_t>(id - 1ull)
                       : nodes.size();
        };
        // The guid a node id names for VISUAL purposes -- any non-overflow
        // node, tombstones included (a ghost's wires are exactly the "who
        // still points at this dead guid" answer a highlight is for).
        // Overflow companions are excluded because their guid ALIASES
        // their anchor's by construction (AssetGraphViewModel.hpp), so
        // lighting up "their" edges would light the ANCHOR's -- a lie
        // about what is under the cursor.
        const auto visualGuidOf = [&](std::uint64_t id) -> Arcane::Guid
        {
            const std::size_t i = nodeIndexOf(id);
            if (i == nodes.size() || nodes[i].isOverflow)
                return Arcane::Guid{};
            return nodes[i].guid;
        };
        // ...and the entry a node id names for ACTING purposes: select,
        // peek, open, context menu. Stricter than visualGuidOf on both
        // synthetic node kinds, and this is the single place that
        // decision is spelled:
        //   * OVERFLOW ("+N more") nodes are INERT in v1 (controller
        //     ruling). Their guid aliases the anchor's, so a bridge off
        //     one would silently select/open/menu the ANCHOR -- and
        //     "expand this +N more" is deliberately unspecified design
        //     territory, not something to invent from a draw path.
        //   * TOMBSTONES have no AssetPanelEntry by definition, so there
        //     is nothing for the entry-keyed menu or the open routing to
        //     act on, and nothing the peek tooltip's spec-s8 anatomy
        //     (thumb, kind pill, mount path, cook state) could honestly
        //     render. They stay interaction-inert too, selection
        //     included: `model.selected` is the ONE guid every lens and
        //     the Inspector point at, and aiming it at a guid no file
        //     backs would leave all of them pointing at nothing.
        const auto entryForNodeId = [&](std::uint64_t id) -> const AssetPanelEntry*
        {
            const Arcane::Guid g = visualGuidOf(id);
            return g.IsValid() ? model.Find(g) : nullptr;
        };

        // Real (non-overflow) guid -> node index. A tombstone counts as
        // real here: ruling 11 wants a dangling reference's edge to have
        // pixels, and `edges` references tombstones exactly like any
        // other node (AssetGraphViewModel.hpp's own `edges` comment).
        std::unordered_map<Arcane::Guid, std::size_t> indexOfGuid;
        indexOfGuid.reserve(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (!nodes[i].isOverflow)
                indexOfGuid.emplace(nodes[i].guid, i);

        // Which sides actually carry a drawn edge -- what decides both
        // whether a pin exists at all and whether its dot is filled.
        std::vector<GraphNodeVisual> visuals(nodes.size());
        for (const GraphEdge& e : edges)
        {
            const auto from = indexOfGuid.find(e.from);
            const auto to   = indexOfGuid.find(e.to);
            if (from == indexOfGuid.end() || to == indexOfGuid.end())
                continue;
            // `from` is the referencer: the edge leaves its OUTBOUND
            // (left) side. `to` is the target: it arrives on that node's
            // INBOUND (right) side.
            visuals[from->second].hasLeftPin   = true;
            visuals[from->second].leftConnected = true;
            visuals[to->second].hasRightPin    = true;
            visuals[to->second].rightConnected = true;
        }
        // A "+N more" companion means the anchor has undrawn connections
        // on that side, so the anchor keeps the pin even when no drawn
        // edge uses it -- hollow, because nothing is attached to it.
        for (const GraphNode& n : nodes)
        {
            if (!n.isOverflow)
                continue;
            const auto anchor = indexOfGuid.find(n.guid);
            if (anchor == indexOfGuid.end())
                continue;
            if (n.overflowInbound) visuals[anchor->second].hasRightPin = true;
            else                   visuals[anchor->second].hasLeftPin  = true;
        }
        // THE DERIVE AFFORDANCE (Task 6). A material's RIGHT (dependents)
        // pin is the handle the pin-drag gesture starts from, so a
        // material carries that pin even when nothing is attached to it --
        // which is exactly the material you most want to derive a first
        // instance from, and exactly what the board draws: OptionD's (and
        // Demo's) `reference_mesh` is a MATERIAL with no wires at all and
        // a single right-hand pin, wearing the drag's amber glow. Without
        // this the arc's signature gesture would be unreachable on any
        // material nothing references yet.
        //
        // The pin stays HOLLOW while nothing is attached (DrawGraphPinDot's
        // filled-vs-ring rule, unchanged) -- the board paints it solid, but
        // it paints it mid-drag; the ring/fill distinction is this lens's
        // own shipped language and one unconnected pin is not a reason to
        // drop it. Noted for the desk pass.
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (nodes[i].isOverflow || nodes[i].isTombstone)
                continue;
            if (const AssetPanelEntry* e = model.Find(nodes[i].guid))
                if (e->kind == AssetKind::Material)
                    visuals[i].hasRightPin = true;
        }

        const Arcane::Guid bootGuid = BootSceneGuid(project);
        const float nodeHeight = GraphNodeHeight();

        // ---- 4. Positions -- written on REBUILD, never per frame ------
        // Ruling 2: the computed layout is authoritative at every
        // rebuild, and the library's own node dragging stays enabled in
        // between. A reposition is therefore TRANSIENT BY DESIGN -- the
        // next rebuild snaps it back. That is intended behavior, not a
        // bug: spec §10 pins the layout as computed each build and not
        // persisted, so there is nowhere for a drag to live.
        //
        // `applyLayout` is read up at the id-resolution lambdas, which
        // need the same answer -- a frame that re-writes every node
        // position is exactly a frame whose incoming node ids are from
        // the previous generation.

        // ---- 5. Nodes -------------------------------------------------
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const GraphNode& n = nodes[i];
            const std::uint64_t nodeId = GraphNodeIdOf(i);
            GraphNodeVisual& v = visuals[i];

            const AssetPanelEntry* entry = n.isOverflow ? nullptr : model.Find(n.guid);

            // Body content + the width it wants.
            GraphNodeBody body;
            const char* headerIcon = nullptr;
            const char* headerPill = nullptr;
            std::string headerLabel = n.label;

            if (n.isOverflow)
            {
                // Ruling 6's "+N more": no pins, no accent, no thumb --
                // it is not an asset, it is a count of connections this
                // node's own breadth cap did not draw.
                headerIcon = ICON_LC_ELLIPSIS;
                body.meta  = n.overflowInbound ? "referencers not shown" : "references not shown";
            }
            else if (n.isTombstone)
            {
                // Ruling 11: a dangling target finally has pixels -- a
                // ghost node whose name is the short guid the view model
                // already chose for it, wearing the amber attention pill.
                headerIcon = ICON_LC_FILE_QUESTION;
                body.pills.push_back({ "missing", 1 });
            }
            else if (entry)
            {
                headerIcon = KindIcon(entry->kind);
                headerLabel = entry->fileName;
                body.thumb = services.resolveAssetThumb ? services.resolveAssetThumb(n.guid) : 0;
                body.icon  = KindIcon(entry->kind);
                // The same pill vocabulary the Browse rows use, in the
                // same spec order -- subkind, inst, sliced -- so one
                // asset reads identically in both lenses. "boot" moves to
                // the header, where the board puts it.
                if (const char* sub = SubkindPillText(*entry))
                    body.pills.push_back({ sub, 0 });
                if (entry->isInstance)
                    body.pills.push_back({ "inst", 0 });
                if (entry->kind == AssetKind::Sprite && entry->sliced)
                    body.pills.push_back({ "sliced", 0 });
                if (body.pills.empty())
                    body.meta = KindLabel(entry->kind);
                if (entry->kind == AssetKind::Scene && bootGuid.IsValid() && n.guid == bootGuid)
                    headerPill = "boot";
                if (entry->cook == CookState::Refused)
                    body.pills.push_back({ "refused", 1 });
                else if (entry->cook == CookState::Queued)
                    body.pills.push_back({ "queued", 0 });
            }
            else
            {
                headerIcon = KindIcon(n.kind);
                body.icon  = KindIcon(n.kind);
            }

            // Width: what the content wants, clamped into §11.2's band.
            float wantHeader = kGraphNodePadLeft + kGraphNodePadRight;
            {
                ImGui::PushFont(GetEditorFonts().interRegular, kGraphHeaderFontPx);
                if (headerIcon)
                    wantHeader += ImGui::CalcTextSize(headerIcon).x + kGraphNodeIconGap;
                wantHeader += ImGui::CalcTextSize(headerLabel.c_str()).x;
                ImGui::PopFont();
                if (headerPill)
                    wantHeader += kGraphNodeIconGap + PillWidth(headerPill);
            }
            float wantBody = kGraphNodePadLeft + kGraphNodePadRight;
            if (body.thumb != 0 || body.icon)
                wantBody += kAssetRowThumbSize + ImGui::GetStyle().ItemInnerSpacing.x;
            if (!body.pills.empty())
            {
                bool first = true;
                for (const auto& [text, variant] : body.pills)
                {
                    (void)variant;
                    wantBody += (first ? 0.0f : ImGui::GetStyle().ItemSpacing.x) + PillWidth(text);
                    first = false;
                }
            }
            else if (!body.meta.empty())
            {
                ImGui::PushFont(GetEditorFonts().interRegular, kGraphMetaFontPx);
                wantBody += ImGui::CalcTextSize(body.meta.c_str()).x;
                ImGui::PopFont();
            }

            v.width  = std::clamp((std::max)(wantHeader, wantBody),
                                  kGraphNodeMinWidth, kGraphNodeMaxWidth);
            v.height = nodeHeight;
            v.pos    = ImVec2(static_cast<float>(n.layer) * kGraphColumnPitch,
                              static_cast<float>(n.row)   * kGraphRowPitch);
            if (n.isOverflow)
            {
                v.hasLeftPin = v.hasRightPin = false;
                v.leftConnected = v.rightConnected = false;
            }

            if (applyLayout)
            {
                ed::SetNodePosition(ed::NodeId(nodeId), v.pos);
            }
            else
            {
                // The node may have been dragged since the last rebuild
                // (transient, but it has to draw where it IS). An id the
                // editor has never seen answers (FLT_MAX, FLT_MAX), which
                // would fling the node off the canvas -- fall back to the
                // computed layout for it instead.
                const ImVec2 live = ed::GetNodePosition(ed::NodeId(nodeId));
                if (live.x < FLT_MAX * 0.5f && live.y < FLT_MAX * 0.5f)
                    v.pos = live;
                else
                    ed::SetNodePosition(ed::NodeId(nodeId), v.pos);
            }

            // A tombstone wears the editor's amber attention language
            // rather than a kind accent it does not have -- ruling 11's
            // "kAmber border accent", applied to the bar and the border
            // alike (its AssetKind is Other by construction, so the §11.3
            // table has nothing to say about it either way).
            const ImVec4 amber  = Theme::kAmber;
            const ImVec4 dim    = Theme::kTextDim;
            const ImVec4 accent = GraphNodeAccentColor(n);
            const bool ghost = n.isOverflow || n.isTombstone;
            DrawGraphNode(nodeId, v, body, headerIcon, headerLabel, headerPill,
                          accent, ghost);

            // Chrome, after EndNode -- see DrawGraphNodeChrome.
            // Tombstones and refused cooks share amber; queued wears dim
            // so a cook-in-flight node is marked without looking broken.
            const ImVec4* borderAccent = nullptr;
            if (n.isTombstone)
                borderAccent = &amber;
            else if (entry && entry->cook == CookState::Refused)
                borderAccent = &amber;
            else if (entry && entry->cook == CookState::Queued)
                borderAccent = &dim;
            DrawGraphNodeChrome(nodeId, v,
                                /*drawBand=*/!n.isOverflow,
                                /*accent=*/n.isOverflow ? nullptr : &accent,
                                /*wash=*/ghost ? kGraphGhostWash : 0.0f,
                                borderAccent);
        }
        state.graphLayoutDirty = false;

        // ---- 6. Edges -------------------------------------------------
        // The two-layer trick (ruling 7): a FULLY TRANSPARENT ed::Link
        // carries hit-testing, selection, rect-select and the delete flow
        // (alpha 0 costs nothing -- the library's draw helper returns
        // immediately on it, and registration ignores colour entirely),
        // while the visible curve is drawn by hand into the links
        // channel. That is what per-kind colour, mid-edge labels and
        // selection brightening need; the library's flat uniform links
        // can do none of them. Widgets/GraphWire.hpp's kGraphLinkChannel
        // note is the long form of every clause in this paragraph.
        const float viewScale = GraphViewScale();
        // Ruling 9's mid-edge labels are dropped at LowDetail and below.
        // Written against the shared tier vocabulary rather than against a
        // copy of that tier's boundary number (Widgets/GraphNodeLod.hpp).
        //
        // The lookup carries a 1e-4 epsilon the bare `> 0.250f` compare did
        // not, which moves the cut by 0.0001. At every reachable zoom STOP
        // that is a no-op -- no entry in kZoomLevels lies in
        // (0.250, 0.2501]. But a stop is not the only scale this canvas can
        // sit at: ed::NavigateToSelection (section 8b) fits a rectangle and
        // lands on an arbitrary scale, and such a fit CAN land inside that
        // 1e-4 window, where labels now drop a hair earlier than they did
        // before the 2026-09-09 consolidation. Disclosed rather than
        // engineered around: the band is 0.04% of one zoom stop's width and
        // the labels in question are already at the edge of legibility.
        const bool  drawLabels = NodeLODForScale(viewScale) > NodeLOD::LowDetail;
        // Read-only: `selected` is a plain public member of the model, so
        // brightening needs no interaction plumbing at all. The rest of
        // the selection story -- clicking a node, centering on an
        // external change -- is section 8 below.
        const Arcane::Guid& selectedGuid = model.selected;
        // Task 4 / Interactions-FINAL: "the node's own edges brighten, the
        // rest stay dim" on HOVER as well, through this same one mechanism.
        const Arcane::Guid hoveredGuid = visualGuidOf(hoveredNodeId);

        for (std::size_t ei = 0; ei < edges.size(); ++ei)
        {
            const GraphEdge& e = edges[ei];
            const auto from = indexOfGuid.find(e.from);
            const auto to   = indexOfGuid.find(e.to);
            if (from == indexOfGuid.end() || to == indexOfGuid.end())
                continue;

            const GraphNodeVisual& fv = visuals[from->second];
            const GraphNodeVisual& tv = visuals[to->second];

            // The TARGET sits in the lower column, so its right-hand
            // (inbound) pin starts the wire and the REFERENCER's
            // left-hand (outbound) pin ends it -- see GraphLeftPinId.
            const std::uint64_t startPin = GraphRightPinId(GraphNodeIdOf(to->second));
            const std::uint64_t endPin   = GraphLeftPinId(GraphNodeIdOf(from->second));
            ed::Link(ed::LinkId(ei + 1), ed::PinId(startPin), ed::PinId(endPin),
                     ImVec4(0.0f, 0.0f, 0.0f, 0.0f), kGraphWireThickness);

            const ImVec2 p0(tv.pos.x + tv.width, tv.pos.y + tv.height * 0.5f);
            const ImVec2 p3(fv.pos.x,            fv.pos.y + fv.height * 0.5f);

            // Colour = a GRADIENT between the two endpoints' OWN accents
            // (user directive, 2026-09-09), superseding the earlier
            // single tone keyed off one end's kind. p0 is the TARGET's
            // right pin and p3 the REFERENCER's left pin, and each end
            // takes the very colour that pin already wears
            // (GraphNodeAccentColor -- so a tombstone end reads amber,
            // matching its dot), which is what makes a wire read
            // pin-hue -> pin-hue the way the shader graph's do.
            //
            // Emphasis is UNCHANGED -- same trigger, same two functions,
            // now simply applied to both ends instead of one: dimmed at
            // rest, brightened together when either endpoint is the
            // selected asset (spec §10: "selected node's edges brighten")
            // or the hovered one (Task 4). A same-kind edge still ends up
            // with two equal colours and takes the flat fast path, so this
            // costs nothing where there is no hue to travel.
            const bool emphasize = (selectedGuid.IsValid() &&
                                    (e.from == selectedGuid || e.to == selectedGuid)) ||
                                   (hoveredGuid.IsValid() &&
                                    (e.from == hoveredGuid || e.to == hoveredGuid));
            const auto endColor = [emphasize](const GraphNode& n)
            {
                const ImVec4 base = GraphNodeAccentColor(n);
                return emphasize ? GraphBrightenColor(base)
                                 : GraphDimColor(base, kGraphWireDim);
            };
            const ImVec2 mid = DrawGraphWire(p0, p3,
                                             endColor(nodes[to->second]),
                                             endColor(nodes[from->second]),
                                             kGraphWireThickness, viewScale);

            if (drawLabels && e.label)
            {
                // Ruling 9's mid-edge label, 12px and dim, on the small
                // plate the board gives it so the wire does not run
                // through the glyphs.
                ImGui::PushFont(GetEditorFonts().interRegular, kGraphLabelFontPx);
                const ImVec2 size = ImGui::CalcTextSize(e.label);
                const ImVec2 tl(mid.x - size.x * 0.5f, mid.y - size.y * 0.5f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (dl->_Splitter._Count > kGraphLinkChannel)
                {
                    const int prevChannel = dl->_Splitter._Current;
                    dl->ChannelsSetCurrent(kGraphLinkChannel);
                    dl->AddRectFilled(ImVec2(tl.x - 3.0f, tl.y), ImVec2(tl.x + size.x + 3.0f, tl.y + size.y),
                                      ImGui::GetColorU32(kGraphCanvasColor));
                    dl->AddText(tl, ImGui::GetColorU32(Theme::kTextDim), e.label);
                    dl->ChannelsSetCurrent(prevChannel);
                }
                ImGui::PopFont();
            }
        }

        // ---- 7. Anchor -> "+N more" connectors ------------------------
        // NOT a GraphEdge, and inexpressible as one: an overflow node
        // reuses its anchor's guid, and GraphEdge is guid-keyed, so
        // anchor -> companion would be a self-edge. The connector is
        // therefore synthesized here from `isOverflow` /
        // `overflowInbound` / the shared guid, drawn dim and thin so it
        // reads as "and more that way" rather than as a reference the
        // index actually holds. No ed::Link either -- there is nothing to
        // select, hover or delete.
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const GraphNode& n = nodes[i];
            if (!n.isOverflow)
                continue;
            const auto anchor = indexOfGuid.find(n.guid);
            if (anchor == indexOfGuid.end())
                continue;
            const GraphNodeVisual& av = visuals[anchor->second];
            const GraphNodeVisual& ov = visuals[i];
            // ONE tone at both ends, deliberately: this connector is
            // synthetic scaffolding, not a reference the index holds, so
            // it stays the subtle single grey it has always been and the
            // gradient the DATA edges gained above would misrepresent it.
            // Equal colours also mean it takes DrawGraphWire's flat
            // AddBezierCubic path -- unchanged paint, not merely a
            // gradient that happens to be constant.
            const ImVec4 col = GraphDimColor(Theme::kGrab, kGraphOverflowWireDim);
            if (n.overflowInbound)
                // Truncated on the anchor's INBOUND side: the companion
                // stacks one column to the RIGHT.
                DrawGraphWire(ImVec2(av.pos.x + av.width, av.pos.y + av.height * 0.5f),
                              ImVec2(ov.pos.x,            ov.pos.y + ov.height * 0.5f),
                              col, col, kGraphOverflowWireThickness, viewScale);
            else
                // Truncated on the anchor's OUTBOUND side: one column to
                // the LEFT. (When the anchor is already in column 0 the
                // view model clamps the companion into the SAME column,
                // and this connector doubles back on itself -- a layout
                // fact of the projection, drawn honestly rather than
                // hidden.)
                DrawGraphWire(ImVec2(ov.pos.x + ov.width, ov.pos.y + ov.height * 0.5f),
                              ImVec2(av.pos.x,            av.pos.y + av.height * 0.5f),
                              col, col, kGraphOverflowWireThickness, viewScale);
        }

        // ---- 7b. The pin-drag create query (Task 6) -------------------
        // THE ARC'S SIGNATURE GESTURE: drag off a material's DEPENDENTS
        // pin, release over empty canvas, get a ghost menu whose one entry
        // opens the Create dialog already parented to that material.
        //
        // WHICH PIN, geometrically (the vocabulary hazard, settled by the
        // board): a node's RIGHT pin is its inbound/referencers side --
        // wires EXIT right pins toward the assets that depend on this one
        // (see GraphLeftPinId's block). Deriving an instance MAKES a new
        // dependent, so the gesture is the material's RIGHT pin. Both
        // boards agree outright: OptionD/Demo's `reference_mesh` material
        // carries one pin at `right: -5px` with a
        // `box-shadow: 0 0 0 3px rgba(255,166,26,0.35)` amber glow, and
        // the dashed drag path leaves exactly that point.
        //
        // The gesture SPANS FRAMES (drag ... release ... popup open for as
        // long as the user leaves it up), so the only thing stashed is a
        // GUID -- never a pin or node id, which are index-derived and
        // renumber on every rebuild. A rebuild landing mid-drag cancels
        // the gesture: `nodeIndexOf` refuses every id on an applyLayout
        // frame, the query is rejected, and nothing is stashed.
        //
        // Three hops, copied in SHAPE from the shader editor's own
        // (ShaderEditorDocument.cpp:5510-5562 and :4019-4038): query and
        // accept HERE, inside the canvas; stash; open the popup in a
        // Suspend block (8e below), because a popup lives in screen space.
        bool wireCreateRequest = false;

        // A PIN id -> the projection index of the node that owns it, or
        // `nodes.size()`. Routed through nodeIndexOf so it inherits the
        // rebuild-staleness refusal in one place rather than re-deriving
        // it. Pin ids are kGraphPinIdBase + nodeId*4 + {1,2} and node ids
        // start at 1, so the smallest legal pin id is that base + 5 -- and
        // the base is what keeps a pin id from ever reading as a node id
        // (see GraphLeftPinId for why that matters). It subtracts out
        // before the arithmetic, which is otherwise unchanged.
        const auto pinNodeIndex = [&](std::uint64_t pinId) -> std::size_t
        {
            if (pinId < GraphLeftPinId(1ull))
                return nodes.size();
            return nodeIndexOf((pinId - kGraphPinIdBase) / 4ull);
        };
        const auto isRightPin = [](std::uint64_t pinId)
        {
            return pinId >= GraphLeftPinId(1ull) &&
                   ((pinId - kGraphPinIdBase) % 4ull) == 2ull;
        };
        // Remember WHICH asset the live drag is leaving, by guid. Silent on
        // a rebuild frame (nothing resolvable) and on a synthetic node --
        // in both cases the previous answer stands, which is right: the
        // drag did not change, only our ability to name it this frame.
        const auto noteDragSource = [&](std::uint64_t pinId)
        {
            const std::size_t i = pinNodeIndex(pinId);
            if (i == nodes.size() || nodes[i].isOverflow)
                return;
            state.graphDragGuid  = nodes[i].guid;
            state.graphDragRight = isRightPin(pinId);
        };

        // The colour handed to BeginCreate is the one the LIBRARY would
        // paint its own candidate link with -- fully transparent here, the
        // same two-layer trick section 6 uses for real edges, because the
        // visible in-flight curve is the hand-drawn dashed one below.
        //
        // The return value is also the honest "is a drag live at all"
        // answer: CreateItemAction reports true for every frame of a drag
        // (stage Possible) and for the release frame (stage Create), and
        // false once it is over -- which is what retires the curve.
        {
        // The bracket is a scope object now: ed::EndCreate() rides its
        // destructor, so it cannot be skipped on any path out of this block
        // (Widgets/CanvasEditScope.hpp holds the rule and the crash).
        const CanvasCreateScope create(ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
                                       kGraphWireThickness);
        if (create)
        {
            ed::PinId aId, bId;
            if (ed::QueryNewLink(&aId, &bId))
            {
                // Dragged onto another PIN. This lens never AUTHORS a
                // reference -- the graph is a projection of the reference
                // index, and a reference is made by editing an asset, not
                // by dragging a wire -- so the link is refused outright.
                // `aId` is always the DRAGGED pin (DragStart fills
                // m_LinkStart; DropPin only ever fills m_LinkEnd).
                noteDragSource(aId.Get());
                ed::RejectNewItem();
            }
            else if (ed::QueryNewNode(&aId))
            {
                // Dragged over EMPTY canvas. True on every frame of the
                // drag; AcceptNewItem returns true only on the RELEASE
                // frame (CreateItemAction::AcceptItem answers True only in
                // the Create stage), which is the one frame that stashes.
                noteDragSource(aId.Get());
                const std::size_t i = pinNodeIndex(aId.Get());
                if (i == nodes.size() || nodes[i].isOverflow)
                {
                    // Nothing to derive FROM: an overflow companion's guid
                    // aliases its anchor's, and an id nodeIndexOf refused
                    // is either out of range or from a previous build. A
                    // rebuild that lands exactly on the release frame
                    // therefore CANCELS the gesture rather than deriving
                    // from a stranger.
                    ed::RejectNewItem();
                }
                else if (ed::AcceptNewItem())
                {
                    const AssetPanelEntry* src = model.Find(nodes[i].guid);
                    // Derivable = a live MATERIAL, dragged off its
                    // DEPENDENTS pin. A tombstone has no entry, so it
                    // fails this by construction.
                    const bool derivable = isRightPin(aId.Get()) && src &&
                                           src->kind == AssetKind::Material;
                    if (derivable)
                    {
                        // Stash AND raise the menu together -- the guid
                        // named here is the one the popup (8f, below)
                        // will read back by the SAME field, so the two
                        // must agree on every frame the popup can open.
                        state.graphWireGuid      = nodes[i].guid;
                        state.graphWireDerivable = true;
                        wireCreateRequest = true;
                    }
                    else
                    {
                        // 2026-09 user ruling: a non-derivable release is
                        // a QUIET NO-OP, not a disabled menu -- "very
                        // confusing to have the same button show up if or
                        // if not the asset is derivable". No stash, no
                        // OpenPopup below; the dashed wire (drawn for
                        // every drag, derivable or not) simply ends here.
                        // Cleared the same way the gesture stash retires
                        // on a project switch, rather than left to dangle
                        // until the next successful drag overwrites it.
                        state.graphWireGuid      = Arcane::Guid{};
                        state.graphWireDerivable = false;
                    }
                }
            }
            // NO `else`: the pointer is over a NODE BODY, where the library
            // reports neither query. The drag is still live and
            // `graphDragGuid` still names its source, which is exactly why
            // that source is session state -- see its declaration.
        }
        else
        {
            // No create action at all: whatever drag there was is over
            // (released, cancelled, or consumed by the accept above one
            // frame ago). Retire the curve.
            state.graphDragGuid  = Arcane::Guid{};
            state.graphDragRight = false;
        }
        }   // ~CanvasCreateScope -> ed::EndCreate()

        // The in-flight curve, drawn AFTER EndCreate so it is back in
        // CANVAS space: QueryNewLink/QueryNewNode suspend the editor into
        // global (screen) space to answer, and CreateItemAction::End
        // resumes it. Which also means ImGui's mouse position is the
        // canvas-space one again out here -- the same space the pin pivots
        // below are in.
        //
        // The SOURCE is re-resolved from its guid through THIS build's
        // index map, so a rebuild mid-drag re-anchors the curve on the
        // node's new position instead of aiming it at whatever now sits at
        // an old index -- and a source that the rebuild dropped entirely
        // simply stops drawing.
        if (state.graphDragGuid.IsValid())
        {
            if (const auto it = indexOfGuid.find(state.graphDragGuid);
                it != indexOfGuid.end())
            {
                const GraphNodeVisual& wv = visuals[it->second];
                const bool right = state.graphDragRight;
                const ImVec2 pivot(right ? wv.pos.x + wv.width : wv.pos.x,
                                   wv.pos.y + wv.height * 0.5f);
                const ImVec2 tip = ImGui::GetMousePos();
                // Orientation matters: GraphWireControlPoints assumes p0
                // leaves rightward and p3 arrives leftward (the style's
                // SourceDirection/TargetDirection). A right-pin drag LEAVES
                // the pin; a left-pin drag ARRIVES at it. The board's path
                // (`M230,330 C320,330 390,402 462,402`) is the former.
                DrawGraphDashedWire(right ? pivot : tip, right ? tip : pivot,
                                    Theme::kAmber, kGraphWireThickness, viewScale);
                DrawGraphWireEndDot(tip, Theme::kAmber);
            }
        }

        // ---- 8. Interactions (Task 4) ---------------------------------
        // THE SELECTION AUTHORITY IS THE MODEL. The canvas keeps its own
        // selection set (it has to -- it draws the 2px selected border and
        // owns rect-select), but that set is never the truth: it is a
        // MIRROR the steps below keep in sync with the model, each ONLY on
        // the frame its own guard fires -- never unconditionally every
        // frame -- in a fixed order chosen so neither direction can read
        // back its own write.
        //
        //   8a rebuild guard  : node ids are index+1 into the CURRENT
        //                       build, so a rebuild renumbers everything
        //                       and the mirror now names strangers. Drop
        //                       it and re-derive it from the model.
        //   8b model -> canvas: a stamp nobody here acknowledged came from
        //                       somewhere else (a Browse row, the
        //                       Inspector, another lens). Point the mirror
        //                       at it and center ONCE -- and when the guid
        //                       has no node in this scope, CLEAR the
        //                       mirror rather than leave it pointing at
        //                       the previous asset. Acknowledge either
        //                       way, or the stamp re-arms forever.
        //   8c canvas -> model: only now, with the mirror known to agree
        //                       with the model, is a DISAGREEMENT
        //                       necessarily the user's own click. A pick
        //                       that resolves to a real entry pushes it
        //                       into the model and acknowledges the stamp
        //                       it raises in the same statement -- the
        //                       node is under the cursor already and must
        //                       not then be yanked to the middle of the
        //                       view by 8b on the next frame. A pick that
        //                       resolves INERT instead (a tombstone or an
        //                       overflow companion -- entryForNodeId
        //                       returns null) CLEARS the mirror, so a
        //                       ghost the model will never agree points
        //                       at anything stops wearing the 2px
        //                       selected border.
        //
        // The order is load-bearing and was caught by the device-less test
        // rather than reasoned out: with 8c first, a model selection that
        // is OUT of this scope leaves the mirror holding the previous
        // in-scope node, and 8c reads that stale mirror back over the
        // model -- the canvas silently out-voting the authority.
        //
        // Everything read here (the selection set, double-click, the
        // context-menu gesture) was decided by the PREVIOUS frame's
        // ed::End, which is where the library processes its actions. Same
        // position, same one-frame lag, as the shader editor's own read of
        // GetDoubleClickedNode (ShaderEditorDocument.cpp:3125-3129); the
        // alternative, reading after ed::End, cannot then call back INTO
        // the canvas at all.

        // 8a. Rebuild renumber guard.
        if (applyLayout)
        {
            ed::ClearSelection();
            if (model.selected.IsValid())
            {
                const auto it = indexOfGuid.find(model.selected);
                if (it != indexOfGuid.end())
                    ed::SelectNode(ed::NodeId(GraphNodeIdOf(it->second)));
            }
            // No navigation: the view did not RECEIVE a new selection, it
            // is the same one wearing a new id.
        }

        // 8b. Model -> canvas: center once on an externally-changed
        // selection (Browse's own idiom, `wantsScroll` at DrawTable).
        if (state.seenSelectionStampGraph != model.selectionStamp)
        {
            const auto it = model.selected.IsValid() ? indexOfGuid.find(model.selected)
                                                     : indexOfGuid.end();
            if (it != indexOfGuid.end())
            {
                ed::SelectNode(ed::NodeId(GraphNodeIdOf(it->second)));
                // Default zoomIn=false: NavigateTo's ZoomMode::None
                // centers at the CURRENT scale
                // (imgui_node_editor.cpp:3524-3532). Interactions-FINAL
                // says "the graph centers it" -- fitting one ~110x66 node
                // to the whole canvas instead would be a zoom nobody asked
                // for.
                ed::NavigateToSelection();
            }
            else
            {
                // Selected out of this scope (filtered by the focus, a
                // tombstone, never an asset at all): nothing to center on,
                // and the mirror must stop claiming the PREVIOUS node is
                // the selection -- see the ordering note above.
                ed::ClearSelection();
            }
            state.seenSelectionStampGraph = model.selectionStamp;
        }

        // 8c. Canvas -> model. Skipped on a rebuild frame: 8a just wrote
        // that mirror itself, so there is nothing of the user's in it --
        // and the ids in it would be the previous build's anyway, which
        // nodeIndexOf now refuses centrally. The explicit test stays
        // because it also short-circuits the canvas query.
        if (!applyLayout && ed::GetSelectedObjectCount() == 1)
        {
            // Exactly one object, and it has to be a NODE (a selected link
            // makes GetSelectedNodes return 0). A rect-select of several
            // nodes leaves the model alone on purpose: `model.selected` is
            // one guid, and silently picking one of N would be a guess.
            // Clicking empty canvas clears the CANVAS selection only --
            // deselecting inside one lens does not clear the selection
            // every other lens and the Inspector are pointing at, exactly
            // as clicking below the last Browse row does not.
            ed::NodeId picked;
            if (ed::GetSelectedNodes(&picked, 1) == 1)
            {
                if (const AssetPanelEntry* e = entryForNodeId(picked.Get()))
                {
                    if (e->guid != model.selected)
                    {
                        model.Select(e->guid);
                        state.seenSelectionStampGraph = model.selectionStamp;
                    }
                }
                else
                {
                    // Resolved INERT -- a tombstone (no AssetPanelEntry by
                    // construction) or an overflow companion (guid aliases
                    // its anchor's, ruled inert). Not applyLayout here (the
                    // outer guard above already excludes it), so this is a
                    // genuine resolution, not the rebuild guard's blanket
                    // refusal -- nodeIndexOf ran its real bounds check and
                    // the pick landed on a node that just has nothing to
                    // select. Clear the CANVAS's own mirror so the ghost
                    // stops wearing the 2px selected border while the
                    // model's selection (elsewhere, or nothing) is left
                    // untouched -- same "canvas-local, not the authority"
                    // posture as the empty-canvas click above.
                    ed::ClearSelection();
                }
            }
        }

        // 8d. Double-click opens, routed EXACTLY as a Browse row's is
        // (spec §6's verbatim-behavior clause): a scene comes back through
        // actions.openScene for the host's unsaved-changes guard, every
        // other kind opens through the DocumentHost. Overflow companions,
        // tombstones AND a rebuild frame's previous-generation ids are all
        // filtered by entryForNodeId -- the last of those matters most
        // here, since acting on a renumbered id would open a document the
        // user never double-clicked.
        if (const AssetPanelEntry* e = entryForNodeId(ed::GetDoubleClickedNode().Get()))
            OpenAssetRow(*e, project, docs, actions);

        // 8e. Right-click -> the unified asset context menu, the SAME
        // items a Browse row raises (DrawAssetMenuItems is the one copy).
        // `menuOpen` is read by the tooltip after ed::End -- see there.
        bool menuOpen = false;
        {
            const CanvasPopupScope canvasPopup;   // ed::Suspend/Resume, see the header
            ed::NodeId ctxNode;
            if (ed::ShowNodeContextMenu(&ctxNode))
            {
                if (const AssetPanelEntry* e = entryForNodeId(ctxNode.Get()))
                {
                    state.graphMenuGuid = e->guid;
                    // Right-click acts on this node -- DrawRowContextMenu's
                    // own first statement, for the same reason. The MIRROR
                    // moves with it: the context-menu action does not touch
                    // the canvas selection itself, so without this the next
                    // frame's 8c would read the old node back over the
                    // model and revert the right-click's selection.
                    // Acknowledged like 8c's click -- the node is under the
                    // cursor already, nothing to center.
                    ed::ClearSelection();
                    ed::SelectNode(ctxNode);
                    model.Select(e->guid);
                    state.seenSelectionStampGraph = model.selectionStamp;
                    ImGui::OpenPopup(kGraphNodeMenuId);
                }
                // No `else`: an overflow companion, a tombstone, or a
                // rebuild frame's previous-generation id raises no menu at
                // all rather than one about the wrong asset.
            }
            // Once OPEN the popup is already generation-proof: it re-reads
            // `state.graphMenuGuid`, a guid, never the id it came from.
            if (ImGui::BeginPopup(kGraphNodeMenuId))
            {
                menuOpen = true;
                if (const AssetPanelEntry* e = model.Find(state.graphMenuGuid))
                    DrawAssetMenuItems(actions, *e, /*kindSpecific=*/true, services);
                else
                    // The asset went away underneath an open menu (deleted
                    // on disk, or a rebuild dropped it): close rather than
                    // draw a menu about nothing.
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            // 8f. Task 6's ghost menu, opened in this SAME Suspend
            // bracket the shader editor opens its own wire-create popup
            // in (ShaderEditorDocument.cpp:4019-4038 -- one bracket, both
            // popups). ImGui records the popup's position from the mouse
            // AT OpenPopup TIME, and out here that is the SCREEN mouse,
            // which is why the menu lands at the drag's release point (the
            // board's `left: 474px; top: 380px` beside the curve's
            // `462,402` end) with no explicit placement call.
            //
            // 2026-09 user ruling (supersedes dev 11's disabled-not-hidden
            // call): `wireCreateRequest` is now raised ONLY on a derivable
            // release (the branch above), so a non-derivable release opens
            // NOTHING here -- the dashed wire (drawn for every drag,
            // derivable or not, above) simply ends and the gesture reads
            // as a quiet no-op rather than a button that shows up disabled.
            if (wireCreateRequest)
                ImGui::OpenPopup(kGraphCreateMenuId);
            if (ImGui::BeginPopup(kGraphCreateMenuId))
            {
                menuOpen = true;
                // Generation-proof for the same reason the node menu is:
                // it re-reads a stashed GUID, never the pin id it came
                // from. If the asset went away underneath an open menu
                // (deleted on disk, a rebuild dropped it) the entry simply
                // goes dead rather than promising a parent that is gone --
                // the ONE reason left to disable rather than close, now
                // that non-derivable releases never reach this popup at
                // all. (`state.graphWireDerivable` is therefore always
                // true for as long as this popup can be open: it and
                // `graphWireGuid` are stashed together on the SAME frame,
                // above, and nothing else writes either while the popup
                // stands.)
                const AssetPanelEntry* src = model.Find(state.graphWireGuid);
                ImGui::BeginDisabled(!src);
                if (ImGui::MenuItem(ICON_LC_LAYERS " Derive Instance\xE2\x80\xA6"))
                {
                    // THE FIRST REAL PRODUCER of the createPrefillParent
                    // limb. Routed through the ONE unified-create request
                    // every other creation path uses (spec §7: "no
                    // creation path may bypass CreateAssetRequest") --
                    // EditorAppFrame.cpp:2328-2334 turns the pair into
                    // BeginCreateAsset({MaterialInstance, parent}), whose
                    // MaterialInstance arm (:2490-2499) lands the guid in
                    // the dialog's `parent` field and leaves the picker
                    // CLOSED because the parent is already known.
                    //
                    // The kind is a CreateAssetKind, per the field's own
                    // contract -- and it is MaterialInstance outright, not
                    // a bridged source kind: CreateKindForAssetKind maps a
                    // material to CreateAssetKind::Material (the thing the
                    // source IS), while this entry creates the thing that
                    // DERIVES from it.
                    actions.requestCreateKind =
                        static_cast<int>(CreateAssetKind::MaterialInstance);
                    actions.createPrefillParent = state.graphWireGuid;
                }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
        }

        ed::End();
        ed::SetCurrentEditor(nullptr);

        // ---- The canvas legend (Task 6, controller rider) -------------
        // AFTER ed::End, so it is chrome in SCREEN space: it does not pan,
        // zoom, or sort against the nodes. See DrawGraphLegend for the
        // board transcription and the two flagged mismatches.
        DrawGraphLegend(canvasMin, canvasSize);

        // ---- 9. The peek tooltip (Task 4, plan ruling 14) -------------
        // WHERE it landed, and why HERE:
        //
        //  * AFTER ed::End, and outside the editor entirely. Inside
        //    ed::Begin/End the editor has moved ImGui into the canvas's
        //    transformed (pan+zoom) space, and a tooltip positions itself
        //    in SCREEN space -- CanvasPopupScope.hpp states the rule. Out
        //    here ImGui is already back in screen space (Canvas::End ->
        //    LeaveLocalSpace, imgui_canvas.cpp), so no Suspend bracket is
        //    needed at all; that the editor is not even current any more
        //    is the proof. This is "do not fight the canvas": the one
        //    thing the tooltip needs from the canvas -- WHICH node is
        //    hovered -- was captured up top, so the drawing needs nothing
        //    else from it.
        //
        //  * `forceShow=true`. ImGui's "last submitted item" at this point
        //    is the canvas's own full-rect Dummy (imgui_canvas.cpp:182),
        //    never the hovered node, so IsItemHovered() would answer a
        //    question about the CANVAS. That is precisely the situation
        //    the parameter was added for in Plan 2 Task 8 (TimelineFeed);
        //    the node editor's own hit test is the honest authority here.
        //
        //  * ...which costs the ForTooltip delay, so the dwell below
        //    replaces it: the same node must stay hovered for
        //    style.HoverStationaryDelay before the peek appears. Without
        //    it a tooltip would flash on every node the pointer crosses on
        //    its way somewhere -- a peek, not a commit. The node editor
        //    already suppresses hover outright while an action is running,
        //    which covers the drag/pan half of spec §8's tooltip rules.
        //
        // Overflow companions, tombstones and a rebuild frame's
        // previous-generation ids get no peek (entryForNodeId), and
        // neither does a node while EITHER canvas popup is up -- its own
        // context menu (8e) or Task 6's ghost create menu (8f): the peek
        // and the menu are two answers to one hover, and ImGui would stack
        // them at the same mouse position. Tracked through `menuOpen`
        // rather than IsPopupOpen because the popup's id was hashed
        // against the ID stack ed::Begin pushes, which is gone by here.
        //
        // The dwell is keyed on the resolved GUID, not on the node id it
        // came from (fix round 1). A guid is generation-independent, so
        // "is this still the same thing I was hovering?" stays a true
        // question across a rebuild -- whereas an id compares numerically
        // equal while the asset behind it changes, which would have
        // silently carried an elapsed dwell onto a different asset.
        const AssetPanelEntry* hovered = menuOpen ? nullptr : entryForNodeId(hoveredNodeId);
        if (hovered)
        {
            if (state.graphHoverGuid != hovered->guid)
            {
                state.graphHoverGuid    = hovered->guid;
                state.graphHoverSeconds = 0.0f;
            }
            else
            {
                state.graphHoverSeconds += ImGui::GetIO().DeltaTime;
            }
            if (state.graphHoverSeconds >= ImGui::GetStyle().HoverStationaryDelay)
                DrawAssetPeekTooltip(model, services, hovered->guid,
                                     /*forceShow=*/true, /*withEdgeSummary=*/true);
        }
        else if (!applyLayout)
        {
            // Nothing hovered -> drop the dwell. NOT on a rebuild frame
            // though: there the ids are merely unreadable for one frame,
            // which is not evidence the pointer left the node. Holding the
            // dwell is what makes the guid key pay -- the peek pauses for
            // that frame and resumes on the next one instead of making the
            // user wait out the delay again for a node they never left.
            state.graphHoverGuid    = Arcane::Guid{};
            state.graphHoverSeconds = 0.0f;
        }
    }

    // ---- Panel-split Task 7: the window (spec s5/s9) -------------------
    AssetPanelActions DrawAssetGraphPanel(AssetGraphPanelState& state, AssetPanelModel& model,
                                          const Arcane::Project* project, DocumentHost& docs,
                                          const AssetPanelServices& services,
                                          bool* open)
    {
        AssetPanelActions actions;

        // THE SEED RUNS BEFORE Begin, not after it -- deliberately, and this
        // is the ONE thing in this function that is not inside the window.
        //
        // Task 5 round 1 fix established that the boot-scene graphFocus seed
        // must precede the toolbar: the focus combo reads `state.graphFocus`
        // this same frame, and so does the bottom bar after the body, so a
        // later seed (the first cut put it inside DrawAssetGraphBody's own
        // preamble, which runs AFTER the toolbar) split the two bands for one
        // frame -- "focus: everything" up top, "focus: <boot scene>" below.
        //
        // Task 7 adds a SECOND reason it cannot sit below the collapse
        // early-out. `graphFocusSeeded` is a once-per-project latch, and the
        // early-out fires on every frame this window is a docked-but-unselected
        // tab -- which the SHIPPED DEFAULT LAYOUT makes the normal state (Asset
        // Browser is the selected tab, spec s10). The latch would then still be
        // unspent when the user clicks Status's "Focus in Graph": the host
        // writes graphFocus and calls SelectDockTab, the tab comes forward, and
        // the very first frame that draws would seed the BOOT SCENE straight
        // over the scene the user just asked for. Seeding here spends the latch
        // on the panel's first HOSTED frame instead of its first DRAWN one --
        // matching the pre-split shell, which ran the seed unconditionally
        // after a Begin it never checked. Safe above Begin because the seed
        // touches no ImGui state at all (a guid, a bool, and a manifest read).
        SeedAssetGraphFocus(state, project);

        if (!ImGui::Begin("Asset Graph", open))
        {
            // Collapsed, or a docked tab that is not the selected one:
            // ImGui has skipped this window's contents entirely. End is
            // still owed (Begin/End pair unconditionally).
            ImGui::End();
            return actions;
        }

        // ---- toolbar band: the focus combo, and nothing else ----------
        // Spec s9.1 (R2, minimal): no Create and no search here -- the
        // Browser owns both, and the menubar's Assets > Create submenu keeps
        // create reachable when only this window is open. Left-anchored,
        // because with the lens strip gone there is no right-hand band left
        // for it to flex against.
        //
        // Scope the graph to ONE scene, or to "everything" (ruling 6's nil
        // focus). Writing state.graphFocus is all this takes: the body's own
        // dirty check compares graphBuiltFocus and rebuilds the projection,
        // so there is no rebuild call to make here.
        {
            ImGuiStyle& style = ImGui::GetStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(style.FramePadding.x, kAssetPanelToolbarFramePadY));

            ImGui::SetNextItemWidth(kGraphFocusComboWidth);
            // "focus: <name>" -- the BOARD's exact preview string
            // (`OptionD.dc.html`: `<span>focus:</span> main.arcscene`), per
            // the controller's board-strings-win ruling. The board paints its
            // "focus:" half in kTextDim and the name in kText; BeginCombo's
            // preview is a single string in a single colour, so the two-tone
            // half of that is not expressible here without replacing the
            // combo with a hand-drawn widget -- not invented, see the Task 5
            // fix report.
            char focusPreview[160];
            std::snprintf(focusPreview, sizeof(focusPreview), "focus: %s",
                          GraphFocusLabel(model, state.graphFocus));
            if (ImGui::BeginCombo("##graphfocus", focusPreview))
            {
                if (ImGui::Selectable(kGraphFocusEverything, !state.graphFocus.IsValid()))
                    state.graphFocus = Arcane::Guid{};
                // PushID per row, keyed by the guid: two scenes may share a
                // stem ("main.arcscene" in two folders), and ImGui would
                // otherwise give both Selectables the SAME id -- clicking
                // either would activate the first.
                for (const AssetPanelEntry* s : ScenesByName(model))
                {
                    ImGui::PushID(s->guid.ToString().c_str());
                    // fileName for the same reason GraphFocusLabel uses it:
                    // this list and the Status panel's scene cards are the
                    // same scenes, and they read identically there.
                    if (ImGui::Selectable(s->fileName.c_str(), s->guid == state.graphFocus))
                        state.graphFocus = s->guid;
                    ImGui::PopID();
                }
                const std::vector<const AssetPanelEntry*> sources = SourcesByName(model);
                if (!sources.empty())
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("Source");
                    for (const AssetPanelEntry* s : sources)
                    {
                        ImGui::PushID(s->guid.ToString().c_str());
                        if (ImGui::Selectable(s->fileName.c_str(), s->guid == state.graphFocus))
                            state.graphFocus = s->guid;
                        ImGui::PopID();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::PopStyleVar();
        }

        // The 7px toolbar->body seam, stated outright rather than left to
        // ImGui's automatic per-item spacing -- see DrawAssetBrowserPanel's
        // own comment (AssetBrowserPanel.cpp) for the mock-parity fix this
        // came from, and kAssetPanelToolbarBodyGapPx for the measured value.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, kAssetPanelToolbarBodyGapPx));
        ImGui::PopStyleVar();

        // ---- body band -----------------------------------------------
        constexpr float kSelectionStripH = 52.0f;
        if (ImGui::BeginChild("##assetgraphbody",
                              ImVec2(0.0f, -(kAssetPanelBottomBarHeight + kSelectionStripH))))
        {
            if (!project)
                DrawAssetPanelNoProjectMessage();
            else
                DrawAssetGraphBody(state, model, project, docs, services, actions);
        }
        ImGui::EndChild();

        // Selection strip: the Browser preview's facts (thumb, name, cook)
        // when this window is the one that holds the selection -- Graph-only
        // layouts otherwise had only the peek tooltip.
        {
            if (ImGui::BeginChild("##graphsel", ImVec2(0.0f, kSelectionStripH), ImGuiChildFlags_Borders))
            {
                const AssetPanelEntry* e = model.selected.IsValid() ? model.Find(model.selected) : nullptr;
                if (!e)
                    ImGui::TextDisabled("No selection");
                else
                {
                    const float thumb = 36.0f;
                    const ImVec2 thumbMin = ImGui::GetCursorScreenPos();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const std::uint64_t tex = services.resolveAssetThumb
                        ? services.resolveAssetThumb(e->guid) : 0;
                    if (tex != 0)
                        dl->AddImage(static_cast<ImTextureID>(tex), thumbMin,
                                     ImVec2(thumbMin.x + thumb, thumbMin.y + thumb));
                    else
                    {
                        dl->AddRectFilled(thumbMin, ImVec2(thumbMin.x + thumb, thumbMin.y + thumb),
                                          ImGui::GetColorU32(Theme::kWell));
                        const char* icon = KindIcon(e->kind);
                        const ImVec2 ks = ImGui::CalcTextSize(icon);
                        dl->AddText(ImVec2(thumbMin.x + (thumb - ks.x) * 0.5f,
                                           thumbMin.y + (thumb - ks.y) * 0.5f),
                                    ImGui::GetColorU32(ImGuiCol_Text), icon);
                    }
                    ImGui::Dummy(ImVec2(thumb, thumb));
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::TextUnformatted(e->fileName.c_str());
                    if (e->cook == CookState::Refused)
                        ImGui::TextColored(Theme::kAmber, "%s", CookStateLabel(e->cook));
                    else
                        ImGui::TextDisabled("%s", CookStateLabel(e->cook));
                    ImGui::EndGroup();
                    ImGui::SameLine();
                    if (project && ImGui::Button("Open"))
                        OpenAssetRow(*e, project, docs, actions);
                }
            }
            ImGui::EndChild();
        }

        // ---- bottom bar band (spec s9.2) -----------------------------
        // LEFT: what the SCOPED projection is showing out of the whole
        // project, plus the scope root itself. N is realNodeCount, which
        // counts real ASSET nodes only (ruling 12: neither the synthetic
        // "+N more" companions nor tombstones are assets). The wording is the
        // BOARD's, verbatim (`OptionD.dc.html`: `6 of 15 assets &middot;
        // focus: main.arcscene`).
        //
        // THE GATE, and what remains of it (spec s7.4). Pre-split this line
        // asked AssetsGraphProjectionIsCurrent -- a three-conjunct predicate
        // guarding against a frame where the bar drew for the Graph lens
        // while the BODY drawn that frame was another lens's, leaving the
        // previous build's count paired with a new focus name. That frame is
        // unrepresentable now: this bar draws inside this panel's own
        // Begin/End, immediately after this panel's own body, so the
        // projection beside it is always the one just rebuilt. What survives
        // is the ONE honest unknown -- `graphBuilt` false, i.e. the canvas
        // was never opened (or was just torn down at a project switch), so
        // there is no projection behind the number at all. Spec s13: the bar
        // renders that as an em dash and never as a fabricated 0.
        {
            const AssetPanelBottomBar bar = BeginAssetPanelBottomBar("##assetgraphbottombar");
            if (bar.visible)
            {
                const HealthCounts health = model.Health();
                char shown[16];
                if (state.graphBuilt)
                    std::snprintf(shown, sizeof(shown), "%d", state.graph.realNodeCount);
                else
                    std::snprintf(shown, sizeof(shown), "\xE2\x80\x94");   // U+2014 EM DASH
                // 128, not 64: the form below embeds a SCENE NAME, and a real
                // one ("prototype_courtyard_lighting") overflows 64 on its own
                // -- snprintf would then truncate mid-name with no other symptom.
                char left[128];
                std::snprintf(left, sizeof(left), "%s of %d assets \xC2\xB7 focus: %s",
                              shown, health.total,
                              GraphFocusLabel(model, state.graphFocus));
                ImGui::TextUnformatted(left);

                DrawAssetPanelHealthDigest(bar, model, services, actions);
            }
            EndAssetPanelBottomBar();
        }

        ImGui::End();
        return actions;
    }

    void DestroyAssetGraphPanelCanvas(AssetGraphPanelState& state)
    {
        if (state.graphCanvas)
        {
            ed::DestroyEditor(static_cast<ed::EditorContext*>(state.graphCanvas));
            state.graphCanvas = nullptr;
        }
        // Everything derived from the context or the outgoing project goes
        // with it: a stale projection would otherwise be re-drawn (against
        // brand-new node ids) on the first frame after a project switch,
        // before the model has rebuilt.
        state.graph.Clear();
        state.graphBuilt = false;
        state.graphBuiltStamp = 0;
        state.graphBuiltFocus = Arcane::Guid{};
        state.graphLayoutDirty = false;
        state.graphFocus = Arcane::Guid{};
        // ...and re-arm the boot-scene seed with it (Task 5): the incoming
        // project has its OWN boot scene, and this is the seam that tells the
        // panel a new one is coming. Clearing the focus without clearing this
        // flag would leave the next project permanently scoped to
        // "everything"; clearing this flag without clearing the focus would
        // leave the outgoing project's scene guid readable for one frame.
        state.graphFocusSeeded = false;
        state.graphGrid = GraphGridPhase{};
        state.seenSelectionStampGraph = 0;
        // Task 4's interaction state is derived from the context and the
        // projection too: a node id (the hover dwell) means nothing once the
        // ids are gone, and a menu guid names an asset of the OUTGOING
        // project. Leaving either behind would let the first frame of the
        // next project answer with the last one's.
        state.graphMenuGuid = Arcane::Guid{};
        state.graphHoverGuid = Arcane::Guid{};
        state.graphHoverSeconds = 0.0f;
        // Task 6's gesture stash goes with them, and for the same reason: it
        // names an asset of the OUTGOING project, and the popup it feeds is
        // closed by the context's destruction anyway.
        state.graphWireGuid = Arcane::Guid{};
        state.graphWireDerivable = false;
        state.graphDragGuid = Arcane::Guid{};
        state.graphDragRight = false;
    }
}

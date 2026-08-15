#pragma once

// ImGuiNriNode -- the graph node that draws the HUD, and the LAST visual
// writer of the `--nri-graph` frame (NRI Phase 2, Task 12).
//
// The frame's tail is
//
//     ... -> tonemap [-> outlinecomposite] -> imgui [-> capture] -> present
//
// and that ORDER IS THE MECHANISM: declaration order is execution order on
// this graph, so ImGui composites onto the display-referred backbuffer after
// the tonemap (and after the selection outline, when a probe run armed it)
// and before the capture node reads it. It mirrors what the NVRHI host does
// -- RuntimeApp records `pass:imgui` after `pass:tone`, into the same
// backbuffer framebuffer, and the screenshot/golden readback happens after
// both.
//
// THE OBJECT HERE OWNS ALMOST NOTHING. Everything with a GPU lifetime lives
// in ImGuiNri (Arcane/ImGui/ImGuiNri.hpp) -- the §7 "PORT OURS" backend that
// mirrors ImGuiNvrhi's surface. This class is the graph glue: it holds one
// ImGuiNri, resolves the vehicle's imgui_vs/imgui_ps bins into it at Create,
// and splits the backend's work across the two phases the graph has:
//
//   * DECLARATION time -- PrepareFrame() runs the 1.92 texture protocol
//     (create/update/destroy of the font atlas and any user texture) through
//     nri::HelperInterface::UploadData, which SUBMITS AND WAITS and therefore
//     must never be reached from inside the frame's open command buffer. Same
//     placement, and the same reason, as Batch2DNode::Prepare and
//     PostChainNode::PrepareChain.
//   * RECORD time -- Record() copies this frame's vertex/index data into the
//     upload ring and issues one CmdDrawIndexed per ImDrawCmd. Nothing
//     retains a pointer into ImGui's draw lists past that call.
//
// NO POOL-EPOCH DISCIPLINE HERE, and its absence is deliberate rather than an
// omission: FullscreenNodes' SyncPoolEpoch exists because those nodes cache
// SHADER_RESOURCE views over graph TRANSIENTS, which RealizePool may destroy
// from inside Execute(). This node samples no transient at all. The font
// atlas is node-owned (created by ImGuiNri, buried by it), a user texture is
// the host's, and the backbuffer is an executor-provided ATTACHMENT that the
// graph re-views every Execute. There is no cached view over a pool texture
// to invalidate, so there is nothing for an epoch check to do.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/ImGui/ImGuiNri.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>

#include <cstdint>
#include <memory>

struct ImDrawData;

namespace Arcane
{
    class Graveyard;
    class NriGraphContext;

    class ARCANE_API ImGuiNriNode
    {
    public:
        // Loads imgui_vs/imgui_ps through the vehicle and builds the backend's
        // sampler, layout and descriptor pool. Null (already logged + latched)
        // on failure -- built eagerly at Create like every other node, so a
        // HUD that cannot be drawn fails the vehicle at boot instead of
        // silently rendering nothing.
        static std::unique_ptr<ImGuiNriNode> Create(NriGraphContext& context);

        // SAFETY NET, NOT THE PATH -- see Batch2DNode's matching comment.
        ~ImGuiNriNode() = default;

        ImGuiNriNode(const ImGuiNriNode&)            = delete;
        ImGuiNriNode& operator=(const ImGuiNriNode&) = delete;

        // Buries every NRI object the backend owns at `fence` and empties it.
        // Idempotent.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // DECLARATION-time half: the 1.92 texture protocol. See the header
        // block -- this submits and waits, so it must not be reached from an
        // exec fn. `fence` is what destroyed textures are buried at (the
        // graph's own last submitted value).
        void PrepareFrame(ImDrawData* drawData, std::uint64_t fence);

        // RECORD-time half: records `drawData` into an ALREADY-OPEN raster
        // pass whose single colour attachment is `target`. Emits no barrier.
        void Record(RenderGraphNodeContext& context, ImDrawData* drawData, RgTexture target);

    private:
        ImGuiNriNode() = default;

        ImGuiNri m_renderer;
    };

    // Declares the ImGui node into `graph`: reads nothing, writes `target`
    // (the imported backbuffer the tonemap has already written) with the SAME
    // ColorWrite state -- so the compile derives no transition between them,
    // which is both correct and what NVRHI's own state tracker does
    // (requireTextureState skips a barrier when before == after for a non-UAV
    // state). The blend reads the destination through the ROP, which
    // AccessBits::COLOR_ATTACHMENT already covers.
    //
    // Also runs the node's DECLARATION-time texture prepare, for the same
    // reason AddPickNodes builds its geometry here: this is the last point
    // before the frame's command buffer opens.
    //
    // `context` may be null -- see AddBatch2DNode's signature note; a null
    // context is the headless declaration-shape drive the [nri] frame-shape
    // cases use, where every declaration is identical and only the exec fn is
    // inert.
    ARCANE_API void AddImGuiNode(RenderGraph& graph, NriGraphContext* context, RgTexture target);
}

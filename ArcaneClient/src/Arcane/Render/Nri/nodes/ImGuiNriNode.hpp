#pragma once

// ImGuiNriNode -- the graph node that draws the HUD, and the LAST visual
// writer of the frame.
//
// The frame's tail is
//
//     ... -> tonemap [-> outlinecomposite] -> imgui [-> capture] -> present
//
// and that ORDER IS THE MECHANISM: declaration order is execution order on
// this graph, so ImGui composites onto the display-referred backbuffer after
// the tonemap (and after the selection outline, when a probe run armed it)
// and before the capture node reads it -- which is why a screenshot carries
// the chrome that was actually shown.
//
// THE OBJECT HERE OWNS ALMOST NOTHING. Everything with a GPU lifetime lives
// in ImGuiNri (Arcane/ImGui/ImGuiNri.hpp) -- the §7 "PORT OURS" first-party
// backend. This class is the graph glue: it holds one
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

        // Point this backend at the ImGui context whose draw data it will be
        // handed -- see ImGuiNri::AdoptContext for why Create() cannot always
        // work it out for itself. Call once, right after Create, for any node
        // that serves a context OTHER than the one current at Create time (the
        // editor's game-UI node). Idempotent and null-safe.
        void AdoptImGuiContext(void* imguiContext) { m_renderer.AdoptContext(imguiContext); }

        // DECLARATION-time half: the 1.92 texture protocol. See the header
        // block -- this submits and waits, so it must not be reached from an
        // exec fn. `graveyard`/`fence` are where and at what value destroyed
        // textures are buried: the OWNING CONTEXT's lane
        // (NriGraphContext::Graves()) and its graph's own last submitted value.
        void PrepareFrame(ImDrawData* drawData, Graveyard& graveyard, std::uint64_t fence);

        // THE USER-TEXTURE INVALIDATION HOOK, both variants forwarded. Call
        // one whenever a texture this backend may
        // have drawn is about to be destroyed -- the editor's offscreen
        // viewport output above all, whose replacement NRI may hand the address
        // the destroyed one just vacated.
        //
        // WHICH ONE: `Now` for a texture ANOTHER context owns. It destroys the
        // view inside the call, behind its own idle, so calling it BEFORE the
        // owner's destroy keeps the view ahead of the texture it views -- there
        // is no ordering between two lanes to lean on instead. The DEFERRED one
        // is only for a texture that dies in `graveyard` itself, after this
        // call. ImGuiNri's declarations carry the full contract; the exact call
        // the viewport owes sits on NriGraphContext::ResizeOffscreen.
        //
        // NOTE WHOSE NODE THIS IS: the one belonging to the context that DRAWS
        // the texture (the editor's chrome/host-window context), not the
        // offscreen context that owns it. They are different objects with
        // different lanes -- which is the whole reason `Now` exists.
        bool InvalidateUserTexture(nri::Texture* texture, Graveyard& graveyard,
                                   std::uint64_t fence);
        bool InvalidateUserTextureNow(nri::Texture* texture);

        // RECORD-time half: records `drawData` into an ALREADY-OPEN raster
        // pass whose single colour attachment is `target`. Emits no barrier.
        void Record(RenderGraphNodeContext& context, ImDrawData* drawData, RgTexture target);

        // The backend itself, for the [nri] cases' introspection
        // (LiveTextureCount/HasEntryFor/RetiredSetCount) and for a host that
        // wants to PRE-WARM a user texture's binding outside a frame. Not part
        // of any cross-task contract.
        [[nodiscard]] ImGuiNri& Renderer() noexcept { return m_renderer; }

    private:
        ImGuiNriNode() = default;

        ImGuiNri m_renderer;
    };

    // WHICH ImGui node a declaration means, and therefore which backend and
    // which of the frame's two ImDrawData it draws.
    //
    // They are TWO NODES over TWO BACKENDS rather than one node drawn twice,
    // and that is structural rather than tidy: an ImGuiNri caches the 1.92
    // texture protocol's ImTextureData-owned textures for ONE context's atlas,
    // and the two draw datas here come from two different ImGui contexts (the
    // host's chrome context, and the editor's game/plugin context -- whose
    // io.IniFilename is deliberately null where the editor's is the per-project
    // layout file). One backend serving both would interleave two atlases in
    // one pointer-keyed cache against a fixed kMaxTextures.
    enum class ImGuiNodeSlot : std::uint8_t
    {
        // The HOST CHROME hud, `FrameDesc::imgui`, declared LAST -- after the
        // tonemap, after the outline composite, before the capture.
        HostHud,
        // The GAME's own HUD, `FrameDesc::gameUi`, declared between the tonemap
        // and the pick/outline chain: the editor's phase 11 (the game HUD)
        // then phase 12 (the selection outline) order, expressed against this
        // recorder. The two are mutually exclusive by MODE in the editor (Play
        // draws the HUD, Edit draws the outline), so the order is a fidelity
        // guarantee rather than something a frame exercises today.
        GameUi,
    };

    // Declares the ImGui node into `graph`: reads nothing, writes `target`
    // (the imported backbuffer the tonemap has already written) with the SAME
    // ColorWrite state -- so the compile derives no transition between them,
    // which is correct: a barrier between equal non-UAV states is a no-op.
    // The blend reads the destination through the ROP, which
    // AccessBits::COLOR_ATTACHMENT already covers.
    //
    // Also runs the node's DECLARATION-time texture prepare, for the same
    // reason AddPickNodes builds its geometry here: this is the last point
    // before the frame's command buffer opens.
    //
    // `context` may be null -- see AddBatch2DNode's signature note; a null
    // context is the device-less declaration-shape drive the [nri] frame-shape
    // cases use, where every declaration is identical and only the exec fn is
    // inert.
    ARCANE_API void AddImGuiNode(RenderGraph& graph, NriGraphContext* context, RgTexture target,
                                 ImGuiNodeSlot slot = ImGuiNodeSlot::HostHud);
}

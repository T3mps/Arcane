#pragma once

// DeferredPick -- the viewport click-pick that CANNOT ANSWER IN THE FRAME IT IS
// ASKED (NRI Phase 3, Task 9), as a pure state machine.
//
// ===================================================================
// WHY THIS EXISTS AT ALL
// ===================================================================
// On the NVRHI arm a viewport click is resolved inside one statement:
// PickBuffer::Pick renders the id pass and then calls waitForIdle to read one
// texel back. That is a full GPU stall, which is affordable exactly because it
// happens on a click rather than every frame.
//
// The graph arm cannot do that and must not want to. Its readback is a graph
// COPY node into a per-frame-slot HOST_READBACK region, and the CPU reads a
// region only at the moment the graph is about to overwrite it -- so the value
// that comes back is the one written kSwapchainFramesInFlight frames ago
// (PickOutlineNodes.hpp, THE READBACK). No fence query, no idle, and no answer
// this frame. The plan's reconciliation 5 accepts that latency; what it does
// not accept is applying the WRONG answer, and everything below is about that.
//
// ===================================================================
// THE THREE HAZARDS, and which member closes each
// ===================================================================
//  1. WHICH REQUEST DOES THIS ANSWER? The editor's probe pixel moves: it is the
//     click pixel on the frame a click is resolved and nothing in particular
//     otherwise, and several clicks can be in flight across a slow frame. So
//     every request carries a TICKET, minted here, stamped onto the copy
//     (NriGraphContext::FrameDesc::pickTicket) and handed back beside the id
//     (ProbeResult). A value whose ticket is not the one outstanding is not our
//     answer and is dropped.
//
//  2. WHICH SCENE WAS IT ASKED ABOUT? An id is only meaningful against the
//     id<->entity table of the frame that RASTERISED it, and that table is
//     rebuilt every frame. So the table is COPIED (entities only) at request
//     time and inverted at landing time -- never the live one. On top of that,
//     a scene that was REPLACED between the two (New/Open Scene, a project
//     switch, Play start or stop -- all of which swap the registry object)
//     invalidates the answer entirely, so the request records the scene EPOCH
//     and the play mode it was made under and refuses to apply across a change
//     in either.
//
//  3. CAN IT APPLY TWICE? The vehicle's ProbeResult does not self-clear: it
//     reports the same (id, ticket) pair every frame until the next drain
//     replaces it. Consuming it exactly once is the host's job, and here it is
//     structural -- Land() returns to Idle, and no two requests ever share a
//     ticket.
//
// PURE AND HEADLESS: no ImGui, no engine render types, no registry. It knows
// entities only as opaque handles, which is what lets the [editor] units drive
// the whole table -- including the orderings a GPU run would take frames to
// reach -- with no device at all. The host keeps the two policy decisions this
// deliberately does NOT make: whether a click was legal (the four guards at
// EditorApp::HandleViewportPick) and what a resolved entity means (Select /
// Toggle / Clear).

#include <Astra/Entity/Entity.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Arcane::Editor
{
    class DeferredPick
    {
    public:
        // Armed  -- a click is waiting for the next frame to rasterise it.
        // InFlight -- a copy carrying our ticket is on the GPU.
        // The gap between Armed and InFlight is one frame and is inherent: the
        // click is detected in phase 17, AFTER phase 10 declared this frame's
        // graph, so the earliest frame that can probe it is the next one.
        enum class Phase : std::uint8_t { Idle, Armed, InFlight };

        // What one landed readback resolves to. An INVALID entity is a real
        // answer and means BACKGROUND -- the host clears the selection on it
        // (unless ctrl was held), exactly as the NVRHI arm does on id 0.
        struct Hit
        {
            Astra::Entity entity{};
            bool          ctrlHeld = false;
        };

        // What the frame must put in FrameDesc: the texel to copy and the label
        // to stamp on it.
        struct Request
        {
            glm::ivec2    pixel{0, 0};
            std::uint64_t ticket = 0;
        };

        // Phase 17: a viewport click passed the host's guards. REPLACES any
        // outstanding request -- the newest click wins, and the older one's
        // readback is dropped on arrival by its stale ticket. `sceneEpoch` and
        // `playMode` are recorded here, not at TakeRequest, because they
        // describe the scene the USER clicked on.
        void Arm(glm::ivec2 pixel, bool ctrlHeld, std::uint64_t sceneEpoch, bool playMode)
        {
            m_phase       = Phase::Armed;
            m_pixel       = pixel;
            m_ctrlHeld    = ctrlHeld;
            m_epoch       = sceneEpoch;
            m_playMode    = playMode;
            m_ticket      = 0;          // minted at TakeRequest, not here
            m_framesInFlight = 0;
            m_ordered.clear();
        }

        // Phase 10: this frame is about to declare the pick chain. Hands back
        // the probe pixel + a FRESH ticket, and takes custody of the id<->entity
        // table THIS frame's id pass will be built from -- which is the only
        // table the id coming back can be inverted through.
        //
        // Nullopt unless a click is Armed. Moving to InFlight here (rather than
        // at Arm) is what makes "the chain must stay declared until this lands"
        // a single predicate the caller can read (Busy()).
        std::optional<Request> TakeRequest(std::span<const Astra::Entity> ordered)
        {
            if (m_phase != Phase::Armed)
                return std::nullopt;
            m_ticket = ++m_nextTicket;   // 1-based: 0 is the "unlabelled" default
            m_ordered.assign(ordered.begin(), ordered.end());
            m_phase  = Phase::InFlight;
            m_framesInFlight = 0;
            return Request{ m_pixel, m_ticket };
        }

        // Phase 17: a readback landed. Returns the hit to apply, or nullopt for
        // every reason not to:
        //   * nothing outstanding, or the ticket is not ours (an older
        //     request's copy, or a value from before this request);
        //   * the scene was replaced, or Play started/stopped, since the click
        //     -- the retained table then names entities of a scene that no
        //     longer exists, and the SAFE direction is to change nothing.
        // Consumes the request either way when the ticket matches: a stale
        // answer to OUR ticket is still the answer to it, and leaving the
        // request outstanding would keep the pick chain declared forever.
        std::optional<Hit> Land(std::uint64_t ticket, std::uint32_t id,
                                std::uint64_t sceneEpoch, bool playMode)
        {
            if (m_phase != Phase::InFlight || ticket == 0 || ticket != m_ticket)
                return std::nullopt;

            const bool sameScene = (sceneEpoch == m_epoch) && (playMode == m_playMode);
            const bool ctrlHeld  = m_ctrlHeld;
            // Inverted BEFORE the reset, out of the RETAINED table -- never the
            // caller's live drawables, which have been rebuilt since.
            const Astra::Entity entity = PickedEntity(id);
            Reset();
            if (!sameScene)
                return std::nullopt;
            return Hit{ entity, ctrlHeld };
        }

        // Called once per frame while a request is outstanding. True EXACTLY ON
        // the frame it gives up, so the caller can say so once.
        //
        // A budget rather than a trust: the readback is guaranteed to drain
        // after kSwapchainFramesInFlight rendered frames of the declared chain,
        // and the caller keeps the chain declared for precisely that reason --
        // so this should never fire. It exists because the failure it guards is
        // a state machine that never returns to Idle, which would silently keep
        // an outline chain declared for the rest of the session; a bounded,
        // loud give-up is strictly better than an unbounded silent one.
        bool TickAndMaybeAbandon()
        {
            if (m_phase != Phase::InFlight)
                return false;
            if (++m_framesInFlight <= kMaxFramesInFlight)
                return false;
            Reset();
            return true;
        }

        // Drop any outstanding request. The host calls this wherever the scene
        // is replaced, so the epoch check below is a backstop rather than the
        // only line of defence.
        void Reset() noexcept
        {
            m_phase = Phase::Idle;
            m_ticket = 0;
            m_framesInFlight = 0;
            m_ordered.clear();
        }

        // "Something is outstanding", i.e. the pick chain must stay declared.
        [[nodiscard]] bool Busy()  const noexcept { return m_phase != Phase::Idle; }
        [[nodiscard]] Phase State() const noexcept { return m_phase; }
        // The ticket currently outstanding, or 0. For logging and the units.
        [[nodiscard]] std::uint64_t Ticket() const noexcept { return m_ticket; }

        // id 0 -> background (invalid entity), id k -> the k-th retained entity.
        // Public so a unit can read the retained table's inversion directly.
        [[nodiscard]] Astra::Entity PickedEntity(std::uint32_t id) const
        {
            if (id == 0 || id > m_ordered.size())
                return Astra::Entity{};
            return m_ordered[id - 1];
        }

        // Frames a request may stay in flight before TickAndMaybeAbandon gives
        // up. Two orders of magnitude above kSwapchainFramesInFlight, because
        // the only legitimate reason to exceed that is a run of SKIPPED frames
        // (a collapsed viewport panel), and the budget must not turn a briefly
        // collapsed panel into a lost click.
        static constexpr std::uint32_t kMaxFramesInFlight = 64;

    private:
        Phase         m_phase    = Phase::Idle;
        glm::ivec2    m_pixel{0, 0};
        bool          m_ctrlHeld = false;
        // The scene identity the CLICK was made against -- see hazard 2.
        std::uint64_t m_epoch    = 0;
        bool          m_playMode = false;

        std::uint64_t m_ticket     = 0;
        std::uint64_t m_nextTicket = 0;
        std::uint32_t m_framesInFlight = 0;

        // The id<->entity table of the frame that rasterised the request.
        // Entities only (not whole PickDrawables) because the inverse mapping
        // is all that is ever asked of it, and this is retained across frames.
        std::vector<Astra::Entity> m_ordered;
    };
}

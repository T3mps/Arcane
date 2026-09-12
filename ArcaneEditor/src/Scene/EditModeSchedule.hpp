#pragma once

// EditModeSchedule (Astra adoption 2026-09-11, spec s7): the editor's OWN
// scheduler for Edit mode, plus the single pending camera-frame request that is
// serviced right after it runs.
//
// Edit mode holds the RunLoop paused, so the game module's fixedUpdate -- which
// owns TransformPropagationSystem -- never runs there, while update/render run
// every frame. Three ad-hoc temporaries used to paper over that (a propagation
// per frame in RefreshSceneResolution, another on scene open, another on the
// F/Home key at input time -- two of them in the same frame as the first). Now
// propagation runs EXACTLY ONCE per Edit-mode frame, through a real scheduler,
// and anything that wants the camera framed records a request that is serviced
// after that pass: bounds computed on fresh WorldTransforms, camera moved, same
// frame, before the scene renders. In Play mode fixedUpdate owns propagation and
// this runs nothing. Both schedulers share TransformOrder (a registry resource)
// and its lastRun; a Play->Stop switch costs one full recompose, which the
// registry restore forces anyway.
//
// ImGui-free on purpose: ArcaneTests compiles this TU directly (root
// premake5.lua) and drives it against a bare registry.

#include "Viewport/EditorCamera.hpp"

#include <Astra/Entity/Entity.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/System/SystemScheduler.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <functional>
#include <span>

namespace Arcane::Editor
{
    // What to frame. The request is a SINGLE slot: the last one recorded in a
    // frame wins. SceneOpen differs from Scene only when nothing is framable --
    // a just-opened empty scene centres the origin (the user is about to build
    // there); a Home press on an empty scene leaves the view alone.
    enum class FrameRequest : std::uint8_t
    {
        None = 0,
        Selection,   // F with a selection: SelectionFramingBounds over it
        Scene,       // F with no selection, or Home: SceneFramingBounds
        SceneOpen,   // a scene just became current: SceneFramingBounds, origin fallback
    };

    class EditModeSchedule
    {
    public:
        EditModeSchedule();

        // Phase 9. Runs the Edit-mode systems (today: TransformPropagationSystem)
        // exactly once when !inPlayMode; a no-op in Play. Returns true iff it ran.
        bool RunFrame(Astra::Registry& reg, bool inPlayMode);

        void RequestFrame(FrameRequest request) noexcept { m_pending = request; }
        [[nodiscard]] FrameRequest Pending() const noexcept { return m_pending; }

        // Services and clears the pending request against the registry's
        // WorldTransforms AS THEY ARE NOW (call after RunFrame). A zero-sized
        // viewport keeps a SceneOpen request for a later frame (the panel has not
        // been laid out yet) and drops the other two, exactly as EditorCamera::
        // Frame's own guard used to. Returns true iff the camera moved.
        bool ServicePendingFrame(Astra::Registry& reg, std::span<const Astra::Entity> selection,
                                 EditorCamera& camera, glm::vec2 viewportSize);

        // Edit mode's ONLY physics (spec 2026-09-11-physics-2d-wiring s6.1):
        // an injected callable -- EditorApp binds Runtime::PhysicsEditPass --
        // run by RunFrame BEFORE propagation, so bodies exist in the editor
        // world without simulating and every authored edit reaches them the
        // frame after it lands. A callable rather than the system itself so
        // this class (and its device-less test) links no Manifold2D.
        void SetPhysicsEditPass(std::function<void()> pass) { m_physicsEditPass = std::move(pass); }

    private:
        Astra::SystemScheduler  m_schedule;
        FrameRequest            m_pending = FrameRequest::None;
        std::function<void()>  m_physicsEditPass;
    };
}

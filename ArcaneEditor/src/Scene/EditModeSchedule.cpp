#include "Scene/EditModeSchedule.hpp"

#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <tuple>

namespace Arcane::Editor
{
    EditModeSchedule::EditModeSchedule()
    {
        // The two Edit-mode systems. AddSystem's Result is [[nodiscard]]; its only
        // failure is a duplicate registration, impossible on a fresh scheduler.
        std::ignore = m_schedule.AddSystem<Arcane::TransformPropagationSystem>();
        std::ignore = m_schedule.AddSystem<Arcane::BoundsSystem>();   // F3: boxes follow the Edit-mode propagation
    }

    bool EditModeSchedule::RunFrame(Astra::Registry& reg, bool inPlayMode)
    {
        if (inPlayMode)
            return false;
        if (m_physicsEditPass) m_physicsEditPass();   // mint / destroy / reconcile; propagation composes the result
        m_schedule.Execute(reg);   // sequential executor; two exclusive systems, propagation then bounds
        return true;
    }

    bool EditModeSchedule::ServicePendingFrame(Astra::Registry& reg,
                                               std::span<const Astra::Entity> selection,
                                               EditorCamera& camera, glm::vec2 viewportSize)
    {
        const FrameRequest request = m_pending;
        m_pending = FrameRequest::None;
        if (request == FrameRequest::None)
            return false;
        if (!(viewportSize.x > 0.0f) || !(viewportSize.y > 0.0f))
        {
            if (request == FrameRequest::SceneOpen)
                m_pending = request;   // deferred: the viewport has no size yet
            return false;
        }

        const FramingBounds bounds = (request == FrameRequest::Selection)
            ? SelectionFramingBounds(reg, selection)
            : SceneFramingBounds(reg);
        if (bounds.Valid())
        {
            camera.Frame(bounds, glm::uvec2(viewportSize));   // mode-aware (spec s4)
            return true;
        }
        if (request == FrameRequest::SceneOpen)
        {
            camera.CentreOrigin();   // an empty scene: centre the origin
            return true;
        }
        return false;   // nothing framable: leave the user's view where it is
    }
}

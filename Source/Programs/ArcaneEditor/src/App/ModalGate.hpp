#pragma once
// ModalGate: at most ONE dockspace-level modal draws per frame (DrawModals).
// ImGui allows exactly one popup at a given stack level, and every modal in
// DrawModals re-arms itself ("if (!IsPopupOpen) OpenPopup"), so two of them up
// at once close each other every frame and both end up invisible. The gate
// names the winner: a queued error first, then a parked scene intent (the
// unsaved-scene confirm), then the create dialog. A loser keeps its state and
// draws again once everything above it is clear. Pure -- drawing stays host-side.
#include <cstdint>

namespace Arcane::Editor
{
    enum class ModalKind : std::uint8_t { None, Error, UnsavedScene, CreateAsset };

    [[nodiscard]] inline ModalKind ModalAllowed(bool errorQueued,
                                                bool sceneIntentParked,
                                                bool createOpen)
    {
        if (errorQueued)        return ModalKind::Error;
        if (sceneIntentParked)  return ModalKind::UnsavedScene;
        if (createOpen)         return ModalKind::CreateAsset;
        return ModalKind::None;
    }
}

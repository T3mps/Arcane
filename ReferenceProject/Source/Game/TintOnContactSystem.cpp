#include "TintOnContactSystem.hpp"

#include <Arcane/Plugin/GameSystems.hpp>

// The module prologue discovers this default-constructible system. It runs in
// Update, after this frame's fixed steps, so FrameEvents() holds every contact
// that began during the frame. It writes a presented tint, so it does not run
// on a dedicated server's separate world.
ARC_SYSTEM(
    ReferenceProject::TintOnContactSystem,
    Arcane::RoleMask::Client,
    Arcane::SystemPhase::Update)

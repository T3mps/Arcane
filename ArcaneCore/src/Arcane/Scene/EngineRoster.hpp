#pragma once

// The engine's own component roster, named once.
//
// Astra assigns ComponentIDs from a first-touch counter, so the ORDER of the
// list below IS the engine's id numbering. Do not reorder it; append only.
// (2026-09-11: PhysicsSettings was appended after MeshRenderer, which shifted
// the three physics ids up by one -- in-process only, as ever.)
//
// Two places expand this one list, which is the whole point of it existing:
//   - Runtime.cpp's engineModule->Register<...> (RegisterRoster below) -- the
//     registration that MINTS the ids;
//   - ProjectHost.hpp's VerifySharedTypeContext -- the boot check that asks
//     whether the CALLING module resolves every one of them to the same id.
// Before 2026-09-16 the verify probed a single type (Arcane::Transform) and
// the roster lived only in Runtime.cpp, so a module whose cache was wrong for
// SOME types but right for Transform passed the check. That is exactly what
// the editor did: EditorApp's EditModeSchedule member resolved WorldTransform
// in the exe's private TypeContext before the shared one was installed.

#include <Arcane/Scene/Components.hpp>          // Transform / WorldTransform / ... / MeshRenderer / PhysicsSettings
#include <Arcane/Scene/PhysicsComponents.hpp>   // RigidBody2D / Collider2D / PhysicsBodyRef

namespace Arcane
{
    // A bare compile-time type list. Astra has none of its own, and this needs
    // to be nothing more than a pack the two expanders can unpack.
    template<typename... Ts>
    struct TypeList {};

    // EXACTLY the order RegisterSceneComponents + RegisterPhysicsComponents
    // register in (SceneModule.hpp / PhysicsComponents.hpp).
    using EngineComponentRoster = TypeList<Transform, WorldTransform, SpriteRenderer,
                                           PostProcess, Identity, Hidden, Camera, MeshRenderer,
                                           PhysicsSettings,
                                           RigidBody2D, Collider2D, PhysicsBodyRef>;
}

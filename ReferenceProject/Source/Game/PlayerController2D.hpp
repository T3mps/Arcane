#pragma once

// PlayerController2D: a component -- plain data on an entity. Reflected so the editor's
// Inspector can show and edit it, scenes can save it, and the Add Component
// catalog can offer it. Registered with the game module by the
// ARC_COMPONENT line in PlayerController2D.cpp; nothing else to wire.
//
// Authored tuning plus the controller's own per-body state. Input and time are
// NOT copied in here: PlayerController2DSystem reads them as resources.

#include <Arcane/Reflection.hpp>

namespace ReferenceProject
{
    struct PlayerController2D
    {
        float moveSpeed = 5.0f;           // m/s, not pixels per second
        float jumpSpeed = 5.0f;           // m/s upward
        float groundAcceleration = 45.0f; // m/s^2
        float groundBraking = 55.0f;
        float turnAcceleration = 60.0f;
        float airAcceleration = 25.0f;
        float coyoteTime = 0.10f;         // seconds after leaving a ledge
        float jumpBufferTime = 0.10f;     // seconds before landing
        float jumpCutMultiplier = 0.45f;
        float coyoteRemaining = 0.0f;
        float jumpBufferRemaining = 0.0f;
        bool jumpConsumed = false;
        bool jumpCutArmed = false;
    };

    ARC_REFLECT_TYPE(PlayerController2D)
        ARC_REFLECT_FIELD(PlayerController2D, moveSpeed)
        ARC_REFLECT_FIELD(PlayerController2D, jumpSpeed)
        ARC_REFLECT_FIELD(PlayerController2D, groundAcceleration)
        ARC_REFLECT_FIELD(PlayerController2D, groundBraking)
        ARC_REFLECT_FIELD(PlayerController2D, turnAcceleration)
        ARC_REFLECT_FIELD(PlayerController2D, airAcceleration)
        ARC_REFLECT_FIELD(PlayerController2D, coyoteTime)
        ARC_REFLECT_FIELD(PlayerController2D, jumpBufferTime)
        ARC_REFLECT_FIELD(PlayerController2D, jumpCutMultiplier)
        ARC_REFLECT_FIELD(PlayerController2D, coyoteRemaining)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
        ARC_REFLECT_FIELD(PlayerController2D, jumpBufferRemaining)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
        ARC_REFLECT_FIELD(PlayerController2D, jumpConsumed)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
        ARC_REFLECT_FIELD(PlayerController2D, jumpCutArmed)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
    ARC_END_REFLECT_TYPE()
}

#pragma once

// PlayerController2D: a component -- plain data on an entity. Reflected so the editor's
// Inspector can show and edit it, scenes can save it, and the Add Component
// catalog can offer it. Registered with the game module by the
// ARCANE_COMPONENT line in PlayerController2D.cpp; nothing else to wire.
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

    ARCANE_REFLECT_TYPE(PlayerController2D)
        ARCANE_REFLECT_FIELD(PlayerController2D, moveSpeed)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpSpeed)
        ARCANE_REFLECT_FIELD(PlayerController2D, groundAcceleration)
        ARCANE_REFLECT_FIELD(PlayerController2D, groundBraking)
        ARCANE_REFLECT_FIELD(PlayerController2D, turnAcceleration)
        ARCANE_REFLECT_FIELD(PlayerController2D, airAcceleration)
        ARCANE_REFLECT_FIELD(PlayerController2D, coyoteTime)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpBufferTime)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpCutMultiplier)
        ARCANE_REFLECT_FIELD(PlayerController2D, coyoteRemaining)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpBufferRemaining)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpConsumed)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpCutArmed)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
    ARCANE_END_REFLECT_TYPE()
}

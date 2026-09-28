#pragma once

// PlayerController2D: a component -- plain data on an entity. Reflected so the editor's
// Inspector can show and edit it, scenes can save it, and the Add Component
// catalog can offer it. Registered with the game module by the
// ARCANE_COMPONENT line in PlayerController2D.cpp; nothing else to wire.

#include <Astra/Reflection/Reflection.hpp>
namespace ReferenceProject
{
    struct PlayerController2D
    {
        float value = 0.0f;
        float moveSpeed = 5.0f;         // m/s, not pixels per second
        float jumpSpeed = 5.0f;         // m/s upward
        float groundAcceleration = 45.0f; // m/s^2
        float groundBraking = 55.0f;
        float turnAcceleration = 60.0f;
        float airAcceleration = 25.0f;
        float coyoteTime = 0.10f;      // seconds after leaving a ledge
        float jumpBufferTime = 0.10f;  // seconds before landing
        float jumpCutMultiplier = 0.45f;
        bool jumpRequested = false; // fixed-step input pulse, never authored
        bool jumpHeld = false;
        float fixedDt = 0.0f;
        float coyoteRemaining = 0.0f;
        float jumpBufferRemaining = 0.0f;
        bool jumpConsumed = false;
        bool jumpCutArmed = false;
    };

    ASTRA_REFLECT_TYPE(PlayerController2D)
        ASTRA_REFLECT_FIELD(PlayerController2D, value)
        ASTRA_REFLECT_FIELD(PlayerController2D, moveSpeed)
        ASTRA_REFLECT_FIELD(PlayerController2D, jumpSpeed)
        ASTRA_REFLECT_FIELD(PlayerController2D, groundAcceleration)
        ASTRA_REFLECT_FIELD(PlayerController2D, groundBraking)
        ASTRA_REFLECT_FIELD(PlayerController2D, turnAcceleration)
        ASTRA_REFLECT_FIELD(PlayerController2D, airAcceleration)
        ASTRA_REFLECT_FIELD(PlayerController2D, coyoteTime)
        ASTRA_REFLECT_FIELD(PlayerController2D, jumpBufferTime)
        ASTRA_REFLECT_FIELD(PlayerController2D, jumpCutMultiplier)
        ASTRA_REFLECT_FIELD(PlayerController2D, jumpRequested)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(Hidden)
        ASTRA_REFLECT_FIELD(PlayerController2D, jumpHeld)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(Hidden)
        ASTRA_REFLECT_FIELD(PlayerController2D, fixedDt)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(Hidden)
        ASTRA_REFLECT_FIELD(PlayerController2D, coyoteRemaining)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(Hidden)
        ASTRA_REFLECT_FIELD(PlayerController2D, jumpBufferRemaining)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(Hidden)
        ASTRA_REFLECT_FIELD(PlayerController2D, jumpConsumed)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(Hidden)
        ASTRA_REFLECT_FIELD(PlayerController2D, jumpCutArmed)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(Hidden)
    ASTRA_END_REFLECT_TYPE()
}

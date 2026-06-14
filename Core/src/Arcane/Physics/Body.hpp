#pragma once

// Body: an ergonomic view over a {world, handle} pair (M6, Task P1.8).
//
// PORT NOTE: ports the Lua `Body` proxy methods (Client/src/physics/
// PhysicsWorld.lua getPosition/setPosition/setVelocity/getVelocity/
// drawPosition/shape/setEventsEnabled). In the Lua, Body is the ONLY surface
// (the world returns a Body from addBody). In the C++ engine the CANONICAL
// surface is HANDLE-BASED on PhysicsWorld (events carry BodyHandles, so handle
// methods are the natural payload-consuming API); Body is a thin, copyable
// ergonomic wrapper that forwards to those world methods. Pick whichever fits
// the call site.
//
// A Body is cheap (two words: a PhysicsWorld* + a BodyHandle) and copyable. It
// is valid only while world->IsValid(handle); methods on a stale view forward
// to world methods that no-op / return defaults for invalid handles.
//
// DELIBERATELY DEFERRED to P2 (dynamics): applyImpulse, isAwake/wake, getAngle/
// setAngle. P1.8 is kinematic-only; those would be stubs over fields the SoA
// does not yet carry, so they are intentionally absent until P2.1 adds the
// dynamics state. (See PhysicsWorld.hpp PORT BOUNDARY.)
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// namespace Arcane::Physics, Core style.

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>

namespace Arcane
{
    namespace Physics
    {
        class Body
        {
        public:
            Body() = default;
            Body(PhysicsWorld* world, BodyHandle handle) noexcept
                : m_world(world), m_handle(handle)
            {
            }

            [[nodiscard]] BodyHandle    Handle() const noexcept { return m_handle; }
            [[nodiscard]] PhysicsWorld* World()  const noexcept { return m_world; }

            // True iff this view still refers to a live body.
            [[nodiscard]] bool IsValid() const noexcept
            {
                return m_world != nullptr && m_world->IsValid(m_handle);
            }

            // ---- ports of the Lua Body methods (forward to the world) -------

            [[nodiscard]] Vec2 GetPosition() const noexcept
            {
                return m_world->Position(m_handle);
            }

            // Teleport: prev snaps too (ports Body:setPosition).
            void SetPosition(Vec2 p) noexcept { m_world->SetPosition(m_handle, p); }

            [[nodiscard]] Vec2 GetVelocity() const noexcept
            {
                return m_world->Velocity(m_handle);
            }

            void SetVelocity(Vec2 v) noexcept { m_world->SetVelocity(m_handle, v); }

            // Render-boundary lerp prev..curr (ports Body:drawPosition).
            [[nodiscard]] Vec2 DrawPosition(Real alpha) const noexcept
            {
                return m_world->DrawPosition(m_handle, alpha);
            }

            [[nodiscard]] const Shape* GetShape() const noexcept
            {
                return m_world->GetShape(m_handle);
            }

            [[nodiscard]] BodyType GetType() const noexcept
            {
                return m_world->GetType(m_handle);
            }

            [[nodiscard]] bool IsSensor() const noexcept
            {
                return m_world->IsSensor(m_handle);
            }

            // Per-body event gate (ports Body:setEventsEnabled).
            void SetEventsEnabled(bool on) noexcept
            {
                m_world->SetBodyEvents(m_handle, on);
            }

        private:
            PhysicsWorld* m_world  = nullptr;
            BodyHandle    m_handle{};
        };

    } // namespace Physics
} // namespace Arcane

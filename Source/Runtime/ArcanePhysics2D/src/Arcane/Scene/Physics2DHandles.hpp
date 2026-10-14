#pragma once

// Solver handle aliases. PhysicsBodyRef2D::handle and PhysicsWorld2D's private maps name these.
// The using-declarations sit in an internal fence: a game spells Detail::
// nowhere (the [facade] scan). Physics2D.hpp may include this header; it does
// not include the Detail helpers.

#include <Manifold2D/Physics/Fixture.hpp>
#include <Manifold2D/Physics/PhysicsTypes.hpp>

// ARC_INTERNAL_BEGIN: Detail aliases name Manifold2D once, for the engine
namespace Manifold2D::Physics
{
    class PhysicsWorld;
}

namespace Arcane::Detail::Physics2D
{
    using PhysicsWorld  = ::Manifold2D::Physics::PhysicsWorld;
    using BodyHandle    = ::Manifold2D::Physics::BodyHandle;
    using FixtureHandle = ::Manifold2D::Physics::FixtureHandle;

    inline constexpr BodyHandle kInvalidBody{};
}
// ARC_INTERNAL_END

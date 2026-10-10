#pragma once

// Engine-only 2D physics helpers. Physics2D.hpp does not include this.
// Conversions, fixture builders and the private-member accessor live here.

#include <Arcane/Config/Bindings/Physics2DBinding.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>

#include <Manifold2D/Physics/Fixture.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/Shapes.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Arcane::Physics2D::Detail
{
    namespace Phys = ::Manifold2D::Physics;

    struct Access
    {
        static PhysicsWorld* Solver(World& w) noexcept { return w.world.get(); }
        static const PhysicsWorld* Solver(const World& w) noexcept { return w.world.get(); }
        static auto& Entities(World& w) noexcept { return w.entityToBody; }
        static const auto& Entities(const World& w) noexcept { return w.entityToBody; }
        static Arcane::Tick& LastReconcile(World& w) noexcept { return w.lastReconcile; }
        static std::uint32_t& Reconciled(World& w) noexcept { return w.reconciled; }
        static auto& Records(World& w) noexcept { return w.bodyRecords; }
        static const auto& Records(const World& w) noexcept { return w.bodyRecords; }
        static void SetSolver(World& w, std::unique_ptr<PhysicsWorld> solver) noexcept
        {
            w.world = std::move(solver);
        }
    };

    inline World Adopt(std::unique_ptr<PhysicsWorld> solver)
    {
        World w;
        Access::SetSolver(w, std::move(solver));
        return w;
    }

    [[nodiscard]] constexpr Phys::BodyType ToVendor(BodyType t) noexcept
    {
        switch (t)
        {
        case BodyType::Static:    return Phys::BodyType::Static;
        case BodyType::Kinematic: return Phys::BodyType::Kinematic;
        case BodyType::Dynamic:   return Phys::BodyType::Dynamic;
        }
        return Phys::BodyType::Kinematic;
    }

    [[nodiscard]] constexpr Phys::ShapeKind ToVendor(ShapeKind k) noexcept
    {
        switch (k)
        {
        case ShapeKind::Circle:  return Phys::ShapeKind::Circle;
        case ShapeKind::Capsule: return Phys::ShapeKind::Capsule;
        case ShapeKind::Aabb:    return Phys::ShapeKind::Aabb;
        case ShapeKind::Polygon: return Phys::ShapeKind::Polygon;
        }
        return Phys::ShapeKind::Circle;
    }

    [[nodiscard]] inline Phys::BroadphaseKind ToBroadphaseKind(Broadphase b) noexcept
    {
        switch (b)
        {
        case Broadphase::Tree: return Phys::BroadphaseKind::Tree;
        case Broadphase::Hash: return Phys::BroadphaseKind::Hash;
        case Broadphase::Sap:  return Phys::BroadphaseKind::Sap;
        }
        return Phys::BroadphaseKind::Tree;
    }

    [[nodiscard]] inline Phys::WorldDef ToWorldDef(const WorldSettings& s)
    {
        Phys::WorldDef wd;
        wd.broadphase             = ToBroadphaseKind(s.broadphase);
        wd.hashCellSize           = s.hashCellSize;
        wd.substepCount           = s.substepCount;
        wd.contactHertz           = s.contactHertz;
        wd.contactDampingRatio    = s.contactDampingRatio;
        wd.restitutionThreshold   = s.restitutionThreshold;
        wd.contactPushMaxVelocity = s.contactPushMaxVelocity;
        wd.maxLinearVelocity      = s.maxLinearVelocity;
        wd.sleepThreshold         = s.sleepThreshold;
        return wd;
    }

    [[nodiscard]] constexpr std::uint64_t PackBody(Phys::BodyHandle h) noexcept
    {
        return (static_cast<std::uint64_t>(h.index) << 32) | h.generation;
    }

    [[nodiscard]] inline Phys::Shape MakeScaledShape(const Fixture& f, glm::vec2 scale)
    {
        const float sx   = std::abs(scale.x);
        const float sy   = std::abs(scale.y);
        const float sMax = std::max(sx, sy);
        switch (f.kind)
        {
        case ShapeKind::Circle:  return Phys::MakeCircle(f.radius * sMax);
        case ShapeKind::Capsule: return Phys::MakeCapsule(f.halfLen * sx, f.radius * sy);
        case ShapeKind::Aabb:    return Phys::MakeAabb(f.halfW * sx, f.halfH * sy);
        case ShapeKind::Polygon:
            assert(false && "Physics2D::Detail::MakeScaledShape: ShapeKind::Polygon not supported");
            return Phys::MakeCircle(f.radius * sMax);
        }
        return Phys::MakeCircle(f.radius * sMax);
    }

    [[nodiscard]] inline float AngleDelta(float a, float b)
    {
        ARC_CONSTANT("math identity / tolerance: pi")
        constexpr float kPi  = 3.14159265358979323846f;
        ARC_CONSTANT("math identity / tolerance: tau = 2 pi")
        constexpr float kTau = 6.28318530717958647692f;
        float d = a - b;
        while (d >  kPi) d -= kTau;
        while (d < -kPi) d += kTau;
        return std::abs(d);
    }

    [[nodiscard]] inline Phys::FixtureDef MakeFixtureDef(const Fixture& f,
                                                         glm::vec2 scale = glm::vec2(1.0f, 1.0f))
    {
        Phys::FixtureDef fd;
        fd.shape = MakeScaledShape(f, scale);
        fd.localPos   = Phys::Vec2(f.localPos.x * scale.x, f.localPos.y * scale.y);
        fd.localAngle = static_cast<Phys::Real>(f.localAngle);
        fd.density     = static_cast<Phys::Real>(f.density);
        fd.friction    = static_cast<Phys::Real>(f.friction);
        fd.restitution = static_cast<Phys::Real>(f.restitution);
        fd.categoryBits = f.categoryBits;
        fd.maskBits     = f.maskBits;
        fd.isSensor = f.isSensor;
        fd.contactEvents = f.contactEvents;
        fd.sensorEvents  = f.sensorEvents;
        fd.hitEvents     = f.hitEvents;
        return fd;
    }

    [[nodiscard]] inline std::vector<Phys::FixtureHandle> RebuildScaledFixtures(Phys::PhysicsWorld& world,
                                                                               Phys::BodyHandle bh,
                                                                               const Collider& col,
                                                                               glm::vec2 scale)
    {
        const std::uint32_t n = world.FixtureCount(bh);
        std::vector<Phys::FixtureHandle> old;
        old.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i)
            old.push_back(world.GetBodyFixture(bh, i));

        std::vector<Phys::FixtureHandle> neu;
        neu.reserve(col.fixtures.size());
        for (const Fixture& f : col.fixtures)
        {
            Phys::FixtureDef fd = MakeFixtureDef(f, scale);
            neu.push_back(world.AddFixture(bh, fd));
        }
        for (Phys::FixtureHandle fh : old)
            world.DropFixture(fh);
        return neu;
    }
}

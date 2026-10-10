#pragma once

// Physics2D::World and Physics2D::System. The system body lives in
// PhysicsSystem.hpp so a game that includes Physics2D.hpp does not compile
// the solver passes. World's destructor is out of line: the solver object
// stays an incomplete type here.

#include <Arcane/Core/Api.hpp>
#include <Arcane/Ecs.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Scene/Physics2DHandles.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsEvents2D.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Arcane::Physics2D
{
    namespace Detail { struct Access; }

    struct RetiredFixture
    {
        Detail::FixtureHandle handle{};
        std::uint32_t         index = 0;
    };

    struct BodyRecord
    {
        Arcane::ECS::Entity                  entity = Arcane::ECS::Entity::Invalid();
        Guid                                 guid{};
        std::vector<Detail::FixtureHandle>   fixtures;
        bool                                 retired = false;
        std::vector<RetiredFixture>          retiredFixtures;
    };

    struct EventBuffers
    {
        std::vector<ContactBegin> contactBegin;
        std::vector<ContactEnd>   contactEnd;
        std::vector<ContactHit>   contactHit;
        std::vector<SensorBegin>  sensorBegin;
        std::vector<SensorEnd>    sensorEnd;
        void Clear() noexcept
        {
            contactBegin.clear(); contactEnd.clear(); contactHit.clear();
            sensorBegin.clear(); sensorEnd.clear();
        }
        [[nodiscard]] Events View() const noexcept
        {
            return { contactBegin, contactEnd, contactHit, sensorBegin, sensorEnd };
        }
    };

    // Transient registry resource. The solver, the entity map and the event
    // records are private. Games use the methods below.
    struct World
    {
        World() = default;
        ARC_CORE_API ~World();
        ARC_CORE_API World(World&&) noexcept;
        ARC_CORE_API World& operator=(World&&) noexcept;
        World(const World&) = delete;
        World& operator=(const World&) = delete;

        ARC_CORE_API Events StepEvents() const;
        ARC_CORE_API Events FrameEvents() const;
        ARC_CORE_API void BeginFrame();
        ARC_CORE_API void ContactsOf(Arcane::ECS::Entity entity, std::vector<ContactPoint>& out) const;
        ARC_CORE_API BodyMotion Motion(Arcane::ECS::Entity entity, const RigidBody& body) const;
        ARC_CORE_API void SetVelocity(Arcane::ECS::Entity entity, RigidBody& body, float velocityX, float velocityY);

        static constexpr bool AstraTransientResource = true;

        template<typename Archive>
        void Serialize(Archive& /*ar*/) {}

    private:
        friend struct System;
        friend struct Detail::Access;

        ARC_CORE_API void RecordBody(Arcane::ECS::Entity entity, Guid guid, Detail::BodyHandle handle,
                                     std::vector<Detail::FixtureHandle> fixtures);
        ARC_CORE_API void RetireBody(Detail::BodyHandle handle);
        ARC_CORE_API void CaptureStep();

        std::unique_ptr<Detail::PhysicsWorld>                 world;
        std::unordered_map<Arcane::ECS::Entity, Detail::BodyHandle> entityToBody;
        Arcane::ECS::Tick  lastReconcile = 0;
        std::uint32_t      reconciled = 0;
        std::unordered_map<std::uint64_t, BodyRecord> bodyRecords;
        EventBuffers stepEvents;
        EventBuffers frameEvents;
    };

    struct System
        : Arcane::ECS::SystemTraits<Arcane::ECS::Reads<Collider>,
                                    Arcane::ECS::Writes<Arcane::Transform, BodyRef, RigidBody>,
                                    Arcane::ECS::Before<Arcane::TransformPropagationSystem>>
    {
        static constexpr bool RequiresExclusive = true;

        explicit System(float fixedDt, bool stepWorld = true) noexcept
            : m_fixedDt(fixedDt), m_stepWorld(stepWorld) {}

        void operator()(Arcane::ECS::Registry& reg);

    private:
        float m_fixedDt;
        bool  m_stepWorld;
    };
}

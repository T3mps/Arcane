// PhysicsEvents2D.cpp -- PhysicsResource's event members (spec 2026-10-08 s7).
// Translation: Manifold2D handles -> {entity, GUID, fixture index} through the
// body records PhysicsSystem fills at mint. Order is the world's (already sorted
// upstream); an event naming a body with no record is dropped.
#include <Arcane/Scene/PhysicsSystem.hpp>

#include <algorithm>

namespace Arcane
{
    namespace
    {
        bool Side(const PhysicsResource& res, Phys::BodyHandle body, Phys::FixtureHandle fx, ContactSide2D& out)
        {
            const auto it = res.bodyRecords.find(PackBody(body));
            if (it == res.bodyRecords.end()) return false;
            const BodyRecord2D& rec = it->second;
            // Current handles first, then the ones a paused rescale just dropped
            // (same Collider2D indices). A recycled slot's new generation does
            // not match the retired handle.
            auto indexOf = [](const std::vector<Phys::FixtureHandle>& fxs, Phys::FixtureHandle handle) -> int
            {
                const auto f = std::find(fxs.begin(), fxs.end(), handle);
                return f == fxs.end() ? -1 : static_cast<int>(f - fxs.begin());
            };
            int idx = indexOf(rec.fixtures, fx);
            if (idx < 0) idx = indexOf(rec.retiredFixtures, fx);
            if (idx < 0) return false;
            out.entity  = rec.entity;
            out.guid    = rec.guid;
            out.fixture = static_cast<std::uint32_t>(idx);
            return true;
        }
    }

    void PhysicsResource::RecordBody(Arcane::Entity entity, Guid guid, Phys::BodyHandle handle,
                                     std::vector<Phys::FixtureHandle> fixtures)
    {
        bodyRecords[PackBody(handle)] = BodyRecord2D{ entity, guid, std::move(fixtures), false };
    }

    void PhysicsResource::RetireBody(Phys::BodyHandle handle)
    {
        if (const auto it = bodyRecords.find(PackBody(handle)); it != bodyRecords.end())
            it->second.retired = true;
    }

    void PhysicsResource::CaptureStep()
    {
        stepEvents.Clear();
        if (world)
        {
            const Phys::ContactEvents c = world->GetContactEvents();
            for (const auto& e : c.begin)
            {
                ContactBegin2D o;
                if (Side(*this, e.bodyA, e.a, o.a) && Side(*this, e.bodyB, e.b, o.b)) stepEvents.contactBegin.push_back(o);
            }
            for (const auto& e : c.end)
            {
                ContactEnd2D o;
                if (Side(*this, e.bodyA, e.a, o.a) && Side(*this, e.bodyB, e.b, o.b)) stepEvents.contactEnd.push_back(o);
            }
            for (const auto& e : c.hit)
            {
                ContactHit2D o;
                if (!Side(*this, e.bodyA, e.a, o.a) || !Side(*this, e.bodyB, e.b, o.b)) continue;
                o.point  = glm::vec2(static_cast<float>(e.point.x), static_cast<float>(e.point.y));
                o.normal = glm::vec2(static_cast<float>(e.normal.x), static_cast<float>(e.normal.y));
                o.approachSpeed = static_cast<float>(e.approachSpeed);
                stepEvents.contactHit.push_back(o);
            }
            const Phys::SensorEvents s = world->GetSensorEvents();
            for (const auto& e : s.begin)
            {
                SensorBegin2D o;
                if (Side(*this, e.sensorBody, e.sensor, o.sensor) && Side(*this, e.visitorBody, e.visitor, o.visitor)) stepEvents.sensorBegin.push_back(o);
            }
            for (const auto& e : s.end)
            {
                SensorEnd2D o;
                if (Side(*this, e.sensorBody, e.sensor, o.sensor) && Side(*this, e.visitorBody, e.visitor, o.visitor)) stepEvents.sensorEnd.push_back(o);
            }
        }
        const auto append = [](auto& dst, const auto& src) { dst.insert(dst.end(), src.begin(), src.end()); };
        append(frameEvents.contactBegin, stepEvents.contactBegin);
        append(frameEvents.contactEnd,   stepEvents.contactEnd);
        append(frameEvents.contactHit,   stepEvents.contactHit);
        append(frameEvents.sensorBegin,  stepEvents.sensorBegin);
        append(frameEvents.sensorEnd,    stepEvents.sensorEnd);
        for (auto& kv : bodyRecords)
            kv.second.retiredFixtures.clear();
        std::erase_if(bodyRecords, [](const auto& kv) { return kv.second.retired; });
    }

    PhysicsEvents2D PhysicsResource::StepEvents() const  { return stepEvents.View(); }
    PhysicsEvents2D PhysicsResource::FrameEvents() const { return frameEvents.View(); }
    void PhysicsResource::BeginFrame() { frameEvents.Clear(); }
}

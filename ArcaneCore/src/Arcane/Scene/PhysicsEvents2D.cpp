#include <Arcane/Scene/Physics2DDetail.hpp>

#include <Manifold2D/Physics/Events.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include <algorithm>

namespace
{
    namespace Phys = Arcane::Detail::Physics2D::Phys;

    bool Side(const std::unordered_map<std::uint64_t, Arcane::Detail::Physics2D::BodyRecord>& records,
              Phys::BodyHandle body, Phys::FixtureHandle fx, Arcane::ContactSide2D& out)
    {
        using Arcane::Detail::Physics2D::PackBody;
        const auto it = records.find(PackBody(body));
        if (it == records.end()) return false;
        const Arcane::Detail::Physics2D::BodyRecord& rec = it->second;
        const auto cur = std::find(rec.fixtures.begin(), rec.fixtures.end(), fx);
        int idx = cur == rec.fixtures.end() ? -1 : static_cast<int>(cur - rec.fixtures.begin());
        if (idx < 0)
        {
            for (const Arcane::Detail::Physics2D::RetiredFixture& old : rec.retiredFixtures)
            {
                if (old.handle == fx) { idx = static_cast<int>(old.index); break; }
            }
        }
        if (idx < 0) return false;
        out.entity  = rec.entity;
        out.guid    = rec.guid;
        out.fixture = static_cast<std::uint32_t>(idx);
        return true;
    }
}

namespace Arcane
{
    void PhysicsWorld2D::RecordBody(Arcane::Entity entity, Guid guid, Detail::Physics2D::BodyHandle handle,
                           std::vector<Detail::Physics2D::FixtureHandle> fixtures)
    {
        bodyRecords[Detail::Physics2D::PackBody(handle)] = Detail::Physics2D::BodyRecord{ entity, guid, std::move(fixtures), false };
    }

    void PhysicsWorld2D::RetireBody(Detail::Physics2D::BodyHandle handle)
    {
        if (const auto it = bodyRecords.find(Detail::Physics2D::PackBody(handle)); it != bodyRecords.end())
            it->second.retired = true;
    }

    void PhysicsWorld2D::CaptureStep()
    {
        namespace Phys = Detail::Physics2D::Phys;
        stepEvents.Clear();
        if (world)
        {
            const Phys::ContactEvents c = world->GetContactEvents();
            for (const auto& e : c.begin)
            {
                ContactBegin2D o;
                if (Side(bodyRecords, e.bodyA, e.a, o.a) && Side(bodyRecords, e.bodyB, e.b, o.b))
                    stepEvents.contactBegin.push_back(o);
            }
            for (const auto& e : c.end)
            {
                ContactEnd2D o;
                if (Side(bodyRecords, e.bodyA, e.a, o.a) && Side(bodyRecords, e.bodyB, e.b, o.b))
                    stepEvents.contactEnd.push_back(o);
            }
            for (const auto& e : c.hit)
            {
                ContactHit2D o;
                if (!Side(bodyRecords, e.bodyA, e.a, o.a) || !Side(bodyRecords, e.bodyB, e.b, o.b)) continue;
                o.point  = glm::vec2(static_cast<float>(e.point.x), static_cast<float>(e.point.y));
                o.normal = glm::vec2(static_cast<float>(e.normal.x), static_cast<float>(e.normal.y));
                o.approachSpeed = static_cast<float>(e.approachSpeed);
                stepEvents.contactHit.push_back(o);
            }
            const Phys::SensorEvents s = world->GetSensorEvents();
            for (const auto& e : s.begin)
            {
                SensorBegin2D o;
                if (Side(bodyRecords, e.sensorBody, e.sensor, o.sensor) &&
                    Side(bodyRecords, e.visitorBody, e.visitor, o.visitor))
                    stepEvents.sensorBegin.push_back(o);
            }
            for (const auto& e : s.end)
            {
                SensorEnd2D o;
                if (Side(bodyRecords, e.sensorBody, e.sensor, o.sensor) &&
                    Side(bodyRecords, e.visitorBody, e.visitor, o.visitor))
                    stepEvents.sensorEnd.push_back(o);
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

    PhysicsEvents2D PhysicsWorld2D::StepEvents() const  { return stepEvents.View(); }
    PhysicsEvents2D PhysicsWorld2D::FrameEvents() const { return frameEvents.View(); }
    void PhysicsWorld2D::BeginFrame() { frameEvents.Clear(); }

    void PhysicsWorld2D::ContactsOf(Arcane::Entity entity, std::vector<ContactPoint2D>& out) const
    {
        namespace Phys = Detail::Physics2D::Phys;
        out.clear();
        const auto it = entityToBody.find(entity);
        if (!world || it == entityToBody.end() || !world->IsValid(it->second)) return;
        std::vector<Phys::BodyContact> raw;
        world->GetBodyContacts(it->second, raw);
        for (const Phys::BodyContact& c : raw)
        {
            ContactPoint2D p;
            if (!Side(bodyRecords, c.selfBody, c.self, p.self) || !Side(bodyRecords, c.otherBody, c.other, p.other))
                continue;
            p.normal = glm::vec2(static_cast<float>(c.normal.x), static_cast<float>(c.normal.y));
            p.pointCount = static_cast<std::uint32_t>(c.pointCount);
            out.push_back(p);
        }
    }
}

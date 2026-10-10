#include <Arcane/Scene/Physics2DDetail.hpp>

#include <Manifold2D/Physics/Events.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include <algorithm>

namespace
{
    namespace Phys = Arcane::Physics2D::Detail::Phys;

    bool Side(const std::unordered_map<std::uint64_t, Arcane::Physics2D::BodyRecord>& records,
              Phys::BodyHandle body, Phys::FixtureHandle fx, Arcane::Physics2D::ContactSide& out)
    {
        using Arcane::Physics2D::Detail::PackBody;
        const auto it = records.find(PackBody(body));
        if (it == records.end()) return false;
        const Arcane::Physics2D::BodyRecord& rec = it->second;
        const auto cur = std::find(rec.fixtures.begin(), rec.fixtures.end(), fx);
        int idx = cur == rec.fixtures.end() ? -1 : static_cast<int>(cur - rec.fixtures.begin());
        if (idx < 0)
        {
            for (const Arcane::Physics2D::RetiredFixture& old : rec.retiredFixtures)
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

namespace Arcane::Physics2D
{
    void World::RecordBody(Arcane::ECS::Entity entity, Guid guid, Detail::BodyHandle handle,
                           std::vector<Detail::FixtureHandle> fixtures)
    {
        bodyRecords[Detail::PackBody(handle)] = BodyRecord{ entity, guid, std::move(fixtures), false };
    }

    void World::RetireBody(Detail::BodyHandle handle)
    {
        if (const auto it = bodyRecords.find(Detail::PackBody(handle)); it != bodyRecords.end())
            it->second.retired = true;
    }

    void World::CaptureStep()
    {
        namespace Phys = Detail::Phys;
        stepEvents.Clear();
        if (world)
        {
            const Phys::ContactEvents c = world->GetContactEvents();
            for (const auto& e : c.begin)
            {
                ContactBegin o;
                if (Side(bodyRecords, e.bodyA, e.a, o.a) && Side(bodyRecords, e.bodyB, e.b, o.b))
                    stepEvents.contactBegin.push_back(o);
            }
            for (const auto& e : c.end)
            {
                ContactEnd o;
                if (Side(bodyRecords, e.bodyA, e.a, o.a) && Side(bodyRecords, e.bodyB, e.b, o.b))
                    stepEvents.contactEnd.push_back(o);
            }
            for (const auto& e : c.hit)
            {
                ContactHit o;
                if (!Side(bodyRecords, e.bodyA, e.a, o.a) || !Side(bodyRecords, e.bodyB, e.b, o.b)) continue;
                o.point  = glm::vec2(static_cast<float>(e.point.x), static_cast<float>(e.point.y));
                o.normal = glm::vec2(static_cast<float>(e.normal.x), static_cast<float>(e.normal.y));
                o.approachSpeed = static_cast<float>(e.approachSpeed);
                stepEvents.contactHit.push_back(o);
            }
            const Phys::SensorEvents s = world->GetSensorEvents();
            for (const auto& e : s.begin)
            {
                SensorBegin o;
                if (Side(bodyRecords, e.sensorBody, e.sensor, o.sensor) &&
                    Side(bodyRecords, e.visitorBody, e.visitor, o.visitor))
                    stepEvents.sensorBegin.push_back(o);
            }
            for (const auto& e : s.end)
            {
                SensorEnd o;
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

    Events World::StepEvents() const  { return stepEvents.View(); }
    Events World::FrameEvents() const { return frameEvents.View(); }
    void World::BeginFrame() { frameEvents.Clear(); }

    void World::ContactsOf(Arcane::ECS::Entity entity, std::vector<ContactPoint>& out) const
    {
        namespace Phys = Detail::Phys;
        out.clear();
        const auto it = entityToBody.find(entity);
        if (!world || it == entityToBody.end() || !world->IsValid(it->second)) return;
        std::vector<Phys::BodyContact> raw;
        world->GetBodyContacts(it->second, raw);
        for (const Phys::BodyContact& c : raw)
        {
            ContactPoint p;
            if (!Side(bodyRecords, c.selfBody, c.self, p.self) || !Side(bodyRecords, c.otherBody, c.other, p.other))
                continue;
            p.normal = glm::vec2(static_cast<float>(c.normal.x), static_cast<float>(c.normal.y));
            p.pointCount = static_cast<std::uint32_t>(c.pointCount);
            out.push_back(p);
        }
    }
}

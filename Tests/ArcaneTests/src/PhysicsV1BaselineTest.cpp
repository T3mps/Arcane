// 3D-readiness Plan A, Tasks 1-2: the v1 physics baselines. The step trace, the
// debug / inspector overlays and the collider pick list are captured once from
// the unmodified tree and checked in under Tests/ArcaneTests/data/physics/.
// Later refactors must reproduce those bytes. The capture cases are hidden
// ([.]) and armed by an environment switch.
#include <catch2/catch_test_macros.hpp>

#include "Helpers/ConstantScan.hpp"
#include "Helpers/PhysicsV1Baseline.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsEvents2D.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Sim/SimSettings.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace Arcane::Test
{
    namespace
    {
        std::string Hex8(float f)
        {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%08x", std::bit_cast<std::uint32_t>(f));
            return buf;
        }

        std::string Hex16(std::uint64_t v)
        {
            char buf[24];
            std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
            return buf;
        }

        std::string GuidText(const Arcane::Guid& g)
        {
            if (g.IsNil()) return "0";
            std::string s = g.ToString();
            s.erase(std::remove(s.begin(), s.end(), '-'), s.end());
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        std::string Side(const Arcane::ContactSide2D& s)
        {
            return std::to_string(s.entity.GetID()) + " " + std::to_string(s.entity.GetVersion()) + " "
                 + GuidText(s.guid) + " " + std::to_string(s.fixture);
        }

        template<typename Span, typename Fn>
        void Block(std::string& out, const char* name, const Span& events, Fn&& line)
        {
            out += name; out += ' '; out += std::to_string(events.size()); out += '\n';
            for (const auto& e : events) { out += line(e); out += '\n'; }
        }

        void AddBody(Arcane::Registry& reg, Arcane::Entity e, glm::vec3 pos, Arcane::BodyType2D type,
                     float mass, const Arcane::Fixture2D& fx)
        {
            Arcane::Transform t; t.position = pos;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
            Arcane::RigidBody2D rb; rb.type = type; rb.mass = mass;
            // Solver invariant (R8): a dynamic Aabb shape must be fixedRotation.
            if (type == Arcane::BodyType2D::Dynamic && fx.kind == Arcane::ShapeKind2D::Aabb) rb.fixedRotation = true;
            reg.AddComponent<Arcane::RigidBody2D>(e, rb);
            Arcane::Collider2D col; col.fixtures.push_back(fx);
            reg.AddComponent<Arcane::Collider2D>(e, col);
        }

        Arcane::Fixture2D DynFixture(Arcane::ShapeKind2D kind)
        {
            Arcane::Fixture2D f; f.kind = kind;
            f.isSensor = false; f.contactEvents = true; f.hitEvents = true; f.sensorEvents = false;
            return f;
        }

        void WriteSnapshot(const char* stem, const std::string& bytes)
        {
            const auto dir = RepoRoot() / "Tests/ArcaneTests/data/physics";
            fs::create_directories(dir);
            WriteFileBinary(dir / (std::string(stem) + ".txt"), bytes);
            WriteFileBinary(dir / (std::string(stem) + ".sha256"), Arcane::Crypto::Sha256Hex(bytes) + "\n");
        }

        bool CaptureArmed()
        {
            const char* arm = std::getenv("ARCANE_CAPTURE_PHYSICS_BASELINE");
            return arm && arm[0] == '1';
        }
    }

    std::string ReadFileBinary(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    void WriteFileBinary(const fs::path& path, const std::string& bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    V1PhysicsScene BuildV1PhysicsScene(Arcane::Registry& reg)
    {
        V1PhysicsScene s;
        {   // Floor
            Arcane::Fixture2D f = DynFixture(Arcane::ShapeKind2D::Aabb);
            f.halfW = 5.0f; f.halfH = 0.5f;
            s.entities[0] = reg.CreateEntity();
            AddBody(reg, s.entities[0], { 0.0f, -1.0f, 0.0f }, Arcane::BodyType2D::Static, 0.0f, f);
        }
        {   // Circle
            Arcane::Fixture2D f = DynFixture(Arcane::ShapeKind2D::Circle);
            f.radius = 0.5f;
            s.entities[1] = reg.CreateEntity();
            AddBody(reg, s.entities[1], { 0.0f, 2.0f, 0.0f }, Arcane::BodyType2D::Dynamic, 1.0f, f);
        }
        {   // Capsule
            Arcane::Fixture2D f = DynFixture(Arcane::ShapeKind2D::Capsule);
            f.radius = 0.2f; f.halfLen = 0.4f;
            s.entities[2] = reg.CreateEntity();
            AddBody(reg, s.entities[2], { 1.2f, 3.0f, 0.0f }, Arcane::BodyType2D::Dynamic, 1.0f, f);
        }
        {   // Box
            Arcane::Fixture2D f = DynFixture(Arcane::ShapeKind2D::Aabb);
            f.halfW = 0.3f; f.halfH = 0.25f;
            s.entities[3] = reg.CreateEntity();
            AddBody(reg, s.entities[3], { -0.8f, 2.5f, 0.0f }, Arcane::BodyType2D::Dynamic, 1.0f, f);
        }
        {   // Sensor
            Arcane::Fixture2D f = DynFixture(Arcane::ShapeKind2D::Circle);
            f.radius = 1.0f; f.isSensor = true; f.sensorEvents = true; f.contactEvents = false; f.hitEvents = false;
            s.entities[4] = reg.CreateEntity();
            AddBody(reg, s.entities[4], { 0.0f, 0.5f, 0.0f }, Arcane::BodyType2D::Static, 0.0f, f);
        }
        return s;
    }

    std::string WriteV1StepTrace(Arcane::Runtime& rt, int steps)
    {
        REQUIRE(Arcane::Settings<Arcane::SimSettings>().fixedHz == 60.0);
        std::string out = "arcane-physics-v1-step-trace 1\n";
        out += "steps " + std::to_string(steps) + "\n";
        out += "parallelSolver 0\n";
        auto& reg = rt.Registry();
        for (int step = 1; step <= steps; ++step)
        {
            rt.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
            out += "step " + std::to_string(step) + "\n";

            struct Row { std::uint32_t idx, gen; float x, y, angle; };
            std::vector<Row> rows;
            reg.CreateView<const Arcane::RigidBody2D, const Arcane::Transform>()
                .ForEach([&](Arcane::Entity e, const Arcane::RigidBody2D&, const Arcane::Transform& t) {
                    rows.push_back({ static_cast<std::uint32_t>(e.GetID()), static_cast<std::uint32_t>(e.GetVersion()),
                                     t.position.x, t.position.y, Arcane::RotationZ(t.rotation) });
                });
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                return a.idx != b.idx ? a.idx < b.idx : a.gen < b.gen; });
            out += "bodies " + std::to_string(rows.size()) + "\n";
            for (const Row& r : rows)
                out += std::to_string(r.idx) + " " + std::to_string(r.gen) + " " + Hex8(r.x) + " " + Hex8(r.y) + " " + Hex8(r.angle) + "\n";

            auto* w = reg.GetResource<Arcane::PhysicsWorld2D>();
            REQUIRE(w != nullptr);
            const Arcane::PhysicsEvents2D ev = w->StepEvents();
            const auto two = [](const auto& e) { return Side(e.a) + " " + Side(e.b); };
            Block(out, "contactBegin", ev.contactBegin, two);
            Block(out, "contactEnd", ev.contactEnd, two);
            Block(out, "contactHit", ev.contactHit, [&](const Arcane::ContactHit2D& e) {
                return two(e) + " " + Hex8(e.point.x) + " " + Hex8(e.point.y) + " " + Hex8(e.normal.x) + " "
                     + Hex8(e.normal.y) + " " + Hex8(e.approachSpeed); });
            const auto sv = [](const auto& e) { return Side(e.sensor) + " " + Side(e.visitor); };
            Block(out, "sensorBegin", ev.sensorBegin, sv);
            Block(out, "sensorEnd", ev.sensorEnd, sv);
        }
        return out;
    }

}

using namespace Arcane::Test;

TEST_CASE("v1 step trace matches the checked-in bytes", "[physics][baseline]")
{
    const auto path = RepoRoot() / "Tests/ArcaneTests/data/physics/v1-step-trace.txt";
    const auto hashPath = RepoRoot() / "Tests/ArcaneTests/data/physics/v1-step-trace.sha256";
    REQUIRE(fs::exists(path));
    const std::string disk = ReadFileBinary(path);
    std::string expect = ReadFileBinary(hashPath);
    if (!expect.empty() && expect.back() == '\n') expect.pop_back();
    CHECK(Arcane::Crypto::Sha256Hex(disk) == expect);

    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(rt.Registry());
    rt.EnsurePhysics();
    const std::string live = WriteV1StepTrace(rt, 120);
    CHECK(live == disk);
}

TEST_CASE("capture v1 step trace", "[.][physics-baseline]")
{
    if (!CaptureArmed()) return;
    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(rt.Registry());
    rt.EnsurePhysics();
    const std::string live = WriteV1StepTrace(rt, 120);
    WriteSnapshot("v1-step-trace", live);
}

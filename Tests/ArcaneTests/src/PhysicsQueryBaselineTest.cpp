// 3D-readiness Plan A, Task 6: the solver-free 2D physics query proxies. Each
// proxy result, dumped in the Task 2 line format, must equal the checked-in v1
// bytes. An empty result means the Physics2D namespace-scope registrar did not
// run, which is a failure here.
#include <catch2/catch_test_macros.hpp>

#include "Helpers/ConstantScan.hpp"
#include "Helpers/PhysicsV1Baseline.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Render/ExtractFrame.hpp>
#include <Arcane/Render/RenderFramePacket.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/PhysicsQuery.hpp>
#include <Arcane/Scene/SceneResources.hpp>       // RenderContext2D

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace
{
    using namespace Arcane::Test;

    std::string Hex8(float f)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08x", std::bit_cast<std::uint32_t>(f));
        return buf;
    }

    const Arcane::Affine2D kOrdinary{ { 640.0f, 360.0f }, { 100.0f, -100.0f } };
    const Arcane::Affine2D kAlt{ { 100.0f, 80.0f }, { 50.0f, -40.0f } };

    std::string Disk(const char* file)
    {
        return ReadFileBinary(RepoRoot() / "Tests/ArcaneTests/data/physics" / file);
    }

    std::string HeaderOf(const std::string& text)
    {
        return text.substr(0, text.find('\n'));
    }

    // Task 2's line format: the checked-in file's header line, then one line per primitive.
    std::string DumpPhysicsDebug(const std::string& header, const Arcane::PhysicsDebugGeometry& g)
    {
        std::string out = header; out += '\n';
        for (const auto& p : g.primitives)
        {
            if (p.kind == Arcane::PhysicsDebugPrimitive::Kind::Line)
                out += "line " + Hex8(p.a.x) + " " + Hex8(p.a.y) + " " + Hex8(p.b.x) + " " + Hex8(p.b.y) + " "
                     + Hex8(p.size) + " " + Hex8(p.color.r) + " " + Hex8(p.color.g) + " " + Hex8(p.color.b) + " "
                     + Hex8(p.color.a) + "\n";
            else
                out += "circle " + Hex8(p.a.x) + " " + Hex8(p.a.y) + " " + Hex8(p.size) + " "
                     + Hex8(p.color.r) + " " + Hex8(p.color.g) + " " + Hex8(p.color.b) + " " + Hex8(p.color.a) + "\n";
        }
        return out;
    }

    V1PhysicsScene StepScene(Arcane::Runtime& rt, int steps = 120)
    {
        const V1PhysicsScene scene = BuildV1PhysicsScene(rt.Registry());
        rt.EnsurePhysics();
        for (int i = 0; i < steps; ++i)
            rt.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
        return scene;
    }

    Arcane::PhysicsDebugExtractRequest2D WholeWorldRequest()
    {
        Arcane::PhysicsDebugExtractRequest2D r;
        r.view = kOrdinary;
        r.alpha = 0.0f;
        r.physicsWholeWorld = true;
        return r;
    }

    Arcane::PhysicsDebugExtractRequest2D SelectedRequest(Arcane::Entity selected)
    {
        Arcane::PhysicsDebugExtractRequest2D r;
        r.view = kOrdinary;
        r.alpha = 0.0f;
        r.selected = selected;
        r.physicsWholeWorld = false;
        return r;
    }

    Arcane::PhysicsDebugExtractRequest2D AltViewRequest()
    {
        Arcane::PhysicsDebugExtractRequest2D r;
        r.view = kAlt;
        r.alpha = 0.25f;
        r.physicsWholeWorld = true;
        return r;
    }

    std::uint64_t ParseHex(const std::string& header, const char* key)
    {
        const std::string k = std::string(key) + " ";
        const auto at = header.find(k);
        REQUIRE(at != std::string::npos);
        return std::strtoull(header.c_str() + at + k.size(), nullptr, 16);
    }

    Arcane::PhysicsDebugExtractRequest2D InspectorRequest(const std::string& header)
    {
        Arcane::PhysicsDebugExtractRequest2D r;
        r.view = kOrdinary;
        r.physicsWholeWorld = false;   // the ordinary gather stays empty; the file holds the overlay only
        r.inspectorFixtureA = ParseHex(header, "fixtureA");
        r.inspectorFixtureB = ParseHex(header, "fixtureB");
        r.inspectorStepIndex = -1;
        r.inspectorThickness = std::nullopt;   // header: thickness none
        r.inspectorEmphasis = 1.0f;            // header: emphasis 1
        return r;
    }

    void CheckDebug(const char* file, const Arcane::Registry& reg, const Arcane::PhysicsDebugExtractRequest2D& request)
    {
        const std::string disk = Disk(file);
        REQUIRE_FALSE(disk.empty());
        const auto geo = Arcane::FillPhysicsDebug2D(reg, request);
        CHECK_FALSE(geo.primitives.empty());   // empty means the namespace-scope registrar did not run
        CHECK(DumpPhysicsDebug(HeaderOf(disk), geo) == disk);
    }

    std::string DumpSilhouettes(const std::vector<Arcane::PhysicsColliderSilhouette2D>& list)
    {
        std::string out = "arcane-physics-v1-pick 1\n";
        for (const auto& d : list)
        {
            const std::string who = std::to_string(d.entity.GetID()) + " " + std::to_string(d.entity.GetVersion());
            const std::string c = Hex8(d.center.x) + " " + Hex8(d.center.y) + " " + Hex8(d.center.z);
            switch (d.kind)
            {
            case Arcane::PhysicsColliderSilhouette2D::Kind::Circle:
                out += "circle " + who + " " + c + " " + Hex8(d.radius) + " " + Hex8(d.angle) + "\n"; break;
            case Arcane::PhysicsColliderSilhouette2D::Kind::Capsule:
                out += "capsule " + who + " " + c + " " + Hex8(d.halfLen) + " " + Hex8(d.radius) + " " + Hex8(d.angle) + "\n"; break;
            case Arcane::PhysicsColliderSilhouette2D::Kind::Box:
                out += "box " + who + " " + c + " " + Hex8(d.halfExtents.x) + " " + Hex8(d.halfExtents.y) + " " + Hex8(d.angle) + "\n"; break;
            }
        }
        return out;
    }
}

TEST_CASE("two debug requests do not share one list", "[physics][proxy]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    StepScene(rt);
    const std::string wholeHeader = HeaderOf(Disk("v1-debug-wholeworld.txt"));
    const std::string whole = DumpPhysicsDebug(wholeHeader, Arcane::FillPhysicsDebug2D(rt.Registry(), WholeWorldRequest()));
    const std::string alt = DumpPhysicsDebug(wholeHeader, Arcane::FillPhysicsDebug2D(rt.Registry(), AltViewRequest()));
    CHECK(whole == Disk("v1-debug-wholeworld.txt"));
    CHECK(whole != alt);
    CHECK_FALSE(whole.empty());
    CHECK(whole.find('\n') + 1 < whole.size());   // primitives, not just the header
}

TEST_CASE("the whole-world proxy matches the v1 bytes", "[physics][proxy]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    StepScene(rt);
    CheckDebug("v1-debug-wholeworld.txt", rt.Registry(), WholeWorldRequest());
}

TEST_CASE("the selected-only proxy matches the v1 bytes", "[physics][proxy]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    const auto scene = StepScene(rt);
    CheckDebug("v1-debug-selected.txt", rt.Registry(), SelectedRequest(scene.Circle()));
}

TEST_CASE("the alt-view proxy matches the v1 bytes", "[physics][proxy]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    StepScene(rt, 20);
    CheckDebug("v1-debug-altview.txt", rt.Registry(), AltViewRequest());
}

TEST_CASE("the inspector proxy matches the v1 bytes", "[physics][proxy]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    StepScene(rt);
    CheckDebug("v1-inspector.txt", rt.Registry(), InspectorRequest(HeaderOf(Disk("v1-inspector.txt"))));
}

TEST_CASE("a perspective view draws no physics debug", "[physics][proxy]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    StepScene(rt);
    Arcane::PhysicsDebugExtractRequest2D request = WholeWorldRequest();
    request.view = std::nullopt;
    CHECK(Arcane::FillPhysicsDebug2D(rt.Registry(), request).primitives.empty());
}

TEST_CASE("play mode with no whole-world draws no selected outline", "[physics][proxy]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    const auto scene = StepScene(rt);
    Arcane::PhysicsDebugExtractRequest2D request = SelectedRequest(scene.Circle());
    request.playMode = true;
    CHECK(Arcane::FillPhysicsDebug2D(rt.Registry(), request).primitives.empty());
}

TEST_CASE("the silhouette proxy matches the v1 pick bytes", "[physics][proxy]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    StepScene(rt);
    std::vector<Arcane::PhysicsColliderSilhouette2D> list;
    Arcane::FillColliderSilhouettes2D(rt.Registry(), list);
    CHECK_FALSE(list.empty());
    CHECK(DumpSilhouettes(list) == Disk("v1-pick.txt"));

    // Append, do not clear: the same contract as CollectPickables.
    const std::size_t n = list.size();
    Arcane::FillColliderSilhouettes2D(rt.Registry(), list);
    CHECK(list.size() == 2 * n);
}

TEST_CASE("a second 2D query registration is refused", "[physics][proxy]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    StepScene(rt);
    constexpr Arcane::PhysicsDebugFillFn2D fake =
        [](const Astra::Registry&, const Arcane::PhysicsDebugExtractRequest2D&) { return Arcane::PhysicsDebugGeometry{}; };
    CHECK_FALSE(Arcane::SetPhysicsQueryFns2D(fake, nullptr));
    CheckDebug("v1-debug-wholeworld.txt", rt.Registry(), WholeWorldRequest());
}

TEST_CASE("PhysicsQuery.hpp names no solver type", "[physics][proxy]")
{
    const std::string text = ReadFileBinary(Arcane::Test::RepoRoot() /
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsQuery.hpp");
    REQUIRE_FALSE(text.empty());
    for (const char* bad : { "Manifold2D", "Manifold", "NarrowphaseTrace", "PhysicsWorld", "BodyHandle", "FixtureHandle" })
        CHECK(text.find(bad) == std::string::npos);
}

// Task 7: Extract reaches physics only through the proxy, and Render has no physics edge.
TEST_CASE("ArcaneRender includes no physics solver header", "[physics][proxy]")
{
    const auto root = Arcane::Test::RepoRoot() / "Source/Client/ArcaneRender";
    const std::vector<std::string> banned = {
        "Physics2DDetail.hpp", "Physics2DHandles.hpp", "PhysicsSystem.hpp",
        "PhysicsDebugGeometry.hpp", "Manifold2D/"
    };
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension();
        if (ext != ".hpp" && ext != ".cpp" && ext != ".inl") continue;
        const std::string text = ReadFileBinary(entry.path());
        for (const auto& ban : banned)
        {
            INFO(entry.path().string() << " names " << ban);
            CHECK(text.find(ban) == std::string::npos);
        }
    }
    const std::string module = ReadFileBinary(root / "ArcaneRender.arcmodule");
    CHECK(module.find("ArcanePhysics2D") == std::string::npos);
    const auto sceneTypes = Arcane::Test::RepoRoot() /
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsDebugTypes.hpp";
    const auto physicsTypes = Arcane::Test::RepoRoot() /
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugTypes.hpp";
    CHECK(std::filesystem::exists(sceneTypes));
    CHECK_FALSE(std::filesystem::exists(physicsTypes));
    CHECK(std::filesystem::exists(Arcane::Test::RepoRoot() /
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsDebugDrawOptions.hpp"));
}

TEST_CASE("ExtractFrame's physics overlay equals the v1 whole-world bytes", "[physics][proxy][extract]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    StepScene(rt);

    // The v1 view is Affine2D{ {640, 360}, {100, -100} }. An ordinary orthographic view cannot
    // reproduce it bit for bit at height 720 (AsAffine2D's float chain lands one ulp off 100),
    // so this one is built from exactly representable terms: 1280x800 viewport, projection
    // x scale 0.15625 and y scale 0.25 (100 px per metre) and y shift 0.1 (offset.y 360).
    Arcane::RenderContext2D ctx;
    ctx.view.viewport = { 1280u, 800u };
    ctx.view.projection[0][0] = 0.15625f;
    ctx.view.projection[1][1] = 0.25f;
    ctx.view.projection[3][1] = 0.1f;
    ctx.alpha = 0.0f;
    rt.Registry().SetResource<Arcane::RenderContext2D>(std::move(ctx));

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 1280u, 800u };
    req.physicsDebug = true;
    req.physicsWholeWorld = true;
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(rt.Registry(), req, ring);

    REQUIRE(packet.spriteView.has_value());
    const auto affine = packet.spriteView->view.AsAffine2D();
    REQUIRE(affine.has_value());
    REQUIRE(affine->offset == kOrdinary.offset);   // the view must BE the v1 view, or the bytes cannot match
    REQUIRE(affine->scale == kOrdinary.scale);

    const std::string disk = Disk("v1-debug-wholeworld.txt");
    REQUIRE_FALSE(disk.empty());
    std::string out = HeaderOf(disk); out += '\n';
    for (const Arcane::OverlayPrimitive& p : packet.overlay.prims)
    {
        if (p.tag == Arcane::OverlayPrimitive::Tag::Line)
        {
            const auto& l = p.line;
            CHECK(l.screenSpace);
            CHECK(l.a.z == 0.0f);
            out += "line " + Hex8(l.a.x) + " " + Hex8(l.a.y) + " " + Hex8(l.b.x) + " " + Hex8(l.b.y) + " "
                 + Hex8(l.thickness) + " " + Hex8(l.color.r) + " " + Hex8(l.color.g) + " " + Hex8(l.color.b) + " "
                 + Hex8(l.color.a) + "\n";
        }
        else
        {
            const auto& f = p.fill;
            CHECK(f.kind == Arcane::OverlayPrimKind::Circle);
            out += "circle " + Hex8(f.a.x) + " " + Hex8(f.a.y) + " " + Hex8(f.radius) + " "
                 + Hex8(f.color.r) + " " + Hex8(f.color.g) + " " + Hex8(f.color.b) + " " + Hex8(f.color.a) + "\n";
        }
    }
    CHECK(packet.overlay.prims.size() > 1);
    CHECK(out == disk);
}

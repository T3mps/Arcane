// The editor golden run's two viewport pins (NRI Phase 3, Task 13 follow-up):
// which camera it captures through, and which entity it outlines. Both are
// pure registry->answer functions in ArcaneEditor/src/Viewport/GoldenViewPin.hpp
// precisely so they can be driven here -- EditorApp itself is not compiled into
// this exe, and a green [gpu]-less gate proves nothing about either host, so
// the decision half is deliberately where a headless unit can reach it.
//
// The properties worth pinning are the ones a broken pin would silently trade
// away, both learned from the FIRST D3c capture:
//   * the camera must FRAME THE CONTENT for the viewport it is actually
//     capturing at (that capture was fitted to 1280x720 and cropped to a
//     654x330 panel: ~90% flat background, subject off-frame);
//   * the selection must be HIT-PROXY ID 1 -- the same handle the runtime's
//     --pick-probe fabricates -- because the outline chain seeds off the id
//     pass, and any second way of naming "the first entity" is a second
//     ordering rule that can drift from it.

#include <cmath>
#include <memory>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/mat3x3.hpp>
#include <glm/vec2.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Viewport/GoldenViewPin.hpp>

using Catch::Approx;
using Arcane::Editor::EditorCamera;

namespace
{
    // Same fixture shape as EditorCameraTest.cpp's -- a registry with the
    // shared Scene components registered.
    std::unique_ptr<Astra::Registry> MakeSceneRegistry()
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        auto reg = std::make_unique<Astra::Registry>(components);
        Arcane::RegisterSceneComponents(*reg);
        return reg;
    }

    glm::mat3 WorldMat(glm::vec2 pos, glm::vec2 scale)
    {
        glm::mat3 m(1.0f);
        m[0] = glm::vec3(scale.x, 0.0f, 0.0f);
        m[1] = glm::vec3(0.0f, scale.y, 0.0f);
        m[2] = glm::vec3(pos.x, pos.y, 1.0f);
        return m;
    }

    // A sprite entity with a materialised WorldTransform: what the editor
    // holds by the time the pin runs (RefreshSceneResolution propagates
    // transforms immediately before it).
    Astra::Entity MakeSprite(Astra::Registry& reg, glm::vec2 pos, glm::vec2 size)
    {
        Astra::Entity e = reg.CreateEntity();
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{WorldMat(pos, size)});
        reg.AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});
        return e;
    }
}

TEST_CASE("Golden camera pin frames the whole scene into the capture extent", "[editor]")
{
    auto reg = MakeSceneRegistry();
    // A scene wider than it is tall, like ReferenceProject's: the fit is
    // width-bound and the vertical result is what says "centred", not "in a
    // corner".
    MakeSprite(*reg, glm::vec2(-4.0f, 0.5f), glm::vec2(2.0f, 1.0f));
    MakeSprite(*reg, glm::vec2(5.0f, -1.5f), glm::vec2(3.0f, 2.0f));

    // The DOCKED panel extent, not the 1280x720 boot default -- the whole
    // point of re-deriving the pin per frame.
    const glm::vec2 viewport(654.0f, 330.0f);
    const auto cam = Arcane::Editor::GoldenPinnedCamera(*reg, viewport);
    REQUIRE(cam.has_value());

    const Arcane::Editor::FramingBounds b = Arcane::Editor::SceneFramingBounds(*reg);
    REQUIRE(b.Valid());
    for (glm::vec2 corner : { b.min, b.max, glm::vec2(b.min.x, b.max.y), glm::vec2(b.max.x, b.min.y) })
    {
        const glm::vec2 s = cam->WorldToScreen(corner);
        CHECK(s.x >= 0.0f);
        CHECK(s.y >= 0.0f);
        CHECK(s.x <= viewport.x);
        CHECK(s.y <= viewport.y);
    }

    // ...and it is genuinely FRAMED, not merely on-screen: the content spans
    // the fitted axis (kFrameFill = 0.9) rather than sitting in a corner of a
    // mostly-empty capture, which is the defect the pin exists to close.
    const glm::vec2 lo = cam->WorldToScreen(b.min);
    const glm::vec2 hi = cam->WorldToScreen(b.max);
    CHECK(std::abs(hi.x - lo.x) == Approx(viewport.x * EditorCamera::kFrameFill));
    // Centred on both axes: equal margins.
    CHECK(lo.x == Approx(viewport.x - hi.x));
    CHECK(lo.y == Approx(viewport.y - hi.y));
}

TEST_CASE("Golden camera pin is a pure function of scene and extent", "[editor]")
{
    // No dependence on any live camera state: the pin builds its answer from a
    // default EditorCamera every time, so a single-point scene (zero extent,
    // where EditorCamera::Frame leaves zoom alone) still lands on the SAME
    // zoom rather than inheriting whatever the session held.
    auto reg = MakeSceneRegistry();
    const glm::vec2 viewport(654.0f, 330.0f);

    SECTION("an empty scene pins nothing -- the caller must leave the camera alone")
    {
        CHECK_FALSE(Arcane::Editor::GoldenPinnedCamera(*reg, viewport).has_value());
    }

    SECTION("a zero-extent scene still lands on the default zoom, centred")
    {
        // A zero-SIZE sprite is the degenerate box: Frame can derive no scale
        // from it. The answer must still be total.
        Astra::Entity e = reg->CreateEntity();
        reg->AddComponent<Arcane::WorldTransform>(
            e, Arcane::WorldTransform{WorldMat(glm::vec2(3.0f, -2.0f), glm::vec2(0.0f))});
        reg->AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});

        const auto cam = Arcane::Editor::GoldenPinnedCamera(*reg, viewport);
        REQUIRE(cam.has_value());
        CHECK(cam->zoom == Approx(EditorCamera::kDefaultZoom));
        const glm::vec2 s = cam->WorldToScreen(glm::vec2(3.0f, -2.0f));
        CHECK(s.x == Approx(viewport.x * 0.5f));
        CHECK(s.y == Approx(viewport.y * 0.5f));
    }

    SECTION("the same scene at the same extent answers identically")
    {
        MakeSprite(*reg, glm::vec2(0.0f, 0.0f), glm::vec2(2.0f, 2.0f));
        MakeSprite(*reg, glm::vec2(7.0f, 3.0f), glm::vec2(1.0f, 1.0f));
        const auto a = Arcane::Editor::GoldenPinnedCamera(*reg, viewport);
        const auto b = Arcane::Editor::GoldenPinnedCamera(*reg, viewport);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(a->zoom == Approx(b->zoom));
        CHECK(a->offset.x == Approx(b->offset.x));
        CHECK(a->offset.y == Approx(b->offset.y));
    }
}

TEST_CASE("Golden selection pin IS hit-proxy id 1", "[editor]")
{
    // The contract that keeps the two render arms agreeing: whatever the pin
    // selects, the id pass must call it 1. Asserted THROUGH the emitter both
    // arms feed (CollectPickables), not against a hand-written expectation, so
    // a change to the id assignment breaks this test rather than silently
    // moving the outline to a different entity on one arm.
    auto reg = MakeSceneRegistry();
    const Astra::Entity first  = MakeSprite(*reg, glm::vec2(0.0f, 0.0f), glm::vec2(10.0f, 0.5f));
    const Astra::Entity second = MakeSprite(*reg, glm::vec2(-1.0f, -0.5f), glm::vec2(1.0f, 1.0f));
    MakeSprite(*reg, glm::vec2(1.0f, -0.75f), glm::vec2(1.0f, 1.0f));

    const Astra::Entity pinned = Arcane::Editor::GoldenPinnedSelection(*reg);
    REQUIRE(pinned.IsValid());
    CHECK(pinned == first);
    CHECK(pinned != second);

    std::vector<Arcane::PickDrawable> drawables;
    Arcane::CollectPickables(*reg, Arcane::PickView{}, drawables);
    CHECK(Arcane::PickPassIdOf(drawables, pinned) == 1u);
    CHECK(Arcane::PickEntityForId(drawables, 1u) == pinned);
}

TEST_CASE("Golden selection pin is view-independent and total", "[editor]")
{
    auto reg = MakeSceneRegistry();

    SECTION("nothing pickable selects nothing -- an empty scene has no honest outline")
    {
        CHECK_FALSE(Arcane::Editor::GoldenPinnedSelection(*reg).IsValid());
        // A bare transform node is not pickable either.
        const Astra::Entity node = reg->CreateEntity();
        reg->AddComponent<Arcane::WorldTransform>(
            node, Arcane::WorldTransform{WorldMat(glm::vec2(0.0f), glm::vec2(1.0f))});
        CHECK_FALSE(Arcane::Editor::GoldenPinnedSelection(*reg).IsValid());
    }

    SECTION("the answer does not move with the camera")
    {
        // The pin runs before the frame's camera is known to anything else, and
        // CollectPickables projects geometry through a view -- so the ANSWER
        // must not depend on which view. Same registry, wildly different
        // views, same entity.
        const Astra::Entity first = MakeSprite(*reg, glm::vec2(0.0f), glm::vec2(2.0f));
        MakeSprite(*reg, glm::vec2(40.0f, 40.0f), glm::vec2(2.0f));

        std::vector<Arcane::PickDrawable> zoomedOut;
        Arcane::CollectPickables(*reg, Arcane::PickView{glm::vec2(600.0f, 300.0f), 12.5f}, zoomedOut);
        std::vector<Arcane::PickDrawable> zoomedIn;
        Arcane::CollectPickables(*reg, Arcane::PickView{glm::vec2(-90.0f, 7.0f), 940.0f}, zoomedIn);

        REQUIRE(zoomedOut.size() == zoomedIn.size());
        CHECK(zoomedOut.front().entity == zoomedIn.front().entity);
        CHECK(Arcane::Editor::GoldenPinnedSelection(*reg) == first);
    }
}

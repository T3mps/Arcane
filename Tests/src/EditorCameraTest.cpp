// Arcane Editor viewport camera: pure pan / anchored-zoom / framing math, plus
// the framing-bounds sweep over the scene registry. CPU-only ([editor]).
//
// The camera convention under test is the engine's: screen = world * zoom +
// offset, with zoom in PIXELS PER METRE (world is MKS). Framing bounds must
// agree with RenderSubmissionSystem's sprite placement -- if they disagree,
// "frame selected" puts the thing off screen.

#include <cmath>
#include <memory>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/mat3x3.hpp>
#include <glm/vec2.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <EditorCamera.hpp>

using Catch::Approx;
using Arcane::Editor::EditorCamera;

namespace
{
    // Fresh registry with the shared Scene components registered (same fixture
    // shape as EditorEntityListTest.cpp).
    std::unique_ptr<Astra::Registry> MakeSceneRegistry()
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        auto reg = std::make_unique<Astra::Registry>(components);
        Arcane::RegisterSceneComponents(*reg);
        return reg;
    }

    // A non-rotating world matrix: basis columns carry the scale, m[2] the
    // translation (matches Transform::ToMatrix, which RenderSubmissionSystem
    // decomposes the same way).
    glm::mat3 WorldMat(glm::vec2 pos, glm::vec2 scale)
    {
        glm::mat3 m(1.0f);
        m[0] = glm::vec3(scale.x, 0.0f, 0.0f);
        m[1] = glm::vec3(0.0f, scale.y, 0.0f);
        m[2] = glm::vec3(pos.x, pos.y, 1.0f);
        return m;
    }

    // Sprite entity with a materialised WorldTransform (the editor refreshes
    // these before framing; the test sets them directly).
    Astra::Entity MakeSprite(Astra::Registry& reg, glm::vec2 pos, glm::vec2 size,
                             glm::vec2 scale = glm::vec2(1.0f))
    {
        Astra::Entity e = reg.CreateEntity();
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{WorldMat(pos, scale)});
        Arcane::SpriteRenderer sr;
        sr.size = size;
        reg.AddComponent<Arcane::SpriteRenderer>(e, sr);
        return e;
    }
}

TEST_CASE("EditorCamera defaults to 100 px per metre at the origin", "[editor]")
{
    // MKS content must be visible the moment a project opens, without the game
    // module ever calling SetCamera -- zoom 1 would draw a 1 m body as 1 px.
    const EditorCamera cam;
    CHECK(cam.zoom == Approx(100.0f));
    CHECK(cam.offset.x == Approx(0.0f));
    CHECK(cam.offset.y == Approx(0.0f));
    CHECK(cam.zoom == Approx(EditorCamera::kDefaultZoom));
}

TEST_CASE("EditorCamera::Pan translates by the screen delta", "[editor]")
{
    EditorCamera cam;
    const glm::vec2 world(2.0f, -3.0f);
    const glm::vec2 before = cam.WorldToScreen(world);

    cam.Pan(glm::vec2(25.0f, -10.0f));

    CHECK(cam.offset.x == Approx(25.0f));
    CHECK(cam.offset.y == Approx(-10.0f));
    // Content follows the drag 1:1 in screen px (the "grab the scene" feel).
    CHECK(cam.WorldToScreen(world).x == Approx(before.x + 25.0f));
    CHECK(cam.WorldToScreen(world).y == Approx(before.y - 10.0f));
    CHECK(cam.zoom == Approx(100.0f));   // pan never rescales

    cam.Pan(glm::vec2(-5.0f, 5.0f));     // deltas accumulate
    CHECK(cam.offset.x == Approx(20.0f));
    CHECK(cam.offset.y == Approx(-5.0f));
}

TEST_CASE("EditorCamera::ZoomAt keeps the world point under the cursor fixed", "[editor]")
{
    EditorCamera cam;
    cam.offset = glm::vec2(-40.0f, 90.0f);   // deliberately not centred

    const glm::vec2 cursor(317.0f, 208.0f);
    const glm::vec2 anchored = cam.ScreenToWorld(cursor);

    SECTION("zooming in")
    {
        const float z0 = cam.zoom;
        cam.ZoomAt(cursor, 3.0f);
        CHECK(cam.zoom > z0);
        // The round trip is the actual requirement: the same world point must
        // still land on the same pixel.
        CHECK(cam.WorldToScreen(anchored).x == Approx(cursor.x).margin(1e-3));
        CHECK(cam.WorldToScreen(anchored).y == Approx(cursor.y).margin(1e-3));
    }

    SECTION("zooming out")
    {
        const float z0 = cam.zoom;
        cam.ZoomAt(cursor, -2.0f);
        CHECK(cam.zoom < z0);
        CHECK(cam.WorldToScreen(anchored).x == Approx(cursor.x).margin(1e-3));
        CHECK(cam.WorldToScreen(anchored).y == Approx(cursor.y).margin(1e-3));
    }

    SECTION("a zero-tick wheel changes nothing")
    {
        const EditorCamera before = cam;
        cam.ZoomAt(cursor, 0.0f);
        CHECK(cam.zoom == before.zoom);
        CHECK(cam.offset.x == before.offset.x);
        CHECK(cam.offset.y == before.offset.y);
    }
}

TEST_CASE("EditorCamera::ZoomAt clamps at both ends", "[editor]")
{
    const glm::vec2 cursor(400.0f, 300.0f);

    EditorCamera in;
    in.ZoomAt(cursor, 10000.0f);          // far past the ceiling in one go
    CHECK(in.zoom == Approx(EditorCamera::kMaxZoom));

    EditorCamera out;
    out.ZoomAt(cursor, -10000.0f);        // and past the floor
    CHECK(out.zoom == Approx(EditorCamera::kMinZoom));

    // Repeated notches at the clamp must not drift the offset -- the pinned
    // camera would otherwise creep every frame the wheel is spun.
    const glm::vec2 pinned = in.offset;
    for (int i = 0; i < 8; ++i)
        in.ZoomAt(cursor, 5.0f);
    CHECK(in.zoom == Approx(EditorCamera::kMaxZoom));
    CHECK(in.offset.x == Approx(pinned.x));
    CHECK(in.offset.y == Approx(pinned.y));

    // Many small notches accumulate to the same clamp, never past it.
    EditorCamera step;
    for (int i = 0; i < 500; ++i)
        step.ZoomAt(cursor, 1.0f);
    CHECK(step.zoom == Approx(EditorCamera::kMaxZoom));
    CHECK(EditorCamera::kMinZoom > 0.0f);   // ScreenToWorld can never divide by zero
    CHECK(EditorCamera::kMinZoom < EditorCamera::kDefaultZoom);
    CHECK(EditorCamera::kMaxZoom > EditorCamera::kDefaultZoom);
}

TEST_CASE("EditorCamera ScreenToWorld and WorldToScreen are inverses", "[editor]")
{
    EditorCamera cam;
    cam.zoom   = 250.0f;
    cam.offset = glm::vec2(37.0f, -19.0f);

    const glm::vec2 screen(123.5f, 456.25f);
    const glm::vec2 world = cam.ScreenToWorld(screen);
    const glm::vec2 back  = cam.WorldToScreen(world);
    CHECK(back.x == Approx(screen.x).margin(1e-3));
    CHECK(back.y == Approx(screen.y).margin(1e-3));

    const glm::vec2 w2(-4.25f, 8.75f);
    const glm::vec2 s2 = cam.WorldToScreen(w2);
    const glm::vec2 w2back = cam.ScreenToWorld(s2);
    CHECK(w2back.x == Approx(w2.x).margin(1e-5));
    CHECK(w2back.y == Approx(w2.y).margin(1e-5));

    // The convention itself: screen = world * zoom + offset.
    CHECK(s2.x == Approx(w2.x * cam.zoom + cam.offset.x));
    CHECK(s2.y == Approx(w2.y * cam.zoom + cam.offset.y));
}

TEST_CASE("EditorCamera::Frame centres an AABB and fits it with a margin", "[editor]")
{
    EditorCamera cam;
    const glm::vec2 mn(-2.0f, -1.0f), mx(2.0f, 1.0f);   // 4 x 2 m
    const glm::vec2 viewport(800.0f, 600.0f);

    cam.Frame(mn, mx, viewport);

    // The tighter axis wins: 800 * fill / 4 m == 180 px/m beats 600 * fill / 2 m.
    const float expectZoom = viewport.x * EditorCamera::kFrameFill / 4.0f;
    CHECK(cam.zoom == Approx(expectZoom));

    // The AABB centre lands on the viewport centre.
    const glm::vec2 centre = cam.WorldToScreen((mn + mx) * 0.5f);
    CHECK(centre.x == Approx(viewport.x * 0.5f));
    CHECK(centre.y == Approx(viewport.y * 0.5f));

    // ...and the whole box is inside the viewport, with the margin actually
    // left over on the fitted axis.
    const glm::vec2 lo = cam.WorldToScreen(mn);
    const glm::vec2 hi = cam.WorldToScreen(mx);
    CHECK(lo.x > 0.0f);
    CHECK(lo.y > 0.0f);
    CHECK(hi.x < viewport.x);
    CHECK(hi.y < viewport.y);
    CHECK((hi.x - lo.x) == Approx(viewport.x * EditorCamera::kFrameFill));
    CHECK(EditorCamera::kFrameFill < 1.0f);       // there IS a margin
    CHECK(EditorCamera::kFrameFill > 0.0f);
}

TEST_CASE("EditorCamera::Frame handles the degenerate AABB and viewport", "[editor]")
{
    SECTION("a zero-size AABB (single point entity) centres without rescaling")
    {
        EditorCamera cam;
        const float z0 = cam.zoom;
        const glm::vec2 p(5.0f, -3.0f);
        cam.Frame(p, p, glm::vec2(800.0f, 600.0f));

        CHECK(cam.zoom == Approx(z0));            // a point cannot imply a scale
        CHECK(std::isfinite(cam.offset.x));
        CHECK(std::isfinite(cam.offset.y));
        CHECK(cam.WorldToScreen(p).x == Approx(400.0f));
        CHECK(cam.WorldToScreen(p).y == Approx(300.0f));
    }

    SECTION("one degenerate axis still fits on the other")
    {
        EditorCamera cam;
        const glm::vec2 mn(-2.0f, 0.0f), mx(2.0f, 0.0f);   // a horizontal row
        cam.Frame(mn, mx, glm::vec2(800.0f, 600.0f));
        CHECK(cam.zoom == Approx(800.0f * EditorCamera::kFrameFill / 4.0f));
        CHECK(cam.WorldToScreen(glm::vec2(0.0f, 0.0f)).x == Approx(400.0f));
        CHECK(cam.WorldToScreen(glm::vec2(0.0f, 0.0f)).y == Approx(300.0f));
    }

    SECTION("a zero-size viewport leaves the camera untouched")
    {
        EditorCamera cam;
        const EditorCamera before = cam;
        cam.Frame(glm::vec2(-1.0f), glm::vec2(1.0f), glm::vec2(0.0f, 0.0f));
        CHECK(cam.zoom == before.zoom);
        CHECK(cam.offset.x == before.offset.x);
        CHECK(cam.offset.y == before.offset.y);

        cam.Frame(glm::vec2(-1.0f), glm::vec2(1.0f), glm::vec2(800.0f, 0.0f));
        CHECK(cam.zoom == before.zoom);
        CHECK(cam.offset.x == before.offset.x);
        CHECK(cam.offset.y == before.offset.y);
    }

    SECTION("framing a huge AABB still clamps the zoom into range")
    {
        EditorCamera cam;
        cam.Frame(glm::vec2(-1.0e6f), glm::vec2(1.0e6f), glm::vec2(800.0f, 600.0f));
        CHECK(cam.zoom >= EditorCamera::kMinZoom);
        CHECK(cam.zoom <= EditorCamera::kMaxZoom);
    }
}

TEST_CASE("Framing bounds match how sprites are rendered", "[editor]")
{
    auto reg = MakeSceneRegistry();
    // World size = SpriteRenderer.size * world scale, centred on the world
    // position -- exactly RenderSubmissionSystem's dstSize/dstPos derivation.
    const Astra::Entity a = MakeSprite(*reg, glm::vec2(3.0f, 4.0f), glm::vec2(2.0f, 1.0f));
    const std::vector<Astra::Entity> one{a};

    const Arcane::Editor::FramingBounds b =
        Arcane::Editor::SelectionFramingBounds(*reg, one);
    REQUIRE(b.Valid());
    CHECK(b.count == 1);
    CHECK(b.min.x == Approx(2.0f));
    CHECK(b.min.y == Approx(3.5f));
    CHECK(b.max.x == Approx(4.0f));
    CHECK(b.max.y == Approx(4.5f));

    // A scaled sprite grows by its world scale, same as the drawn quad.
    const Astra::Entity s = MakeSprite(*reg, glm::vec2(-1.0f, 0.0f), glm::vec2(2.0f, 2.0f),
                                       glm::vec2(2.0f, 2.0f));
    const std::vector<Astra::Entity> two{a, s};
    const Arcane::Editor::FramingBounds u =
        Arcane::Editor::SelectionFramingBounds(*reg, two);
    REQUIRE(u.Valid());
    CHECK(u.count == 2);
    CHECK(u.min.x == Approx(-3.0f));
    CHECK(u.min.y == Approx(-2.0f));
    CHECK(u.max.x == Approx(4.0f));
    CHECK(u.max.y == Approx(4.5f));
}

TEST_CASE("Framing bounds distinguish nothing-to-frame from an empty AABB", "[editor]")
{
    auto reg = MakeSceneRegistry();

    SECTION("an empty selection is not framable")
    {
        const Arcane::Editor::FramingBounds b =
            Arcane::Editor::SelectionFramingBounds(*reg, {});
        CHECK_FALSE(b.Valid());
        CHECK(b.count == 0);
    }

    SECTION("an entity with no spatial component at all is not framable")
    {
        const Astra::Entity bare = reg->CreateEntity();
        const std::vector<Astra::Entity> sel{bare};
        CHECK_FALSE(Arcane::Editor::SelectionFramingBounds(*reg, sel).Valid());
    }

    SECTION("a transform-only entity is framable as a zero-size AABB")
    {
        const Astra::Entity node = reg->CreateEntity();
        reg->AddComponent<Arcane::WorldTransform>(
            node, Arcane::WorldTransform{WorldMat(glm::vec2(7.0f, -2.0f), glm::vec2(1.0f))});
        const std::vector<Astra::Entity> sel{node};

        const Arcane::Editor::FramingBounds b =
            Arcane::Editor::SelectionFramingBounds(*reg, sel);
        REQUIRE(b.Valid());          // framable...
        CHECK(b.count == 1);
        CHECK(b.min.x == Approx(7.0f));   // ...but with no extent
        CHECK(b.min.y == Approx(-2.0f));
        CHECK(b.max.x == Approx(7.0f));
        CHECK(b.max.y == Approx(-2.0f));
    }

    SECTION("a destroyed entity in the selection is skipped, not counted")
    {
        const Astra::Entity gone = MakeSprite(*reg, glm::vec2(0.0f), glm::vec2(1.0f));
        const Astra::Entity live = MakeSprite(*reg, glm::vec2(10.0f, 10.0f), glm::vec2(2.0f));
        reg->DestroyEntity(gone);
        const std::vector<Astra::Entity> sel{gone, live};

        const Arcane::Editor::FramingBounds b =
            Arcane::Editor::SelectionFramingBounds(*reg, sel);
        REQUIRE(b.Valid());
        CHECK(b.count == 1);
        CHECK(b.min.x == Approx(9.0f));
        CHECK(b.max.x == Approx(11.0f));
    }
}

TEST_CASE("Scene framing bounds sweep every visible sprite", "[editor]")
{
    auto reg = MakeSceneRegistry();

    SECTION("an empty scene is not framable")
    {
        CHECK_FALSE(Arcane::Editor::SceneFramingBounds(*reg).Valid());
    }

    SECTION("the union of all sprites, hidden ones excluded")
    {
        MakeSprite(*reg, glm::vec2(0.0f, 0.0f), glm::vec2(2.0f, 2.0f));
        MakeSprite(*reg, glm::vec2(6.0f, 0.0f), glm::vec2(2.0f, 2.0f));
        const Astra::Entity ghost = MakeSprite(*reg, glm::vec2(100.0f, 0.0f), glm::vec2(2.0f, 2.0f));
        // Hidden entities are not drawn, so framing "everything" must not
        // stretch the view out to reach them.
        reg->AddComponent<Arcane::Hidden>(ghost, Arcane::Hidden{});
        // A bare transform node (every scene has a SceneRoot) must not drag
        // the box back toward the origin either.
        const Astra::Entity root = reg->CreateEntity();
        reg->AddComponent<Arcane::WorldTransform>(
            root, Arcane::WorldTransform{WorldMat(glm::vec2(0.0f, 0.0f), glm::vec2(1.0f))});

        const Arcane::Editor::FramingBounds b = Arcane::Editor::SceneFramingBounds(*reg);
        REQUIRE(b.Valid());
        CHECK(b.count == 2);
        CHECK(b.min.x == Approx(-1.0f));
        CHECK(b.min.y == Approx(-1.0f));
        CHECK(b.max.x == Approx(7.0f));
        CHECK(b.max.y == Approx(1.0f));
    }
}

TEST_CASE("Framed bounds put the content inside the viewport", "[editor]")
{
    // The end-to-end contract the feature exists for: sweep -> frame -> every
    // framed corner is on screen at the camera the editor will push.
    auto reg = MakeSceneRegistry();
    MakeSprite(*reg, glm::vec2(-4.0f, 2.0f), glm::vec2(1.0f, 1.0f));
    MakeSprite(*reg, glm::vec2(9.0f, -6.0f), glm::vec2(3.0f, 2.0f));

    const Arcane::Editor::FramingBounds b = Arcane::Editor::SceneFramingBounds(*reg);
    REQUIRE(b.Valid());

    EditorCamera cam;
    const glm::vec2 viewport(1280.0f, 720.0f);
    cam.Frame(b.min, b.max, viewport);

    for (glm::vec2 corner : { b.min, b.max, glm::vec2(b.min.x, b.max.y), glm::vec2(b.max.x, b.min.y) })
    {
        const glm::vec2 s = cam.WorldToScreen(corner);
        CHECK(s.x >= 0.0f);
        CHECK(s.y >= 0.0f);
        CHECK(s.x <= viewport.x);
        CHECK(s.y <= viewport.y);
    }
}

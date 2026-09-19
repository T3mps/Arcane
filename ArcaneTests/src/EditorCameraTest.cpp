// Arcane Editor viewport camera (F4 plan 1 T6, spec s4): two persisted
// transforms (Ortho2D / Orbit3D) selected by ViewMode, Resolve() to the ONE
// ViewTransform the host pushes, the Unreal-shaped navigation ops, mode-aware
// framing -- plus the 3D framing-bounds sweep over the scene registry.
// CPU-only ([editor][camera]).
//
// Every pixel expectation goes through ViewTransform::WorldToScreen on the
// resolved view, because that is the map every consumer reads: if the camera
// and the renderer disagree, "frame selected" puts the thing off screen.

#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// mat4x4, NOT mat3x3. WorldMat below returns a mat4 (Task 3, F1) and this file
// carries no mat3 at all -- the implicit glm::mat4(glm::mat3) conversion once
// let a mat3 helper build into the widened WorldTransform and silently put the
// translation in the Z basis column. Keeping mat3x3 out disarms the trap.
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Viewport/EditorCamera.hpp>

using Catch::Approx;
using namespace Arcane::Editor;

static const glm::uvec2 kVp{800, 600};

// ---------------------------------------------------------------------------
// The camera itself
// ---------------------------------------------------------------------------

TEST_CASE("EditorCamera defaults: 2D mode, 5 m half-height at the origin, and Resolve is orthographic", "[editor][camera]")
{
    const EditorCamera cam;
    CHECK(cam.mode == ViewMode::TwoD);
    const auto v = cam.Resolve(kVp);
    CHECK(v.IsOrthographic());
    CHECK(v.WorldToScreen({0,0,0}).x == Approx(400.0f));
    CHECK(v.WorldToScreen({0,1,0}).y == Approx(300.0f - 60.0f));
}
TEST_CASE("Pan2D: the world follows the cursor 1:1; dragging DOWN moves the world down", "[editor][camera]")
{
    EditorCamera cam;
    const glm::vec3 before = cam.Resolve(kVp).WorldToScreen({1,1,0});
    cam.Pan2D({30.0f, -12.0f}, kVp);
    const glm::vec3 after = cam.Resolve(kVp).WorldToScreen({1,1,0});
    CHECK(after.x - before.x == Approx(30.0f)); CHECK(after.y - before.y == Approx(-12.0f));
}
TEST_CASE("ZoomAt2D keeps the world point under the cursor fixed and clamps", "[editor][camera]")
{
    EditorCamera cam;
    const glm::vec2 cursor{123.0f, 456.0f};
    const Arcane::Ray r = cam.Resolve(kVp).ScreenToRay(cursor);
    cam.ZoomAt2D(cursor, 3.0f, kVp);
    const glm::vec3 s = cam.Resolve(kVp).WorldToScreen(r.origin);
    CHECK(s.x == Approx(cursor.x).margin(1e-2f)); CHECK(s.y == Approx(cursor.y).margin(1e-2f));
    CHECK(cam.ortho.halfHeight == Approx(5.0f / std::pow(EditorCamera::kWheelStep, 3.0f)));
    cam.ZoomAt2D(cursor, -1000.0f, kVp); CHECK(cam.ortho.halfHeight == Approx(EditorCamera::kMaxHalfHeight));
    cam.ZoomAt2D(cursor,  1000.0f, kVp); CHECK(cam.ortho.halfHeight == Approx(EditorCamera::kMinHalfHeight));
}
TEST_CASE("Perspective Resolve: the eye orbits the pivot at yaw/pitch/distance and looks at it", "[editor][camera]")
{
    EditorCamera cam; cam.mode = ViewMode::Perspective;
    cam.orbit = { {1,2,3}, 0.0f, 0.0f, 10.0f, 60.0f };
    CHECK(cam.Eye() == glm::vec3(1, 2, 13));                       // yaw 0 / pitch 0 sits on +Z looking down -Z
    const auto v = cam.Resolve(kVp);
    CHECK_FALSE(v.IsOrthographic());
    const glm::vec3 c = v.WorldToScreen(cam.orbit.pivot);
    CHECK(c.x == Approx(400.0f)); CHECK(c.y == Approx(300.0f));
    CHECK(v.WorldToScreen({1, 3, 3}).y < 300.0f);                   // +Y up on screen
    cam.orbit.pitchDeg = 90.0f - 1e-3f;                              // clamp holds
    cam.Orbit({0.0f, -1000.0f}); CHECK(cam.orbit.pitchDeg <= 89.999f); CHECK(cam.orbit.pitchDeg >= -89.999f);
}
TEST_CASE("Orbit turns about the pivot; Look turns about the eye and moves the pivot", "[editor][camera]")
{
    EditorCamera cam; cam.mode = ViewMode::Perspective;
    const glm::vec3 pivot = cam.orbit.pivot; const glm::vec3 eye = cam.Eye();
    cam.Orbit({40.0f, 0.0f});
    CHECK(cam.orbit.pivot == pivot); CHECK(glm::length(cam.Eye() - pivot) == Approx(10.0f));
    EditorCamera cam2; cam2.mode = ViewMode::Perspective;
    cam2.Look({40.0f, 0.0f});
    CHECK(glm::length(cam2.Eye() - eye) < 1e-4f); CHECK(cam2.orbit.pivot != pivot);
}
TEST_CASE("Fly moves eye and pivot together along the camera axes, scaled by distance and the scalar", "[editor][camera]")
{
    EditorCamera cam; cam.mode = ViewMode::Perspective;   // distance 10 -> scale 1
    const glm::vec3 eye0 = cam.Eye(), piv0 = cam.orbit.pivot;
    cam.Fly({0,0,1}, 1.0f, false);                         // forward for one second
    CHECK(glm::length(cam.Eye() - eye0) == Approx(EditorCamera::kBaseFlySpeed));
    CHECK(glm::length(cam.orbit.pivot - piv0) == Approx(EditorCamera::kBaseFlySpeed));
    CHECK(glm::dot(cam.Eye() - eye0, cam.Forward()) > 0.0f);
    cam.speedScalar = 2.0f; cam.orbit.distance = 20.0f;
    const glm::vec3 eye1 = cam.Eye();
    cam.Fly({1,0,0}, 0.5f, true);                          // right, half a second, boosted x2
    CHECK(glm::length(cam.Eye() - eye1) == Approx(EditorCamera::kBaseFlySpeed * 2.0f * 2.0f * 2.0f * 0.5f));
}
TEST_CASE("Dolly scales the distance about the pivot; AdjustSpeed steps x1.1 within limits", "[editor][camera]")
{
    EditorCamera cam; cam.mode = ViewMode::Perspective;
    cam.Dolly(1.0f); CHECK(cam.orbit.distance == Approx(10.0f / EditorCamera::kWheelStep));
    cam.Dolly(-1000.0f); CHECK(cam.orbit.distance == Approx(EditorCamera::kMaxDistance));
    cam.AdjustSpeed(1.0f); CHECK(cam.speedScalar == Approx(1.1f));
    cam.AdjustSpeed(-100.0f); CHECK(cam.speedScalar >= 0.01f);
}
TEST_CASE("Frame: 2D fits the tighter axis with the margin; perspective solves distance = radius / (tan(fov/2) * min(aspect,1))", "[editor][camera]")
{
    FramingBounds b; b.min = {-2, -1, 0}; b.max = {2, 1, 0}; b.count = 1;
    EditorCamera cam;
    cam.Frame(b, kVp);
    CHECK(cam.ortho.center == glm::vec2(0, 0));
    // width 4 m across 800 px * 0.9 = 180 px/m -> halfHeight = 300/180
    CHECK(cam.ortho.halfHeight == Approx(300.0f / 180.0f));
    cam.mode = ViewMode::Perspective;
    cam.Frame(b, kVp);
    CHECK(cam.orbit.pivot == glm::vec3(0, 0, 0));
    const float radius = glm::length(glm::vec3(2, 1, 0));
    CHECK(cam.orbit.distance == Approx(radius / (std::tan(glm::radians(30.0f)) * 1.0f) / EditorCamera::kFrameFill));
}
TEST_CASE("Frame ignores an invalid bounds and a zero viewport; CentreOrigin resets the 2D centre only", "[editor][camera]")
{
    EditorCamera cam; const EditorCamera before = cam;
    cam.Frame(FramingBounds{}, kVp); CHECK(cam.ortho.halfHeight == before.ortho.halfHeight);
    FramingBounds b; b.min = b.max = {3,3,3}; b.count = 1;
    cam.Frame(b, {0, 0}); CHECK(cam.ortho.center == before.ortho.center);
    cam.ortho.center = {9, 9}; cam.CentreOrigin(); CHECK(cam.ortho.center == glm::vec2(0, 0));
}

// ---------------------------------------------------------------------------
// Framing bounds over the registry (3D AABB)
// ---------------------------------------------------------------------------

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

    // A non-rotating world matrix: basis columns carry the scale, m[3] the
    // translation (matches Transform::ToMatrix, which RenderSubmissionSystem
    // decomposes the same way).
    //
    // Task 3 (F1): this MUST return a mat4. glm's mat4(mat3) conversion is
    // implicit, so a mat3 helper still compiled into the widened
    // WorldTransform -- and silently landed the translation in the Z BASIS
    // COLUMN, framing every entity at the origin.
    glm::mat4 WorldMat(glm::vec3 pos, glm::vec3 scale)
    {
        glm::mat4 m(1.0f);
        m[0] = glm::vec4(scale.x, 0.0f, 0.0f, 0.0f);
        m[1] = glm::vec4(0.0f, scale.y, 0.0f, 0.0f);
        m[2] = glm::vec4(0.0f, 0.0f, scale.z, 0.0f);
        m[3] = glm::vec4(pos, 1.0f);
        return m;
    }

    // Sprite entity with a materialised WorldTransform (the editor refreshes
    // these before framing; the test sets them directly). With no .arcsprite
    // asset a sprite draws a 1x1 m quad at the centre pivot (SpriteEntry's
    // default (0.5, 0.5)), so its `size` is carried by the world matrix's
    // basis columns -- `scale` stays a separate argument to keep the "a scaled
    // sprite grows by its world scale" case reading the same way.
    Astra::Entity MakeSprite(Astra::Registry& reg, glm::vec2 pos, glm::vec2 size,
                             glm::vec2 scale = glm::vec2(1.0f))
    {
        Astra::Entity e = reg.CreateEntity();
        reg.AddComponent<Arcane::WorldTransform>(e,
            Arcane::WorldTransform{WorldMat(glm::vec3(pos, 0.0f), glm::vec3(size * scale, 1.0f))});
        Arcane::SpriteRenderer sr;
        reg.AddComponent<Arcane::SpriteRenderer>(e, sr);
        return e;
    }

    // Mesh entity: a MeshRenderer naming `mesh`, placed by WorldMat.
    Astra::Entity MakeMesh(Astra::Registry& reg, const Arcane::Guid& mesh, glm::vec3 pos, glm::vec3 scale)
    {
        Astra::Entity e = reg.CreateEntity();
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{WorldMat(pos, scale)});
        Arcane::MeshRenderer mr;
        mr.mesh = mesh;
        reg.AddComponent<Arcane::MeshRenderer>(e, mr);
        return e;
    }

    // A resolved mesh whose LOCAL bounds are the unit cube [-1, 1]^3 -- the
    // table entry framing reads; the geometry itself is irrelevant here.
    Arcane::MeshEntry UnitCubeEntry()
    {
        Arcane::MeshEntry entry;
        entry.bounds.min = glm::vec3(-1.0f);
        entry.bounds.max = glm::vec3( 1.0f);
        return entry;
    }
}

TEST_CASE("Framing bounds match how sprites are rendered", "[editor][camera]")
{
    auto reg = MakeSceneRegistry();
    // World size = the sprite asset's base size (1x1 m unresolved) * world
    // scale, about the pivot (the centre by default) -- the box of exactly
    // the SpriteWorldQuad corners RenderSubmissionSystem submits, widened by
    // kSpriteDepthEpsilon on every axis by BoundsSystem (F3: this box comes
    // from WorldBounds, the same one culling and picking read).
    const Astra::Entity a = MakeSprite(*reg, glm::vec2(3.0f, 4.0f), glm::vec2(2.0f, 1.0f));
    const std::vector<Astra::Entity> one{a};

    Arcane::BoundsSystem{}(*reg);   // F3: framing reads WorldBounds, which this pass writes from WorldTransform + the renderer
    const FramingBounds b = SelectionFramingBounds(*reg, one);
    REQUIRE(b.Valid());
    CHECK(b.count == 1);
    const float eps = Arcane::kSpriteDepthEpsilon;
    CHECK(b.min.x == Approx(2.0f - eps));
    CHECK(b.min.y == Approx(3.5f - eps));
    CHECK(b.min.z == Approx(0.0f - eps));
    CHECK(b.max.x == Approx(4.0f + eps));
    CHECK(b.max.y == Approx(4.5f + eps));
    CHECK(b.max.z == Approx(0.0f + eps));

    // A scaled sprite grows by its world scale, same as the drawn quad.
    const Astra::Entity s = MakeSprite(*reg, glm::vec2(-1.0f, 0.0f), glm::vec2(2.0f, 2.0f),
                                       glm::vec2(2.0f, 2.0f));
    Arcane::BoundsSystem{}(*reg);
    const std::vector<Astra::Entity> two{a, s};
    const FramingBounds u = SelectionFramingBounds(*reg, two);
    REQUIRE(u.Valid());
    CHECK(u.count == 2);
    CHECK(u.min.x == Approx(-3.0f - eps));
    CHECK(u.min.y == Approx(-2.0f - eps));
    CHECK(u.max.x == Approx(4.0f + eps));
    CHECK(u.max.y == Approx(4.5f + eps));
}

TEST_CASE("A sprite at (2,3,0) with the default pivot frames as its 1x1 quad", "[editor][camera]")
{
    auto reg = MakeSceneRegistry();
    const Astra::Entity a = MakeSprite(*reg, glm::vec2(2.0f, 3.0f), glm::vec2(1.0f, 1.0f));
    const std::vector<Astra::Entity> sel{a};
    Arcane::BoundsSystem{}(*reg);   // F3: framing reads WorldBounds, which this pass writes from WorldTransform + the renderer
    const FramingBounds b = SelectionFramingBounds(*reg, sel);
    REQUIRE(b.Valid());
    const glm::vec3 eps(Arcane::kSpriteDepthEpsilon);   // WorldBounds widens a flat sprite box on every axis
    CHECK(b.min == glm::vec3(1.5f, 2.5f, 0.0f) - eps);
    CHECK(b.max == glm::vec3(2.5f, 3.5f, 0.0f) + eps);
}

TEST_CASE("Framing bounds take a mesh's table AABB through its world matrix", "[editor][camera]")
{
    auto reg = MakeSceneRegistry();
    const Arcane::Guid meshId = Arcane::Guid::Generate();
    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    meshes.emplace(meshId, UnitCubeEntry());
    reg->SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });

    SECTION("a resolved mesh contributes its transformed local bounds, all three axes")
    {
        const Astra::Entity m = MakeMesh(*reg, meshId, glm::vec3(5.0f, 0.0f, 2.0f), glm::vec3(2.0f, 1.0f, 3.0f));
        const std::vector<Astra::Entity> sel{m};
        Arcane::BoundsSystem{}(*reg);   // F3: framing reads WorldBounds, which this pass writes from WorldTransform + the renderer
        const FramingBounds b = SelectionFramingBounds(*reg, sel);
        REQUIRE(b.Valid());
        CHECK(b.count == 1);
        CHECK(b.min.x == Approx(3.0f));
        CHECK(b.min.y == Approx(-1.0f));
        CHECK(b.min.z == Approx(-1.0f));
        CHECK(b.max.x == Approx(7.0f));
        CHECK(b.max.y == Approx(1.0f));
        CHECK(b.max.z == Approx(5.0f));

        // The scene sweep sees it too, and Hidden excludes it.
        CHECK(SceneFramingBounds(*reg).count == 1);
        reg->AddComponent<Arcane::Hidden>(m, Arcane::Hidden{});
        CHECK_FALSE(SceneFramingBounds(*reg).Valid());
    }

    SECTION("an UNRESOLVED mesh draws nothing: a point in a selection, absent from the scene sweep")
    {
        const Astra::Entity m = MakeMesh(*reg, Arcane::Guid::Generate(), glm::vec3(4.0f, -4.0f, 1.0f), glm::vec3(1.0f));
        const std::vector<Astra::Entity> sel{m};
        Arcane::BoundsSystem{}(*reg);   // F3: framing reads WorldBounds, which this pass writes from WorldTransform + the renderer
        const FramingBounds b = SelectionFramingBounds(*reg, sel);
        REQUIRE(b.Valid());
        CHECK(b.count == 1);
        CHECK(b.min == glm::vec3(4.0f, -4.0f, 1.0f));
        CHECK(b.max == glm::vec3(4.0f, -4.0f, 1.0f));
        CHECK_FALSE(SceneFramingBounds(*reg).Valid());
    }
}

TEST_CASE("Framing bounds distinguish nothing-to-frame from an empty AABB", "[editor][camera]")
{
    auto reg = MakeSceneRegistry();

    SECTION("an empty selection is not framable")
    {
        const FramingBounds b = SelectionFramingBounds(*reg, {});
        CHECK_FALSE(b.Valid());
        CHECK(b.count == 0);
    }

    SECTION("an entity with no spatial component at all is not framable")
    {
        const Astra::Entity bare = reg->CreateEntity();
        const std::vector<Astra::Entity> sel{bare};
        CHECK_FALSE(SelectionFramingBounds(*reg, sel).Valid());
    }

    SECTION("a transform-only entity is framable as a zero-size AABB at its 3D position")
    {
        const Astra::Entity node = reg->CreateEntity();
        reg->AddComponent<Arcane::WorldTransform>(
            node, Arcane::WorldTransform{WorldMat(glm::vec3(7.0f, -2.0f, 1.5f), glm::vec3(1.0f))});
        const std::vector<Astra::Entity> sel{node};

        Arcane::BoundsSystem{}(*reg);   // F3: framing reads WorldBounds, which this pass writes from WorldTransform + the renderer
        const FramingBounds b = SelectionFramingBounds(*reg, sel);
        REQUIRE(b.Valid());          // framable...
        CHECK(b.count == 1);
        CHECK(b.min.x == Approx(7.0f));   // ...but with no extent
        CHECK(b.min.y == Approx(-2.0f));
        CHECK(b.min.z == Approx(1.5f));
        CHECK(b.max.x == Approx(7.0f));
        CHECK(b.max.y == Approx(-2.0f));
        CHECK(b.max.z == Approx(1.5f));
    }

    SECTION("a destroyed entity in the selection is skipped, not counted")
    {
        const Astra::Entity gone = MakeSprite(*reg, glm::vec2(0.0f), glm::vec2(1.0f));
        const Astra::Entity live = MakeSprite(*reg, glm::vec2(10.0f, 10.0f), glm::vec2(2.0f));
        Arcane::BoundsSystem{}(*reg);   // F3: framing reads WorldBounds, which this pass writes from WorldTransform + the renderer
        reg->DestroyEntity(gone);
        const std::vector<Astra::Entity> sel{gone, live};

        const FramingBounds b = SelectionFramingBounds(*reg, sel);
        REQUIRE(b.Valid());
        CHECK(b.count == 1);
        const float eps = Arcane::kSpriteDepthEpsilon;
        CHECK(b.min.x == Approx(9.0f - eps));
        CHECK(b.max.x == Approx(11.0f + eps));
    }
}

TEST_CASE("Scene framing bounds sweep every visible sprite", "[editor][camera]")
{
    auto reg = MakeSceneRegistry();

    SECTION("an empty scene is not framable")
    {
        CHECK_FALSE(SceneFramingBounds(*reg).Valid());
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
            root, Arcane::WorldTransform{WorldMat(glm::vec3(0.0f), glm::vec3(1.0f))});

        Arcane::BoundsSystem{}(*reg);   // F3: framing reads WorldBounds, which this pass writes from WorldTransform + the renderer
        const FramingBounds b = SceneFramingBounds(*reg);
        REQUIRE(b.Valid());
        CHECK(b.count == 2);
        const float eps = Arcane::kSpriteDepthEpsilon;
        CHECK(b.min.x == Approx(-1.0f - eps));
        CHECK(b.min.y == Approx(-1.0f - eps));
        CHECK(b.max.x == Approx(7.0f + eps));
        CHECK(b.max.y == Approx(1.0f + eps));
    }
}

TEST_CASE("Framed bounds put the content inside the viewport", "[editor][camera]")
{
    // The end-to-end contract the feature exists for: sweep -> frame -> every
    // framed corner is on screen at the view the editor will push.
    auto reg = MakeSceneRegistry();
    MakeSprite(*reg, glm::vec2(-4.0f, 2.0f), glm::vec2(1.0f, 1.0f));
    MakeSprite(*reg, glm::vec2(9.0f, -6.0f), glm::vec2(3.0f, 2.0f));

    Arcane::BoundsSystem{}(*reg);   // F3: framing reads WorldBounds, which this pass writes from WorldTransform + the renderer
    const FramingBounds b = SceneFramingBounds(*reg);
    REQUIRE(b.Valid());

    const glm::uvec2 viewport{1280u, 720u};
    const glm::vec3 corners[] = {
        b.min, b.max, glm::vec3(b.min.x, b.max.y, 0.0f), glm::vec3(b.max.x, b.min.y, 0.0f)
    };

    SECTION("2D")
    {
        EditorCamera cam;
        cam.Frame(b, viewport);
        const Arcane::ViewTransform v = cam.Resolve(viewport);
        for (const glm::vec3& corner : corners)
        {
            const glm::vec3 s = v.WorldToScreen(corner);
            CHECK(s.x >= 0.0f);
            CHECK(s.y >= 0.0f);
            CHECK(s.x <= float(viewport.x));
            CHECK(s.y <= float(viewport.y));
        }
    }

    SECTION("perspective")
    {
        EditorCamera cam;
        cam.mode = ViewMode::Perspective;
        cam.Frame(b, viewport);
        const Arcane::ViewTransform v = cam.Resolve(viewport);
        for (const glm::vec3& corner : corners)
        {
            const glm::vec3 s = v.WorldToScreen(corner);
            CHECK(std::isfinite(s.x));
            CHECK(s.x >= 0.0f);
            CHECK(s.y >= 0.0f);
            CHECK(s.x <= float(viewport.x));
            CHECK(s.y <= float(viewport.y));
            CHECK(s.z > 0.0f);   // in front of the near plane
        }
    }
}

TEST_CASE("framing reads WorldBounds: a box the renderer would draw is the box that frames", "[editor][camera][framing]")
{
    // A cube [-1,1]^3 at (10,0,0) -> WorldBounds [9,11]x[-1,1]x[-1,1]; a bare node at (-3,0,0) contributes its position.
    auto reg = MakeSceneRegistry();
    const Arcane::Guid cubeId = Arcane::Guid::Generate();
    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    Arcane::MeshEntry entry;
    entry.data   = Arcane::BuildCube(2.0f);          // local box [-1, 1]^3
    entry.bounds = Arcane::ComputeMeshBounds(entry.data);
    meshes.emplace(cubeId, entry);
    reg->SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });

    const Astra::Entity a = MakeMesh(*reg, cubeId, glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(1.0f));
    const Astra::Entity b = reg->CreateEntity();
    reg->AddComponent<Arcane::WorldTransform>(
        b, Arcane::WorldTransform{WorldMat(glm::vec3(-3.0f, 0.0f, 0.0f), glm::vec3(1.0f))});

    Arcane::BoundsSystem{}(*reg);   // F3: framing reads WorldBounds, which this pass writes from WorldTransform + the renderer
    const Astra::Entity sel[] = { a, b };
    const FramingBounds fb = SelectionFramingBounds(*reg, sel);
    REQUIRE(fb.count == 2);
    CHECK(fb.min == glm::vec3(-3, -1, -1));
    CHECK(fb.max == glm::vec3(11, 1, 1));
    const FramingBounds scene = SceneFramingBounds(*reg);
    REQUIRE(scene.count == 1);   // the bare node is not drawn, so Frame All ignores it
    CHECK(scene.min == glm::vec3(9, -1, -1));
}

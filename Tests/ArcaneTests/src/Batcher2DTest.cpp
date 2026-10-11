// Batcher2D -- the device-less batching contract for the WORLD-space path
// (F4 plan 1, Task 4). The screen-space contract (spans split on material +
// texture Guid, Stats parity, the device-less End()) lives in SeveranceTest.cpp;
// this file pins what Task 4 added beside it: vec3 vertices, the per-span
// `worldSpace` selector, and the sticky view-projection that leaves through
// Batch2DDrained.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Guid.hpp>
#include <Arcane/Render/Batcher2D.hpp>

#include <glm/glm.hpp>

#include <array>

TEST_CASE("QuadWorld records a world-space span; Rect records a screen-space one; both sort by layer", "[batcher]")
{
    auto b = Arcane::Batcher2D::Create();
    b->Begin(800, 600);
    b->SetViewProjection(glm::mat4(2.0f));
    b->SetLayer(1, 0);
    b->Rect({10, 10}, {20, 20}, {1,1,1,1});
    b->SetLayer(0, 0);
    const std::array<glm::vec3, 4> corners{ glm::vec3{-1, 1, 0}, {1, 1, 0}, {1, -1, 0}, {-1, -1, 0} };
    b->QuadWorld(Arcane::Batcher2D::kMaterialSprite, Arcane::Guid::Nil(), corners, {0,0}, {1,1}, {1,1,1,1});
    const Arcane::Batch2DDrained d = b->Drain();
    REQUIRE(d.spans.size() == 2);
    CHECK(d.spans[0].worldSpace);          // layer 0 first
    CHECK_FALSE(d.spans[1].worldSpace);
    // The vertex stream stays in PUSH order (Rect first: vertices 0..3, the
    // world quad 4..7); only the INDEX stream is sorted -- so a span's first
    // index is the way to its first vertex (SeveranceTest's idiom).
    CHECK(d.vertices[d.indices[d.spans[0].firstIndex]].pos == glm::vec3(-1, 1, 0));   // the world quad's TL, verbatim
    CHECK(d.vertices[d.indices[d.spans[1].firstIndex]].pos == glm::vec3(10, 10, 0));  // the screen rect's TL...
    CHECK(d.vertices[d.indices[d.spans[1].firstIndex]].pos.z == 0.0f);                // ...carries z = 0
    CHECK(d.viewProjection == glm::mat4(2.0f));
    CHECK(sizeof(Arcane::Batch2DVertex) == 36);
}

TEST_CASE("QuadWorld's vertices are the caller's corners verbatim, indexed through the span",
          "[batcher]")
{
    // The vertex stream is in PUSH order (a record's firstVertex never moves);
    // only the records -- and so the index stream and the spans -- are sorted.
    // Read the world quad through its span's indices so the assertion holds
    // whichever way the sort lands.
    auto b = Arcane::Batcher2D::Create();
    b->Begin(320, 240);
    b->Rect({0, 0}, {8, 8}, {1, 1, 1, 1});                    // pushed first, screen
    const std::array<glm::vec3, 4> corners{ glm::vec3{-2.0f, 3.0f, 0.5f}, {2.0f, 3.0f, 0.5f},
                                            {2.0f, -3.0f, 0.5f}, {-2.0f, -3.0f, 0.5f} };
    b->QuadWorld(Arcane::Batcher2D::kMaterialSprite, Arcane::Guid::Nil(), corners,
                 {0.25f, 0.5f}, {0.75f, 1.0f}, {1, 0, 0, 1});
    const Arcane::Batch2DDrained d = b->Drain();

    // Same (layer, order, material, nil texture): the two quads coalesce into
    // ONE span only if worldSpace does NOT split them -- it must, so two spans.
    REQUIRE(d.spans.size() == 2);
    CHECK_FALSE(d.spans[0].worldSpace);
    CHECK(d.spans[1].worldSpace);
    CHECK(d.spans[0].indexCount == 6);
    CHECK(d.spans[1].indexCount == 6);

    const Arcane::Batch2DDrawSpan& world = d.spans[1];
    REQUIRE(world.firstIndex + 6 <= d.indices.size());
    const Arcane::Batch2DVertex& tl = d.vertices[d.indices[world.firstIndex + 0]];
    const Arcane::Batch2DVertex& tr = d.vertices[d.indices[world.firstIndex + 1]];
    const Arcane::Batch2DVertex& br = d.vertices[d.indices[world.firstIndex + 2]];
    // Second triangle is (0, 2, 3): its last index is BL.
    const Arcane::Batch2DVertex& bl = d.vertices[d.indices[world.firstIndex + 5]];
    CHECK(tl.pos == corners[0]);
    CHECK(tr.pos == corners[1]);
    CHECK(br.pos == corners[2]);
    CHECK(bl.pos == corners[3]);
    // UVs follow PushQuad's corner order: TL=(uMin,vMin), TR=(uMax,vMin),
    // BR=(uMax,vMax), BL=(uMin,vMax).
    CHECK(tl.uv == glm::vec2(0.25f, 0.5f));
    CHECK(tr.uv == glm::vec2(0.75f, 0.5f));
    CHECK(br.uv == glm::vec2(0.75f, 1.0f));
    CHECK(bl.uv == glm::vec2(0.25f, 1.0f));
    CHECK(tl.color == glm::vec4(1, 0, 0, 1));

    // The screen rect's four vertices are z = 0 -- the old vec2 path widened,
    // not moved.
    const Arcane::Batch2DDrawSpan& screen = d.spans[0];
    for (std::uint32_t i = 0; i < 6; ++i)
        CHECK(d.vertices[d.indices[screen.firstIndex + i]].pos.z == 0.0f);
}

TEST_CASE("CircleWorld records a world-space circle quad in the (right, up) plane with the SDF uv",
          "[batcher]")
{
    auto b = Arcane::Batcher2D::Create();
    b->Begin(100, 100);
    // A plane tilted out of XY: right = +X, up = +Z. The four corners must be
    // center +- r*right +- r*up, and the uv the same [-1,1] square Circle()
    // records (circle.hlsl's length(uv) is sign-agnostic).
    b->CircleWorld(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), 0.5f,
                   glm::vec4(0, 1, 0, 1));
    const Arcane::Batch2DDrained d = b->Drain();
    REQUIRE(d.spans.size() == 1);
    CHECK(d.spans[0].worldSpace);
    CHECK(d.spans[0].material == Arcane::Batcher2D::kMaterialCircle);
    CHECK(d.spans[0].textureId.IsNil());
    REQUIRE(d.vertices.size() == 4);
    CHECK(d.vertices[0].pos == glm::vec3(0.5f, 2.0f, 3.5f));   // center - r + u  (TL)
    CHECK(d.vertices[1].pos == glm::vec3(1.5f, 2.0f, 3.5f));   // center + r + u  (TR)
    CHECK(d.vertices[2].pos == glm::vec3(1.5f, 2.0f, 2.5f));   // center + r - u  (BR)
    CHECK(d.vertices[3].pos == glm::vec3(0.5f, 2.0f, 2.5f));   // center - r - u  (BL)
    CHECK(d.vertices[0].uv == glm::vec2(-1.0f, -1.0f));
    CHECK(d.vertices[1].uv == glm::vec2( 1.0f, -1.0f));
    CHECK(d.vertices[2].uv == glm::vec2( 1.0f,  1.0f));
    CHECK(d.vertices[3].uv == glm::vec2(-1.0f,  1.0f));

    // A non-positive radius records nothing, like Circle().
    b->Begin(100, 100);
    b->CircleWorld(glm::vec3(0.0f), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), 0.0f, glm::vec4(1.0f));
    CHECK(b->Drain().Empty());
}

TEST_CASE("SetViewProjection is sticky within a Begin() bracket and resets to identity at Begin()",
          "[batcher]")
{
    auto b = Arcane::Batcher2D::Create();
    b->Begin(64, 64);
    CHECK(b->Drain().viewProjection == glm::mat4(1.0f));   // the default: identity

    b->Begin(64, 64);
    b->SetViewProjection(glm::mat4(3.0f));
    b->Rect({0, 0}, {1, 1}, {1, 1, 1, 1});
    CHECK(b->Drain().viewProjection == glm::mat4(3.0f));

    // A new bracket forgets it: a host that sets no view this frame gets
    // identity, never last frame's matrix.
    b->Begin(64, 64);
    CHECK(b->Drain().viewProjection == glm::mat4(1.0f));
}

TEST_CASE("every screen-space primitive records worldSpace = false and z = 0", "[batcher]")
{
    // The byte-stability contract for the pre-Task-4 API: nothing in the
    // existing submission surface can land on the view-projection path.
    auto b = Arcane::Batcher2D::Create();
    b->Begin(64, 64);
    b->SetViewProjection(glm::mat4(5.0f));   // set, and must be IGNORED by every span below
    b->Quad({0, 0}, {4, 4}, {0, 0}, {1, 1}, {1, 1, 1, 1});
    b->QuadTextured(Arcane::Batcher2D::kMaterialSprite, Arcane::Guid::Generate(),
                    {0, 0}, {4, 4}, {0, 0}, {1, 1}, {1, 1, 1, 1});
    b->Rect({0, 0}, {4, 4}, {1, 1, 1, 1}, 0.3f);
    b->Line({0, 0}, {8, 8}, 1.0f, {1, 1, 1, 1});
    b->Circle({8, 8}, 2.0f, {1, 1, 1, 1});
    b->Triangle({0, 0}, {4, 0}, {0, 4}, {1, 1, 1, 1});
    b->Glyph({0, 0}, {4, 4}, {0, 0}, {1, 1}, {1, 1, 1, 1});
    const Arcane::Batch2DDrained d = b->Drain();
    REQUIRE_FALSE(d.spans.empty());
    for (const Arcane::Batch2DDrawSpan& span : d.spans)
        CHECK_FALSE(span.worldSpace);
    REQUIRE(d.vertices.size() == 7 * 4);
    for (const Arcane::Batch2DVertex& v : d.vertices)
        CHECK(v.pos.z == 0.0f);
}

// ExtractFrameTest.cpp -- the one extract stage (F5 plan 1 Task 5, spec s4.1):
// ExtractFrame runs the sprite submission into the host's batcher and COPIES
// the drained batch into the packet, so a later registry edit cannot reach
// the packet. Device-free, under ~[gpu]; the GPU witness that a registered
// sprite material id survives the copy is in NriGraphPixelTest.cpp.
#include <Arcane/Base/ExtractAccounting.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/ExtractFrame.hpp>
#include <Arcane/Render/GpuSceneSync.hpp>         // the World fixture's GpuSceneSync
#include <Arcane/Render/GpuSceneTypes.hpp>        // the World fixture's GpuSceneMirror / GpuSceneStage
#include <Arcane/Render/RenderFramePacket.hpp>
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderViewSettings.hpp>   // Ortho2DView
#include <Arcane/Scene/SceneCamera.hpp>          // ActiveSceneCamera, ActivePerspectiveSceneCamera
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include "Helpers/ConstantScan.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // GpuSceneSyncTest.cpp's World (that copy is not visible to this TU), plus
    // AddSprite.
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        std::unordered_map<Arcane::Guid, Arcane::MeshEntry>            meshes;
        std::unordered_map<Arcane::Guid, Arcane::ResolvedMeshMaterial> materials;
        std::uint64_t meshGen = 1;   // the fixture plays MeshCache: MeshTable::generation, bumped on a re-publish
        Astra::Entity root{};
        Arcane::GpuSceneMirror mirror;
        Arcane::GpuSceneStage  stage;
        std::uint64_t          deviceGen = 0;   // what the device side last synced; 0 = never

        World()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
            reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes, &meshGen });
            reg.SetResource<Arcane::MeshMaterialTable>(Arcane::MeshMaterialTable{ &materials });
        }
        Arcane::Guid Mesh(std::uint32_t sections = 1, Arcane::Guid slotMaterial = {})
        {
            const Arcane::Guid id = Arcane::Guid::Generate();
            Arcane::MeshEntry entry;
            entry.data   = Arcane::BuildCube(2.0f);
            entry.bounds = Arcane::ComputeMeshBounds(entry.data);
            const std::uint32_t per = static_cast<std::uint32_t>(entry.data.indices.size()) / sections;
            entry.data.sections.clear();
            for (std::uint32_t s = 0; s < sections; ++s)
            {
                entry.data.sections.push_back(Arcane::MeshSection{ std::string(), s * per, per, s });
                entry.slots.push_back(Arcane::MeshSlot{ std::string(), slotMaterial });
            }
            meshes.emplace(id, entry);
            return id;
        }
        Astra::Entity Spawn(glm::vec3 pos, Arcane::Guid mesh, Arcane::Guid overrideMat = {})
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t; t.position = pos;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.SetParent(e, root);
            reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ mesh, overrideMat });
            return e;
        }
        // One host frame: propagate, bounds, sync (the device side "applies" by stamping the generation).
        void Frame()
        {
            Arcane::TransformPropagationSystem{}(reg);
            Arcane::BoundsSystem{}(reg);
            Arcane::GpuSceneSync(reg, mirror, deviceGen, stage);
            deviceGen = mirror.generation;
        }
        const Arcane::GpuInstance* Staged(std::uint32_t row) const
        {
            for (std::size_t i = 0; i < stage.rows.size(); ++i)
                if (stage.rows[i] == row) return &stage.values[i];
            return nullptr;
        }
        std::vector<const Arcane::GpuInstance*> StagedAll(std::uint32_t row) const
        {
            std::vector<const Arcane::GpuInstance*> out;
            for (std::size_t i = 0; i < stage.rows.size(); ++i)
                if (stage.rows[i] == row) out.push_back(&stage.values[i]);
            return out;
        }
        std::uint32_t RowOf(Astra::Entity e, std::uint32_t section = 0) const
        {
            const Arcane::GpuSceneMirror::Rows* r = mirror.slots.TryGet(e);
            REQUIRE(r);
            return r->first + section;
        }

        Astra::Entity sprite{};
        Astra::Entity AddSprite(glm::vec3 pos)
        {
            sprite = reg.CreateEntity();
            Arcane::Transform t;
            t.position = pos;
            reg.AddComponent<Arcane::Transform>(sprite, t);
            reg.SetParent(sprite, root);
            reg.AddComponent<Arcane::SpriteRenderer>(sprite, Arcane::SpriteRenderer{});
            return sprite;
        }
        void AddOrthoCamera()
        {
            Arcane::Camera cam;
            cam.active = true;
            cam.projection = Arcane::CameraProjection::Orthographic;
            cam.orthographicSize = 2.0f;
            reg.AddComponent<Arcane::Camera>(root, cam);
        }
        void AddPerspectiveCamera()
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t;
            t.position = { 0.0f, 0.0f, 5.0f };
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.SetParent(e, root);
            Arcane::Camera cam;
            cam.active = true;
            cam.projection = Arcane::CameraProjection::Perspective;
            cam.fovYDegrees = 50.0f;
            cam.nearZ = 0.05f;
            cam.farZ = 500.0f;
            reg.AddComponent<Arcane::Camera>(e, cam);
        }
    };
}

TEST_CASE("extract copies one sprite quad and ignores a later edit", "[nri][extract]")
{
    World world;
    Astra::Entity sprite = world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);
    // Task 6's BuildVisibleSet only inserts entities that already have
    // WorldBounds (VisibilitySystem.hpp:124-128). RenderSubmissionSystem
    // drops anything that set does not contain (RenderSystems.hpp:62-63).
    Arcane::BoundsSystem{}(world.reg);

    auto batcher = Arcane::Batcher2D::Create();
    batcher->Begin(128u, 128u);
    Arcane::RenderContext2D ctx;
    ctx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(std::move(ctx));

    Arcane::ResetExtractFrameCallCount();
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    // Ortho view so Task 6's visible set still contains this quad.
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    CHECK(packet.origin.translation == glm::vec3(0.0f));
    CHECK(packet.viewport == req.viewport);
    CHECK(packet.batchVertices.size() == 4u);
    CHECK(Arcane::ExtractFrameCallCount() == 1u);
    const glm::vec3 copied = packet.batchVertices[0].pos;

    world.reg.GetComponent<Arcane::Transform>(sprite)->position = { 50.0f, 50.0f, 0.0f };
    CHECK(packet.batchVertices[0].pos == copied);
    CHECK(Arcane::ExtractFrameCallCount() == 1u);
}

TEST_CASE("extract keeps a registered sprite material id", "[nri][extract]")
{
    World world;
    auto batcher = Arcane::Batcher2D::Create();
    REQUIRE(batcher != nullptr);

    auto templ = std::make_shared<Arcane::MaterialTemplate>(Arcane::MaterialTemplate::Build(
        "extract-id", 1, {}));
    auto instance = std::make_shared<Arcane::MaterialInstance>(
        std::shared_ptr<const Arcane::MaterialTemplate>(templ));
    Arcane::Material2DDesc desc;
    desc.templ = templ;
    desc.instance = instance;
    desc.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 1, 2, 3, 4 });
    desc.psBytes = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 5, 6, 7, 8 });
    const std::uint16_t id = batcher->RegisterMaterial(desc);
    REQUIRE(id != Arcane::Batcher2D::kInvalidMaterialId);
    REQUIRE(id >= 3u);
    REQUIRE(id != Arcane::Batcher2D::kMaterialSprite);
    REQUIRE(batcher->MaterialDesc(id) != nullptr);

    batcher->Begin(128u, 128u);
    Arcane::RenderContext2D ctx;
    ctx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(std::move(ctx));

    const Arcane::Guid matGuid = Arcane::Guid::Generate();
    std::unordered_map<Arcane::Guid, std::uint16_t> ids;
    ids.emplace(matGuid, id);
    world.reg.SetResource<Arcane::SpriteMaterialTable>(Arcane::SpriteMaterialTable{ &ids });

    Astra::Entity sprite = world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::SpriteRenderer& rend = *world.reg.GetComponent<Arcane::SpriteRenderer>(sprite);
    rend.material = matGuid;
    rend.tint = { 1.0f, 0.0f, 0.0f, 1.0f };
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::BoundsSystem{}(world.reg);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.batchSpans.size() == 1u);
    CHECK(packet.batchSpans[0].material == id);
    CHECK(packet.batchSpans[0].material != Arcane::Batcher2D::kMaterialSprite);
    CHECK(packet.batchSpans[0].worldSpace);
    CHECK(packet.batchVertices.size() == 4u);
}

TEST_CASE("extract publishes one mesh row and ignores a later transform edit", "[nri][extract][gpuscene]")
{
    World world;
    const Arcane::Guid mesh = world.Mesh(1);
    Astra::Entity spawned = world.Spawn({ 0.0f, 0.0f, 0.0f }, mesh);
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::BoundsSystem{}(world.reg);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.mesh.stats.total >= 1u);
    const std::uint32_t total = packet.mesh.stats.total;
    world.reg.GetComponent<Arcane::Transform>(spawned)->position = { 40.0f, 0.0f, 0.0f };
    CHECK(packet.mesh.stats.total == total);
    CHECK(packet.visible.entries.size() >= 1u);
}

TEST_CASE("meshes cull against the perspective mesh view, not the orthographic sprite view", "[nri][extract][gpuscene]")
{
    auto coarse = [](glm::vec3 position)
    {
        World world;
        world.AddOrthoCamera();
        world.AddPerspectiveCamera();
        world.Spawn(position, world.Mesh(1));
        Arcane::TransformPropagationSystem{}(world.reg);
        Arcane::BoundsSystem{}(world.reg);
        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { 200u, 100u };
        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
        REQUIRE(packet.mesh.stats.total == 1u);
        return packet.mesh.stats.coarseVisible;
    };
    // x = 30 at depth 55: inside the perspective frustum (half-width about 51 m),
    // outside the orthographic view (half-width 4 m).
    CHECK(coarse({ 30.0f, 0.0f, -50.0f }) == 1u);
    // z = -600: inside the orthographic slab (+-render.ortho2D.depthRange, 1000 m),
    // 605 m from the perspective eye, past its far plane.
    CHECK(coarse({ 0.0f, 0.0f, -600.0f }) == 0u);
}

TEST_CASE("the scene-save cover invalidates the GPU-scene mirror after its extract", "[nri][extract][gpuscene]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorApp.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::size_t fn = text.find("bool EditorApp::CaptureGraphViewportPng(");
    REQUIRE(fn != std::string::npos);
    const std::size_t end = text.find("\n    }\n", fn);   // four-space-indented member definitions
    REQUIRE(end != std::string::npos);
    const std::string body = text.substr(fn, end - fn);
    const std::size_t extract = body.find("ExtractFrame(");
    const std::size_t render = body.find("RenderFrameOffscreen(");
    const std::size_t invalidate = body.find("GpuSceneInvalidate(m_runtime->Registry())");
    REQUIRE(extract != std::string::npos);
    REQUIRE(render != std::string::npos);
    REQUIRE(invalidate != std::string::npos);
    CHECK(extract < render);
    CHECK(render < invalidate);
    // Before the early return on a non-Presented outcome, so every outcome invalidates.
    const std::size_t early = body.find("return false;", render);
    CHECK((early == std::string::npos || invalidate < early));
}

static void CheckView(const Arcane::ViewTransform& a, const Arcane::ViewTransform& b)
{
    CHECK(a.view == b.view);
    CHECK(a.projection == b.projection);
    CHECK(a.viewport == b.viewport);
}

TEST_CASE("runtime extract keeps the ortho sprite view and the perspective mesh view", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddPerspectiveCamera();
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 200u, 100u };
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    const std::optional<Arcane::SceneCameraView> ortho = Arcane::ActiveSceneCamera(world.reg, req.viewport);
    const std::optional<Arcane::ViewTransform> persp = Arcane::ActivePerspectiveSceneCamera(world.reg, req.viewport);
    REQUIRE(packet.spriteView.has_value());
    REQUIRE(packet.meshView.has_value());
    REQUIRE(ortho.has_value());
    REQUIRE(persp.has_value());
    CheckView(packet.spriteView->view, ortho->view);
    CHECK(packet.spriteView->worldCenter == ortho->worldCenter);
    CHECK(packet.spriteView->halfHeight == ortho->halfHeight);
    CheckView(*packet.meshView, *persp);
    CHECK(packet.spriteViewFromScene);
}

TEST_CASE("with no scene camera the sprite view is the host's stored view, not identity", "[nri][extract]")
{
    World world;   // no Camera component
    Arcane::RenderContext2D ctx;
    ctx.view = Arcane::Ortho2DView({ 3.0f, 1.0f }, 2.0f, { 64u, 64u });
    // SetResource takes T&&. With the template argument explicit, an lvalue
    // does not bind; the move leaves this trivial view intact for the check.
    world.reg.SetResource<Arcane::RenderContext2D>(std::move(ctx));
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 64u, 64u };
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    CheckView(packet.spriteView->view, ctx.view);
    CHECK_FALSE(packet.spriteViewFromScene);
}

TEST_CASE("an editorView is the world view and is not replaced by a scene camera", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddPerspectiveCamera();
    world.reg.GetComponent<Arcane::Transform>(world.root)->position = { 4.0f, 0.0f, 0.0f };
    const std::optional<Arcane::SceneCameraView> scene = Arcane::ActiveSceneCamera(world.reg, { 64u, 64u });
    REQUIRE(scene.has_value());
    Arcane::ViewTransform editor = scene->view;
    editor.view[3][0] += 3.0f;
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 64u, 64u };
    req.editorView = editor;
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    REQUIRE(packet.meshView.has_value());
    CHECK(packet.spriteView->view.view == editor.view);
    CHECK(packet.spriteView->view.projection == editor.projection);
    CHECK(packet.meshView->view == editor.view);
    CHECK(packet.meshView->projection == editor.projection);
    CHECK(packet.spriteView->worldCenter == glm::vec2(0.0f));
    CHECK(scene->worldCenter != packet.spriteView->worldCenter);
}

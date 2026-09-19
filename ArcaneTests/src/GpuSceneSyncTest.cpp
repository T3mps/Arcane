// GpuSceneSyncTest.cpp -- the GPU scene's CPU half (F3 plan 1 T5, spec s5):
// GpuSceneSync (reconcile + exact dirty + the lastModel prior-pose history
// with the re-dirty + the material chain) and BuildGpuSceneFrame (batches
// nearest-first, CPU-written visible indices + indirect args). Device-free,
// under ~[gpu]. The [gpuscene][material] cases absorb the retired
// CollectMeshInstances pins that used to live in MeshSubmissionTest.cpp.
#include <Arcane/Host/GpuSceneHost.hpp>   // PrepareSceneForRender (F3 plan 1 T8): the host-side [gpuscene][host] case
#include <Arcane/Render/GpuSceneSync.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <unordered_map>

namespace
{
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        std::unordered_map<Arcane::Guid, Arcane::MeshEntry>            meshes;
        std::unordered_map<Arcane::Guid, Arcane::ResolvedMeshMaterial> materials;
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
            reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });
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
        std::uint32_t RowOf(Astra::Entity e, std::uint32_t section = 0) const
        {
            const Arcane::GpuSceneMirror::Rows* r = mirror.slots.TryGet(e);
            REQUIRE(r);
            return r->first + section;
        }
    };
}

TEST_CASE("GpuSceneSync: a new entity allocates one row per section, staged with prev == model", "[gpuscene]")
{
    World w;
    const Arcane::Guid mesh = w.Mesh(3);
    Astra::Entity e = w.Spawn(glm::vec3(1, 2, 3), mesh);
    w.Frame();
    REQUIRE(w.stage.rows.size() == 3);
    CHECK(w.stage.fullRebuild);   // first sync against a device that never synced this mirror
    for (std::uint32_t s = 0; s < 3; ++s)
    {
        const Arcane::GpuInstance* v = w.Staged(w.RowOf(e, s));
        REQUIRE(v);
        CHECK(v->model[3] == glm::vec4(1, 2, 3, 1));
        CHECK(v->prevModel == v->model);
        CHECK(v->boundsMin == glm::vec4(0, 1, 2, 0));
        CHECK(v->boundsMax == glm::vec4(2, 3, 4, 0));
        CHECK(v->materialSlot == Arcane::kGpuInvalidMaterialSlot);
        CHECK(v->baseColor == glm::vec4(1.0f));
    }
    // Three distinct batch keys (same mesh, sections 0/1/2), each with one row.
    CHECK(w.mirror.batchKeys.size() == 3);
    CHECK(w.mirror.batchRowCount == std::vector<std::uint32_t>{ 1, 1, 1 });
}

TEST_CASE("GpuSceneSync: a static scene stages nothing on the second frame", "[gpuscene]")
{
    World w;
    w.Spawn(glm::vec3(0), w.Mesh());
    w.Frame();
    w.Frame();
    CHECK(w.stage.rows.empty());
    CHECK_FALSE(w.stage.fullRebuild);
}

TEST_CASE("GpuSceneSync: a move stages prev = the old pose, and the frame after re-stages prev == model (the re-dirty)", "[gpuscene]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0), w.Mesh());
    w.Frame();
    w.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(5, 0, 0);
    w.Frame();                                             // frame N: moved
    const std::uint32_t row = w.RowOf(e);
    REQUIRE(w.stage.rows.size() == 1);
    CHECK(w.Staged(row)->model[3] == glm::vec4(5, 0, 0, 1));
    CHECK(w.Staged(row)->prevModel[3] == glm::vec4(0, 0, 0, 1));
    w.Frame();                                             // frame N+1: nothing changed, the re-dirty settles it
    REQUIRE(w.stage.rows.size() == 1);
    CHECK(w.Staged(row)->prevModel == w.Staged(row)->model);
    w.Frame();                                             // frame N+2: at rest
    CHECK(w.stage.rows.empty());
}

TEST_CASE("GpuSceneSync: moved on N and N+1 carries N's pose as prev on N+1", "[gpuscene]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0), w.Mesh());
    w.Frame();
    w.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(1, 0, 0);
    w.Frame();
    w.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(2, 0, 0);
    w.Frame();
    const std::uint32_t row = w.RowOf(e);
    REQUIRE(w.stage.rows.size() == 1);
    CHECK(w.Staged(row)->model[3] == glm::vec4(2, 0, 0, 1));
    CHECK(w.Staged(row)->prevModel[3] == glm::vec4(1, 0, 0, 1));
}

TEST_CASE("GpuSceneSync: a destroyed entity frees its rows and a later spawn reuses the span", "[gpuscene]")
{
    World w;
    const Arcane::Guid mesh = w.Mesh(2);
    Astra::Entity a = w.Spawn(glm::vec3(0), mesh);
    w.Frame();
    const std::uint32_t firstA = w.RowOf(a);
    w.reg.DestroyEntity(a);
    w.Frame();
    CHECK(w.mirror.slots.TryGet(a) == nullptr);
    CHECK(w.mirror.batchRowCount == std::vector<std::uint32_t>{ 0, 0 });
    Astra::Entity b = w.Spawn(glm::vec3(0), mesh);
    w.Frame();
    CHECK(w.RowOf(b) == firstA);
    CHECK(w.mirror.allocator.HighWater() == 2);
}

TEST_CASE("GpuSceneSync: Hidden frees the rows; unhiding re-allocates them as new (prev == model)", "[gpuscene]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0), w.Mesh());
    w.Frame();
    w.reg.AddComponent<Arcane::Hidden>(e, Arcane::Hidden{});
    w.Frame();
    CHECK(w.mirror.slots.TryGet(e) == nullptr);
    w.reg.RemoveComponent<Arcane::Hidden>(e);
    w.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(7, 0, 0);
    w.Frame();
    const Arcane::GpuInstance* v = w.Staged(w.RowOf(e));
    REQUIRE(v);
    CHECK(v->prevModel == v->model);
}

TEST_CASE("GpuSceneSync: reassigning the mesh to one with a different section count reallocates", "[gpuscene]")
{
    World w;
    const Arcane::Guid one = w.Mesh(1);
    const Arcane::Guid three = w.Mesh(3);
    Astra::Entity e = w.Spawn(glm::vec3(0), one);
    w.Frame();
    w.reg.GetComponent<Arcane::MeshRenderer>(e)->mesh = three;
    w.Frame();
    CHECK(w.mirror.slots.TryGet(e)->count == 3);
    CHECK(w.stage.rows.size() == 3);
}

TEST_CASE("GpuSceneSync: a generation mismatch (registry swap) re-stages every live row with prev == model", "[gpuscene]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0), w.Mesh());
    w.Frame();
    w.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(1, 0, 0);
    w.Frame();
    w.deviceGen = 0;   // the device forgot this mirror (a new context, or a mirror from a swapped registry)
    w.Frame();
    REQUIRE(w.stage.rows.size() == 1);
    CHECK(w.stage.fullRebuild);
    CHECK(w.Staged(w.RowOf(e))->prevModel == w.Staged(w.RowOf(e))->model);
}

TEST_CASE("GpuSceneSync: the material chain -- override wins, else the slot's material, else white; an unresolvable override falls to the slot", "[gpuscene][material]")
{
    World w;
    const Arcane::Guid slotMat = Arcane::Guid::Generate();
    const Arcane::Guid overMat = Arcane::Guid::Generate();
    w.materials.emplace(slotMat, Arcane::ResolvedMeshMaterial{ glm::vec4(1, 0, 0, 1) });
    w.materials.emplace(overMat, Arcane::ResolvedMeshMaterial{ glm::vec4(0, 1, 0, 1) });
    const Arcane::Guid withSlot = w.Mesh(1, slotMat);
    const Arcane::Guid noSlot   = w.Mesh(1);
    Astra::Entity a = w.Spawn(glm::vec3(0), withSlot, overMat);
    Astra::Entity b = w.Spawn(glm::vec3(0), withSlot);
    Astra::Entity c = w.Spawn(glm::vec3(0), noSlot);
    Astra::Entity d = w.Spawn(glm::vec3(0), withSlot, Arcane::Guid::Generate());   // valid Guid, absent from the table
    w.Frame();
    CHECK(w.Staged(w.RowOf(a))->baseColor == glm::vec4(0, 1, 0, 1));
    CHECK(w.Staged(w.RowOf(b))->baseColor == glm::vec4(1, 0, 0, 1));
    CHECK(w.Staged(w.RowOf(c))->baseColor == glm::vec4(1, 1, 1, 1));
    CHECK(w.Staged(w.RowOf(d))->baseColor == glm::vec4(1, 0, 0, 1));
}

TEST_CASE("GpuSceneSync: a material whose resolved colour changed re-stages the row without any component write", "[gpuscene][material]")
{
    World w;
    const Arcane::Guid slotMat = Arcane::Guid::Generate();
    w.materials.emplace(slotMat, Arcane::ResolvedMeshMaterial{ glm::vec4(1, 0, 0, 1) });
    Astra::Entity e = w.Spawn(glm::vec3(0), w.Mesh(1, slotMat));
    w.Frame();
    w.materials[slotMat].baseColor = glm::vec4(0, 0, 1, 1);   // the resolver re-read the .arcmat
    w.Frame();
    REQUIRE(w.stage.rows.size() == 1);
    CHECK(w.Staged(w.RowOf(e))->baseColor == glm::vec4(0, 0, 1, 1));
    CHECK(w.Staged(w.RowOf(e))->prevModel == w.Staged(w.RowOf(e))->model);   // a material change is not a move
}

TEST_CASE("GpuSceneSync: materialSlot copies the resolved bindless slot; override repaints every section", "[gpuscene][material]")
{
    World w;
    const Arcane::Guid s0 = Arcane::Guid::Generate(), s1 = Arcane::Guid::Generate(), over = Arcane::Guid::Generate();
    Arcane::ResolvedMeshMaterial m0{ glm::vec4(1, 0, 0, 1) }; m0.materialSlot = 7;
    Arcane::ResolvedMeshMaterial m1{ glm::vec4(0, 1, 0, 1) }; m1.materialSlot = 9;
    w.materials.emplace(s0, m0); w.materials.emplace(s1, m1);
    w.materials.emplace(over, Arcane::ResolvedMeshMaterial{ glm::vec4(0, 0, 1, 1) });
    const Arcane::Guid mesh = w.Mesh(2);
    w.meshes[mesh].slots[0].material = s0;
    w.meshes[mesh].slots[1].material = s1;
    Astra::Entity plain = w.Spawn(glm::vec3(0), mesh);
    Astra::Entity painted = w.Spawn(glm::vec3(0), mesh, over);
    w.Frame();
    CHECK(w.Staged(w.RowOf(plain, 0))->materialSlot == 7);
    CHECK(w.Staged(w.RowOf(plain, 1))->materialSlot == 9);
    CHECK(w.Staged(w.RowOf(painted, 0))->baseColor == glm::vec4(0, 0, 1, 1));
    CHECK(w.Staged(w.RowOf(painted, 1))->baseColor == glm::vec4(0, 0, 1, 1));
    CHECK(w.Staged(w.RowOf(painted, 0))->materialSlot == Arcane::kGpuInvalidMaterialSlot);
}

TEST_CASE("GpuSceneSync: nil mesh, a Guid not in the table, and an entity missing WorldTransform get no rows", "[gpuscene]")
{
    World w;
    Astra::Entity nil = w.Spawn(glm::vec3(0), Arcane::Guid{});
    Astra::Entity absent = w.Spawn(glm::vec3(0), Arcane::Guid::Generate());
    Astra::Entity noWorld = w.reg.CreateEntity();
    w.reg.AddComponent<Arcane::MeshRenderer>(noWorld, Arcane::MeshRenderer{ w.Mesh(), {} });
    w.Frame();
    CHECK(w.mirror.slots.TryGet(nil) == nullptr);
    CHECK(w.mirror.slots.TryGet(absent) == nullptr);
    CHECK(w.mirror.slots.TryGet(noWorld) == nullptr);
    CHECK(w.stage.rows.empty());
}

TEST_CASE("BuildGpuSceneFrame: capacities prefix-sum by batch id; only coarse-visible rows are written; empty batches are not emitted; nearest first", "[gpuscene][frame]")
{
    World w;
    const Arcane::Guid cube = w.Mesh(1);
    const Arcane::Guid other = w.Mesh(1);
    // `near` / `far` are <windef.h> macros (they expand to nothing), hence the names.
    Astra::Entity nearest  = w.Spawn(glm::vec3(0, 0, -2), cube);     // nearest
    Astra::Entity farthest = w.Spawn(glm::vec3(0, 0, -9), cube);
    Astra::Entity mid  = w.Spawn(glm::vec3(0, 0, -5), other);
    Astra::Entity off  = w.Spawn(glm::vec3(80, 0, -5), other);   // outside
    Astra::Entity gone = w.Spawn(glm::vec3(80, 0, -5), w.Mesh(1));   // its whole batch is off-screen
    w.Frame();
    const Arcane::ViewTransform view = Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 10.0f, glm::uvec2{ 800, 600 });
    Arcane::VisibleSet vis;
    Arcane::BuildVisibleSet(w.reg, view, vis);
    Arcane::GpuSceneFrame frame;
    Arcane::BuildGpuSceneFrame(w.mirror, &vis, w.reg.GetResource<Arcane::MeshTable>(), view, frame);

    REQUIRE(frame.batches.size() == 2);                    // cube batch, other batch; gone's batch not emitted
    CHECK(frame.batches[0].mesh == cube);                  // nearDepth 1 (near's front face at z=-1)
    CHECK(frame.batches[1].mesh == other);                 // nearDepth 4
    CHECK(frame.batches[0].argIndex == 0);
    CHECK(frame.batches[1].argIndex == 1);
    CHECK(frame.args[0].instanceNum == 2);
    CHECK(frame.args[1].instanceNum == 1);
    CHECK(frame.args[0].indexNum == frame.batches[0].indexCount);
    CHECK(frame.batches[0].capacity == 2);
    CHECK(frame.batches[1].capacity == 2);                 // `other` has two resident rows (mid + off), one visible
    CHECK(frame.batches[1].firstOutput == 2);
    CHECK(frame.rowCount == 5);
    // The visible index region of the cube batch holds exactly nearest's and farthest's rows (any order).
    std::vector<std::uint32_t> cubeRows{ frame.visibleIndices[0], frame.visibleIndices[1] };
    std::sort(cubeRows.begin(), cubeRows.end());
    std::vector<std::uint32_t> expected{ w.RowOf(nearest), w.RowOf(farthest) };
    std::sort(expected.begin(), expected.end());
    CHECK(cubeRows == expected);
    CHECK(frame.visibleIndices[2] == w.RowOf(mid));
    CHECK(frame.stats.total == 5);
    CHECK(frame.stats.coarseVisible == 3);
    CHECK(frame.stats.batches == 2);
    CHECK(frame.stats.draws == 2);
    CHECK(frame.HasDraws());
}

TEST_CASE("BuildGpuSceneFrame: no VisibleSet means every row is written", "[gpuscene][frame]")
{
    World w;
    w.Spawn(glm::vec3(0), w.Mesh(1));
    w.Spawn(glm::vec3(500, 0, 0), w.Mesh(1));
    w.Frame();
    Arcane::GpuSceneFrame frame;
    Arcane::BuildGpuSceneFrame(w.mirror, nullptr, w.reg.GetResource<Arcane::MeshTable>(), Arcane::ViewTransform{}, frame);
    CHECK(frame.stats.coarseVisible == 2);
    CHECK(frame.batches.size() == 2);
}

TEST_CASE("BuildGpuSceneFrame: each batch's draw range is the MESH TABLE's section (indexOffset / indexCount / baseIndex), the single-section cube is the whole index range", "[gpuscene][frame]")
{
    // The successor of the retired per-slot pins: the section -> draw-range
    // mapping is asserted against the mesh table's own section values, never
    // against the frame's other fields.
    World w;
    const Arcane::Guid cube  = w.Mesh(1);
    const Arcane::Guid split = w.Mesh(1);
    {
        // Three sections with DIFFERENT offsets and counts (0/6, 6/3, 9/3).
        Arcane::MeshEntry& entry = w.meshes[split];
        entry.data.sections = { Arcane::MeshSection{ "a", 0, 6, 0 },
                                Arcane::MeshSection{ "b", 6, 3, 1 },
                                Arcane::MeshSection{ "c", 9, 3, 2 } };
        entry.slots = { Arcane::MeshSlot{ "a", {} }, Arcane::MeshSlot{ "b", {} }, Arcane::MeshSlot{ "c", {} } };
    }
    w.Spawn(glm::vec3(0, 0, -2), split);
    w.Spawn(glm::vec3(0, 0, -5), cube);
    w.Frame();
    Arcane::GpuSceneFrame frame;
    Arcane::BuildGpuSceneFrame(w.mirror, nullptr, w.reg.GetResource<Arcane::MeshTable>(), Arcane::ViewTransform{}, frame);
    REQUIRE(frame.batches.size() == 4);
    REQUIRE(frame.args.size() == 4);

    const std::vector<Arcane::MeshSection>& sections = w.meshes[split].data.sections;
    const std::uint32_t cubeIndices = static_cast<std::uint32_t>(w.meshes[cube].data.indices.size());
    REQUIRE(cubeIndices == 36);
    std::uint32_t seen = 0;
    for (const Arcane::GpuBatchDraw& b : frame.batches)
    {
        REQUIRE(b.argIndex < frame.args.size());
        const Arcane::DrawIndexedArgs& a = frame.args[b.argIndex];
        if (b.mesh == split)
        {
            REQUIRE(b.section < sections.size());
            const Arcane::MeshSection& s = sections[b.section];
            CHECK(b.indexOffset == s.indexOffset);
            CHECK(b.indexCount  == s.indexCount);
            CHECK(a.baseIndex   == s.indexOffset);
            CHECK(a.indexNum    == s.indexCount);
            CHECK(a.instanceNum == 1);
            seen |= 1u << b.section;
        }
        else
        {
            CHECK(b.mesh == cube);
            CHECK(b.section == 0);
            CHECK(b.indexOffset == 0);
            CHECK(b.indexCount  == cubeIndices);
            CHECK(a.baseIndex   == 0);
            CHECK(a.indexNum    == cubeIndices);
            seen |= 1u << 3;
        }
    }
    CHECK(seen == 0b1111u);   // all three split sections and the cube were each emitted once
}

TEST_CASE("RowSpanAllocator: first-fit reuse, split, and high water", "[gpuscene]")
{
    Arcane::RowSpanAllocator a;
    CHECK(a.Allocate(3) == 0);
    CHECK(a.Allocate(2) == 3);
    CHECK(a.HighWater() == 5);
    a.Free(0, 3);
    CHECK(a.Allocate(1) == 0);   // splits the freed span
    CHECK(a.Allocate(2) == 1);
    CHECK(a.Allocate(1) == 5);   // nothing free fits -> bump
    CHECK(a.HighWater() == 6);
}

// ---- F3 plan 1 T8: the host helper (Host/GpuSceneHost.hpp) ----------------
// PrepareSceneForRender is the three calls every host makes between the
// schedulers and the render scheduler; it is header-only so this case can
// drive it without a device (a null GpuScene* reads as "never synced").

TEST_CASE("PrepareSceneForRender: fills views[0] from the main view and views[1] from a differing mesh view; the frame culls against the mesh view", "[gpuscene][host]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0, 0, -5), w.Mesh());
    Arcane::TransformPropagationSystem{}(w.reg);
    Arcane::BoundsSystem{}(w.reg);
    const Arcane::ViewTransform main = Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 10.0f, glm::uvec2{ 800, 600 });
    const Arcane::ViewTransform mesh = Arcane::ViewTransform::Perspective(glm::vec3(0, 0, 10), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0),
                                                                          60.0f, glm::uvec2{ 800, 600 }, 0.1f, 100.0f);
    Arcane::GpuSceneFrame frame;
    Arcane::PrepareSceneForRender(w.reg, main, mesh, nullptr, frame);
    const Arcane::SceneVisibility* sv = w.reg.GetResource<Arcane::SceneVisibility>();
    REQUIRE(sv);
    REQUIRE(sv->views.size() == 2);
    CHECK(sv->views[0].Contains(e));
    CHECK(sv->views[1].Contains(e));
    CHECK(frame.stats.coarseVisible == 1);
    CHECK(frame.HasDraws());
    // The same main view twice: one view only, and the frame reads views[0].
    Arcane::PrepareSceneForRender(w.reg, main, main, nullptr, frame);
    CHECK(w.reg.GetResource<Arcane::SceneVisibility>()->views.size() == 1);
    // No mesh view: the frame has no draws but views[0] is still built (sprites and picking cull).
    Arcane::PrepareSceneForRender(w.reg, main, std::nullopt, nullptr, frame);
    CHECK_FALSE(frame.HasDraws());
    CHECK(w.reg.GetResource<Arcane::SceneVisibility>()->views.size() == 1);
}

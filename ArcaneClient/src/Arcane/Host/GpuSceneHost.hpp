#pragma once

// PrepareSceneForRender -- the host's three calls per frame (F3, spec s4/s5):
// build the visible set(s), sync the GPU-scene mirror, build the frame. Called
// AFTER the fixed/update schedulers (WorldTransform + WorldBounds current) and
// BEFORE RunLoop::SubmitRender (the sprite sweep reads views[0]). Header-only
// so the two frame drivers (EditorAppFrame.cpp, RuntimeFrame.cpp) share one
// definition and ArcaneTests can drive it without a device.
//
// TWO VIEWS when they differ: in Play the sprites draw with View() (the scene
// camera's orthographic view) while the mesh pass draws with the perspective
// scene camera (RuntimeFrame.cpp's ActivePerspectiveSceneCamera). Each pass
// culls against its own camera: views[0] = main, views[1] = mesh. F5 unifies
// the two passes under one view; until then the second set is the honest one.
//
// NRI-FREE, deliberately: the one device fact this needs -- the mirror
// generation the GpuScene last acknowledged -- comes through the exported
// free function below (defined in Render/Nri/GpuScene.cpp, which is where
// <NRI.h> is visible), so this header pulls neither GpuScene.hpp nor NRI and
// ArcaneTests' ~[gpu] cases include it as freely as GpuSceneSync.hpp.
#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GpuSceneSync.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cstdint>
#include <optional>

namespace Arcane
{
    class GpuScene;   // Render/Nri/GpuScene.hpp; only SyncedGeneration() is read here, through the free function below

    // The mirror generation the device last synced (GpuScene::Apply stamps it
    // on success); 0 for a null device, which GpuSceneSync reads as "never
    // synced" -> full rebuild. Re-declared identically in GpuScene.hpp.
    [[nodiscard]] ARCANE_API std::uint64_t GpuSceneSyncedGeneration(const GpuScene* device) noexcept;

    [[nodiscard]] inline bool SameView(const ViewTransform& a, const ViewTransform& b) noexcept
    {
        return a.view == b.view && a.projection == b.projection && a.viewport == b.viewport;
    }

    // `meshView` is the view the mesh pass will draw with (Edit: the editor
    // view; Play: the perspective scene camera, or nullopt = no mesh pass).
    // `device` may be null (a device-less host, a test): the mirror then
    // rebuilds fully every frame, which is the honest answer for "nothing has
    // ever been synced".
    //
    // ON THE NO-MESH-VIEW PATH the stage still lands in `out.stage`: Sync has
    // already advanced the mirror's lastModel history past the rows it staged,
    // so a host must still declare the mesh pass whenever the frame carries
    // staged rows (MeshSceneDesc::Empty() counts them -- ruling R-D); only the
    // batch table is cleared, since there is no camera to cull against.
    inline void PrepareSceneForRender(Astra::Registry& reg, const ViewTransform& mainView,
                                      const std::optional<ViewTransform>& meshView,
                                      const GpuScene* device, GpuSceneFrame& out)
    {
        SceneVisibility* sv = reg.GetResource<SceneVisibility>();
        if (!sv) sv = reg.EmplaceResource<SceneVisibility>();
        const bool twoViews = meshView && !SameView(*meshView, mainView);
        sv->views.resize(twoViews ? 2 : 1);
        BuildVisibleSet(reg, mainView, sv->views[0]);
        if (twoViews)
            BuildVisibleSet(reg, *meshView, sv->views[1]);

        GpuSceneMirror* mirror = reg.GetResource<GpuSceneMirror>();
        if (!mirror) mirror = reg.EmplaceResource<GpuSceneMirror>();
        GpuSceneSync(reg, *mirror, GpuSceneSyncedGeneration(device), out.stage);

        if (!meshView)
        {
            out.batches.clear(); out.args.clear(); out.visibleIndices.clear();
            out.rowCount = out.stage.rowCapacity; out.stats = {};
            return;   // no mesh pass this frame; the stage still lands (rows stay current for the frame that has one)
        }
        const VisibleSet* meshVis = twoViews ? &sv->views[1] : &sv->views[0];
        BuildGpuSceneFrame(*mirror, meshVis, reg.GetResource<MeshTable>(), *meshView, out);
    }

    // THE DROPPED-STAGE REMEDY (ruling R-F). GpuSceneSync records lastModel and
    // lastSyncTick in the same step it stages a row, and the device only
    // stamps the synced generation inside GpuScene::Apply -- so a frame whose
    // graph outcome was anything but Presented (Skipped: a collapsed viewport
    // panel, a zero-sized surface, an OUT_OF_DATE acquire; Failed) has a stage
    // that never reached the GPU and would never be re-staged: the next
    // frame's out.Clear() drops it, and the rows draw stale (or never-written)
    // bytes when they come into view. A host calls this on every such outcome
    // AFTER PrepareSceneForRender ran for that frame. It hands the mirror a
    // NEW generation, so the next GpuSceneSync sees the device's stamp
    // mismatch and re-stages every live row with prev == model -- spans and
    // batch ids stay put (cheaper and safer than dropping the resource). A
    // registry with no mirror yet has nothing to lose: no-op.
    inline void GpuSceneInvalidate(Astra::Registry& reg)
    {
        if (GpuSceneMirror* m = reg.GetResource<GpuSceneMirror>())
            m->generation = GpuSceneMirror::NextGeneration();
    }
}

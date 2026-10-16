#include <Arcane/Render/ExtractFrame.hpp>

#include <Arcane/Base/ExtractAccounting.hpp>
#include <Arcane/Base/Log.hpp>                   // ARC_WARN (no-camera / multi-camera, once)
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/GpuSceneSync.hpp>        // GpuSceneSync, BuildGpuSceneFrame, MeshSectionRanges
#include <Arcane/Render/Grid2D.hpp>
#include <Arcane/Render/OverlayBatchCollector.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Render/RenderSystems.hpp>       // RenderSubmissionSystem
#include <Arcane/Render/SpriteGeometry.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneCamera.hpp>          // ActiveSceneCamera, ActivePerspectiveSceneCamera
#include <Arcane/Scene/SceneResources.hpp>       // RenderContext2D, MeshTable

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Arcane
{
    // Defined in Nri/GpuScene.cpp. Declared here, not via GpuScene.hpp: that
    // header includes NRI.h, and this TU already pulled windows.h through
    // Astra (the ERROR macro would corrupt nri::Message::ERROR).
    std::uint64_t GpuSceneSyncedGeneration(const GpuScene* device) noexcept;

    namespace
    {
        // Rotate a body-local offset by the body's world angle (radians).
        glm::vec2 RotateVec(glm::vec2 v, float angle)
        {
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return glm::vec2(v.x * c - v.y * s, v.x * s + v.y * c);
        }

        // GpuSceneHost.hpp's SameView. That header is ArcaneClient's;
        // ArcaneRender does not include it.
        [[nodiscard]] bool SameView(const ViewTransform& a, const ViewTransform& b) noexcept
        {
            return a.view == b.view && a.projection == b.projection && a.viewport == b.viewport;
        }

        // The sprite batch, copied. The host already called Begin, SetGlobals
        // and SetRenderContext on its persistent batcher, and the sprite
        // materials are registered on that batcher, so extract submits into
        // it rather than a fresh one: Begin would clear the host's geometry
        // and reset its view-projection, SetGlobals(GlobalParams{}) would wipe
        // Time/DeltaTime, and QuadWorld on a fresh batcher rewrites every
        // registered id to kMaterialSprite. A null batcher submits nothing
        // and leaves the packet's batch empty.
        void ExtractSprites(Astra::Registry& reg, RenderFramePacket& packet)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            Batcher2D* batcher = ctx ? ctx->batcher : nullptr;
            if (!batcher)
                return;

            // The sprite view the caller published: scene camera, editor view,
            // or the host's stored view. Identity only when none of those exist.
            batcher->SetViewProjection(packet.spriteView
                ? packet.spriteView->view.ViewProjection()
                : glm::mat4{ 1.0f });   // does not clear geometry
            RenderSubmissionSystem{}(reg);
            const Batch2DDrained drained = batcher->Drain();
            packet.batchVertices.assign(drained.vertices.begin(), drained.vertices.end());
            packet.batchIndices.assign(drained.indices.begin(), drained.indices.end());
            packet.batchSpans.assign(drained.spans.begin(), drained.spans.end());
            if (drained.globals)
                packet.globals = *drained.globals;
            packet.viewProjection = drained.viewProjection;
        }
    }

    void CollectPickables(Astra::Registry& registry, std::vector<PickDrawable>& out)
    {
        namespace Phys = ::Manifold2D::Physics;
        // ---- PASS 1: sprites -------------------------------------------------
        // The DRAWN set, through the DRAWN corner rule. The view filter is
        // RenderSystems.hpp's own (a Hidden sprite is not drawn, so it is not
        // pickable either), and base size + pivot come from the SpriteTable
        // exactly as submission resolves them -- an unresolved or non-Rect
        // sprite is a 1x1 m quad at the centre pivot -- so the silhouette
        // keeps matching the drawn quad.
        {
            const SpriteTable* spriteTable = registry.GetResource<SpriteTable>();
            const VisibleSet* vis = MainVisibleSet(registry);   // nullptr: cull nothing (spec s4)
            auto spriteView = registry.CreateView<const WorldTransform, const SpriteRenderer, Astra::Not<Hidden>>();
            spriteView.ForEach([&](Astra::Entity e, const WorldTransform& xf, const SpriteRenderer& sp)
            {
                if (vis && !vis->Contains(e))
                    return;   // off-screen this frame (the CPU coarse stage)

                const SpriteEntry* entry =
                    (sp.shape == SpriteShape::Rect && spriteTable)
                        ? spriteTable->Resolve(sp.sprite)
                        : nullptr;
                const glm::vec2 baseSize = entry ? entry->sizeMeters : glm::vec2(1.0f);
                const glm::vec2 pivot    = entry ? entry->pivot      : glm::vec2(0.5f);

                // THE ONE CORNER RULE (SpriteGeometry.hpp): the same four world
                // points the sprite submission draws, so the silhouette matches
                // the drawn quad under ANY projection -- a mirrored or off-centre
                // pivot included (the retired "centre + angle" derivation placed
                // a mirrored off-centre pivot on the wrong side).
                PickDrawable d;
                d.entity  = e;
                d.kind    = PickDrawable::Kind::Quad;
                d.corners = SpriteWorldQuad(xf.matrix, baseSize, pivot).corners;
                out.push_back(d);
            });
        }

        // ---- PASS 2: physics colliders ---------------------------------------
        // One PickDrawable per Arcane::Fixture2D on every live tracked body, iterated via
        // an archetype-stable View<Arcane::Collider2D, Arcane::PhysicsBodyRef2D> -- NOT the
        // Arcane::PhysicsWorld2D::entityToBody unordered_map. The drawable index IS the
        // hit-proxy id (id = index+1), so the order must be DETERMINISTIC: the
        // same rule Arcane::PhysicsSystem2D's create pass follows ("order must not depend on
        // unordered_map hash/bucket layout"). The body pose is read from the live
        // PhysicsWorld via Arcane::PhysicsBodyRef2D::handle so the silhouette registers with
        // the physics-debug overlay; fixture dims + local offset are scaled by
        // Arcane::PhysicsBodyRef2D::appliedScale (the scale the create pass baked into the
        // body's fixtures, mirroring MakeScaledShape / MakeFixtureDef) so a scaled
        // body picks at its drawn size. Polygon fixtures carry no authored vertex
        // array (see PhysicsComponents.hpp) -- v1 approximates with the fixture's
        // halfW/halfH box, per design spec SS8.2 ("Polygon -> Box using its AABB").
        //
        // No physics world on this registry means no colliders -- NOT an early
        // return: the meshes below are collected regardless.
        if (Arcane::PhysicsWorld2D* res = registry.GetResource<Arcane::PhysicsWorld2D>(); res && Arcane::Detail::Physics2D::Access::Solver(*res))
        {
            Phys::PhysicsWorld& world = *Arcane::Detail::Physics2D::Access::Solver(*res);

            auto colliderView = registry.CreateView<const Arcane::Collider2D, const Arcane::PhysicsBodyRef2D>();
            colliderView.ForEach([&](Astra::Entity entity, const Arcane::Collider2D& col, const Arcane::PhysicsBodyRef2D& ref)
            {
                if (ref.handle == Phys::kInvalidBody) return;
                if (!world.IsValid(ref.handle))       return;

                const Phys::Vec2 bp = world.Position(ref.handle);
                const glm::vec2  bodyPos(static_cast<float>(bp.x), static_cast<float>(bp.y));
                const float      bodyAngle = static_cast<float>(world.GetAngle(ref.handle));

                // The silhouette's world z: the solver is planar, so it comes
                // from the entity's own WorldTransform (its translation column)
                // -- what places the body's sprite -- and is 0 without one.
                // Looked up ONCE per entity, not per fixture.
                const WorldTransform* xf = std::as_const(registry).GetComponent<WorldTransform>(entity);
                const float worldZ = xf ? xf->matrix[3].z : 0.0f;

                // Scale the create pass baked into this body's fixtures (identity
                // unless the entity carries an authored Transform.scale). Mirrors
                // Arcane::PhysicsSystem2D::MakeScaledShape: per-axis for Aabb, |sx| length /
                // |sy| radius for Capsule, max(|sx|,|sy|) for Circle.
                const glm::vec2 scale = ref.appliedScale;
                const float     sx    = std::abs(scale.x);
                const float     sy    = std::abs(scale.y);
                const float     sMax  = std::max(sx, sy);

                for (const Arcane::Fixture2D& fx : col.fixtures)
                {
                    // Arcane::Fixture2D local offset scales per-axis with the body's baked scale
                    // (signed, matching MakeFixtureDef), then rotates into world space.
                    const glm::vec2 localScaled(fx.localPos.x * scale.x, fx.localPos.y * scale.y);
                    const glm::vec2 worldCenter  = bodyPos + RotateVec(localScaled, bodyAngle);
                    const float     fixtureAngle = bodyAngle + fx.localAngle;

                    // WORLD space, METRES, world-sense angle: nothing here knows
                    // the camera. The id pass projects.
                    PickDrawable d;
                    d.entity = entity;
                    d.center = glm::vec3(worldCenter, worldZ);
                    d.angle  = fixtureAngle;

                    switch (fx.kind)
                    {
                    case Arcane::ShapeKind2D::Circle:
                        d.kind   = PickDrawable::Kind::Circle;
                        d.radius = fx.radius * sMax;
                        break;
                    case Arcane::ShapeKind2D::Capsule:
                        d.kind    = PickDrawable::Kind::Capsule;
                        d.halfLen = fx.halfLen * sx;
                        d.radius  = fx.radius  * sy;
                        break;
                    case Arcane::ShapeKind2D::Aabb:
                        d.kind        = PickDrawable::Kind::Box;
                        d.halfExtents = glm::vec2(fx.halfW * sx, fx.halfH * sy);
                        break;
                    case Arcane::ShapeKind2D::Polygon:
                        // v1: no vertex data available -- fall back to the fixture's
                        // halfW/halfH box fields (scaled) as its AABB stand-in.
                        d.kind        = PickDrawable::Kind::Box;
                        d.halfExtents = glm::vec2(fx.halfW * sx, fx.halfH * sy);
                        break;
                    }

                    out.push_back(d);
                }
            });
        }

        // ---- PASS 3: meshes -------------------------------------------------
        // One drawable per MeshRenderer entity that GpuSceneSync would stage
        // (Render/GpuSceneSync.hpp: WorldTransform + MeshRenderer, not Hidden,
        // resolved through the MeshTable to a mesh with sections). The whole mesh is
        // one silhouette -- sections carry materials, not identity -- so an entity
        // gets ONE id whatever its section count.
        {
            const MeshTable* meshTable = registry.GetResource<MeshTable>();
            const VisibleSet* vis = MainVisibleSet(registry);   // nullptr: cull nothing (spec s4)
            auto meshView = registry.CreateView<const WorldTransform, const MeshRenderer, Astra::Not<Hidden>>();
            meshView.ForEach([&](Astra::Entity e, const WorldTransform& xf, const MeshRenderer& mr)
            {
                if (vis && !vis->Contains(e))
                    return;   // off-screen this frame (the CPU coarse stage)

                const MeshEntry* entry = meshTable ? meshTable->Resolve(mr.mesh) : nullptr;
                if (!entry || entry->data.sections.empty())
                    return;
                PickDrawable d;
                d.entity = e;
                d.kind   = PickDrawable::Kind::Mesh;
                d.world  = xf.matrix;
                d.mesh   = mr.mesh;
                out.push_back(d);
            });
        }
    }

    const RenderFramePacket& ExtractFrame(Astra::Registry& reg, const ExtractRequest& req, PacketRing& ring)
    {
        NoteExtractFrameCall();
        RenderFramePacket& packet = ring.BeginExtract();
        packet.viewport = req.viewport;
        packet.origin.translation = { 0.0f, 0.0f, 0.0f };

        // The two views. Edit uses the one editor camera for both. Otherwise
        // the orthographic scene camera is the sprite view and the perspective
        // scene camera is the mesh view. The cull below still reads mainView
        // and meshView, with the cull viewport forced to req.viewport.
        if (req.editorView)
        {
            packet.spriteView = SceneCameraView{};
            packet.spriteView->view = *req.editorView;
            packet.meshView = *req.editorView;   // Edit: one camera for both (EditorAppFrame.cpp)
        }
        else
        {
            // Once per process, the same two warnings PushSceneCamera used to
            // print. A host run is one scene's worth of complaint.
            static bool warnedNoSceneCamera = false;
            static bool warnedMultiSceneCamera = false;
            int camCount = 0;
            packet.spriteView = ActiveSceneCamera(reg, req.viewport, &camCount);
            packet.spriteViewFromScene = packet.spriteView.has_value();
            packet.meshView = ActivePerspectiveSceneCamera(reg, req.viewport);
            if (!packet.spriteView && !warnedNoSceneCamera)
            {
                warnedNoSceneCamera = true;
                int total = 0;
                reg.CreateView<const Camera>().ForEach([&](Astra::Entity, const Camera&) { ++total; });
                if (total == 0)
                    ARC_WARN("scene has no Camera component at all -- nothing sets the view. "
                             "Add a Camera component to an entity (a New Scene ships one).");
                else
                    ARC_WARN("scene carries {} Camera component(s) but none is usable "
                             "as the sprite camera.", total);
            }
            if (camCount > 1 && !warnedMultiSceneCamera)
            {
                warnedMultiSceneCamera = true;
                ARC_WARN("scene carries {} active Camera entities -- the first found wins", camCount);
            }
            // No scene camera: keep the host's stored view, as PushSceneCamera
            // does -- a plugin driving ClientRuntime::SetView still works.
            // RenderContext2D::view is that stored view (ClientRuntime.cpp).
            if (!packet.spriteView)
                if (const RenderContext2D* rc = reg.GetResource<RenderContext2D>())
                {
                    packet.spriteView = SceneCameraView{};
                    packet.spriteView->view = rc->view;
                }
        }
        const std::optional<ViewTransform> mainView =
            packet.spriteView ? std::optional<ViewTransform>(packet.spriteView->view) : std::nullopt;
        const std::optional<ViewTransform>& meshView = packet.meshView;
        ViewTransform cullView = mainView.value_or(ViewTransform{});
        cullView.viewport = req.viewport;

        SceneVisibility* sv = reg.GetResource<SceneVisibility>();
        if (!sv)
            sv = reg.EmplaceResource<SceneVisibility>();
        const bool twoViews = meshView && !SameView(*meshView, cullView);
        sv->views.resize(twoViews ? 2 : 1);
        BuildVisibleSet(reg, cullView, sv->views[0]);
        if (twoViews)
            BuildVisibleSet(reg, *meshView, sv->views[1]);
        packet.visible = sv->views[0];
        if (twoViews)
            packet.meshVisible = sv->views[1];

        GpuSceneMirror* mirror = reg.GetResource<GpuSceneMirror>();
        if (!mirror)
            mirror = reg.EmplaceResource<GpuSceneMirror>();
        GpuSceneStage stage;
        GpuSceneSync(reg, *mirror, GpuSceneSyncedGeneration(req.device), stage);
        if (meshView)
        {
            const std::vector<MeshSectionRange> sections = MeshSectionRanges(reg.GetResource<MeshTable>());
            BuildGpuSceneFrame(*mirror, twoViews ? &packet.meshVisible : &packet.visible, sections, *meshView, packet.mesh);
        }
        else
        {
            // PrepareSceneForRender's no-mesh-view arm (GpuSceneHost.hpp): the
            // stage still lands; only the batch table is empty.
            packet.mesh.batches.clear();
            packet.mesh.args.clear();
            packet.mesh.visibleIndices.clear();
            packet.mesh.rowCount = stage.rowCapacity;
            packet.mesh.stats = {};
        }
        packet.mesh.stage = stage;

        ExtractSprites(reg, packet);
        CollectPickables(reg, packet.pickDrawables);

        // Overlays, in submission order: the 2D grid, the 3D grid request,
        // physics debug, the camera rect, then the gizmo. The gizmo arrives
        // already tagged; Append keeps its own line/fill order.
        if (req.showGrid2d && packet.spriteView)
        {
            Grid2DStyle style;
            style.minorAlpha = req.gridMinorAlpha;
            style.majorAlpha = req.gridMajorAlpha;
            style.lineRgb = req.gridLineRgb;
            style.lineThicknessPx = req.gridLineThicknessPx;
            style.maxLinesPerAxis = req.gridMaxLinesPerAxis;
            style.axisX = req.gridAxisX;
            style.axisY = req.gridAxisY;
            const float ppm = GridPixelsPerMetre(packet.spriteView->view);
            EmitGrid2D(packet.spriteView->view,
                       PlanGrid2D(ppm, req.gridMinorAlpha, req.gridMajorAlpha),
                       style, packet.overlay);
        }
        if (req.showGrid3d)
        {
            packet.overlay.hasGrid3d = true;
            packet.overlay.grid3d = req.grid3d;
        }
        if (req.physicsDebug && packet.spriteView)
        {
            PhysicsWorld2D* phys = reg.GetResource<PhysicsWorld2D>();
            std::optional<Detail::Physics2D::BodyHandle> selected;
            if (phys && req.physicsSelectedBody)
            {
                const auto& entities = Detail::Physics2D::Access::Entities(*phys);
                if (const auto it = entities.find(*req.physicsSelectedBody); it != entities.end())
                    selected = it->second;
            }
            const bool drawPhysics = req.physicsWholeWorld || (!req.playMode && selected.has_value());
            const Detail::Physics2D::PhysicsWorld* solver =
                phys ? Detail::Physics2D::Access::Solver(*phys) : nullptr;
            const std::optional<Affine2D> affine = packet.spriteView->view.AsAffine2D();
            if (drawPhysics && solver && affine)
            {
                OverlayBatchCollector collector(packet.overlay);
                collector.Begin(req.viewport.x, req.viewport.y);
                PhysicsDebugDrawOptions2D opts = MakePhysicsDebugDrawOptions();
                opts.view = *affine;
                if (const RenderContext2D* rc = reg.GetResource<RenderContext2D>())
                    opts.alpha = rc->alpha;
                opts.interp = reg.GetResource<PhysicsInterpBuffer2D>();
                opts.velocities = false;
                opts.comMarkers = false;
                opts.orientations = false;
                opts.contacts = req.physicsWholeWorld;
                if (!req.physicsWholeWorld && selected)
                    opts.onlyBody = (static_cast<std::uint64_t>(selected->index) << 32) | selected->generation;
                DrawPhysicsDebug(*solver, collector, opts);
            }
        }
        if (req.cameraRect && req.editorView && !req.playMode)
        {
            if (const auto cam = ActiveSceneCamera(reg, req.viewport))
            {
                const float aspect = (req.viewport.y > 0u)
                    ? (float(req.viewport.x) / float(req.viewport.y)) : 1.0f;
                const float hw = cam->halfHeight * aspect;
                const float hh = cam->halfHeight;
                const glm::vec2 c = cam->worldCenter;
                const ViewTransform& ev = *req.editorView;
                const glm::vec3 p[4] = {
                    ev.WorldToScreen(glm::vec3(c.x - hw, c.y - hh, 0.0f)),
                    ev.WorldToScreen(glm::vec3(c.x + hw, c.y - hh, 0.0f)),
                    ev.WorldToScreen(glm::vec3(c.x + hw, c.y + hh, 0.0f)),
                    ev.WorldToScreen(glm::vec3(c.x - hw, c.y + hh, 0.0f)),
                };
                bool finite = true;
                for (const glm::vec3& q : p)
                    finite = finite && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
                if (finite)
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        OverlayLine line{};
                        line.a = glm::vec3(glm::vec2(p[i]), 0.0f);
                        line.b = glm::vec3(glm::vec2(p[(i + 1) % 4]), 0.0f);
                        line.thickness = 1.0f;
                        line.color = req.cameraFrameColor;
                        line.depth = OverlayDepthClass::AlwaysOnTop;
                        line.space = OverlayColorSpace::Linear;
                        line.screenSpace = true;
                        packet.overlay.AddLine(line);
                    }
                }
            }
        }
        packet.overlay.Append(req.gizmoPrims);
        return packet;
    }
}

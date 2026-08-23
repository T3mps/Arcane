#include "Documents/MeshDocument.hpp"

#include "Panels/AssetBrowser.hpp"
#include "Widgets/EditorWidgets.hpp"   // RangedDragFloat/RangedDragInt

// The preview vehicle. Include-order note for anything moved above it: this
// reaches <NRI.h> and Extensions/NRIDeviceCreation.h, whose nri::Message
// enumerator is literally named ERROR -- the same collision every file under
// Render/Nri documents, and the same reason ShaderEditorDocument.cpp keeps
// this block where it is rather than first.
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/nodes/MeshNode.hpp>        // MeshSceneDesc / MeshInstance
#include <Arcane/Render/Nri/nodes/ImGuiNriNode.hpp>    // InvalidateUserTextureNow
#include <Arcane/Host/HostConfig.hpp>                  // CreateOffscreen's knobs

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Project/Project.hpp>

#include <Astra/Reflection/Attribute.hpp>   // Astra::Range

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>     // lookAtRH / perspectiveRH_ZO

#include <imgui.h>

#include <cmath>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
        const char* SourceLabel(Arcane::MeshSource s)
        {
            switch (s)
            {
                case Arcane::MeshSource::Plane:    return "Plane";
                case Arcane::MeshSource::Cube:     return "Cube";
                case Arcane::MeshSource::UvSphere: return "UV Sphere";
                case Arcane::MeshSource::Cylinder: return "Cylinder";
                case Arcane::MeshSource::Capsule:  return "Capsule";
            }
            return "Unknown";   // unreachable for a well-formed enum value; never refuses a draw
        }

        // One completed field gesture (or single-frame commit) as an undo
        // step -- same shape and doc-identity reasoning as SpriteDocument.cpp's
        // SpriteDataEditCommand (:31-59): the step holds the DOCUMENT weakly
        // through an anchor and forwards to whatever it currently points at,
        // because a raw MeshDocument* dangles the moment the document closes
        // with steps still on the shared stack, and the stack outlives every
        // document (EditorApp owns it; DocumentHost::Close erases the
        // document synchronously).
        class MeshDataEditCommand final : public Arcane::ICommand
        {
        public:
            MeshDataEditCommand(std::weak_ptr<MeshDocument*> anchor, std::string label,
                                Arcane::MeshAssetData before, Arcane::MeshAssetData after)
                : m_anchor(std::move(anchor)), m_label(std::move(label)),
                  m_before(std::move(before)), m_after(std::move(after))
            {
            }

            void Undo() override { Apply(m_before); }
            void Redo() override { Apply(m_after); }
            const char* Label() const override { return m_label.c_str(); }

        private:
            void Apply(const Arcane::MeshAssetData& data)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplyMeshData(data);
            }

            std::weak_ptr<MeshDocument*> m_anchor;
            std::string                  m_label;
            Arcane::MeshAssetData        m_before;
            Arcane::MeshAssetData        m_after;
        };
    }

    MeshDocument::MeshDocument(Services services, std::filesystem::path path,
                              Arcane::MeshAssetData data)
        : m_services(std::move(services)), m_path(std::move(path)), m_data(std::move(data))
    {
        // Same name-fallback rule as SpriteDocument/ShaderEditorDocument: an
        // empty asset name (hand-authored file, or a pre-name-field asset)
        // falls back to the file stem rather than showing a blank title.
        m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name;
        m_windowLabel = m_title + " (Mesh)###meshdoc_" + m_data.id.ToString();
        // The anchor every undo step routes through; it dies with the document.
        m_anchor = std::make_shared<MeshDocument*>(this);

        RebuildPreviewMesh();
        // No bind/compile event to wait for (BuildMeshData is synchronous and
        // pure), so the vehicle is built right here rather than lazily from
        // Tick/Draw -- this IS the "device-less services allocate no preview
        // resources" behaviour: with nriDevice/hostConfig null, this call is
        // a single `if` that returns immediately.
        EnsurePreviewContext();
    }

    MeshDocument::~MeshDocument()
    {
        // Same close-a-parked-gesture reasoning as SpriteDocument's dtor
        // (SpriteDocument.cpp:76-103): a close that lands between a gesture
        // parking and its next Draw would otherwise strand InTransaction()
        // true editor-wide.
        if (m_services.undo)
            EditGesture::ClosePending(*m_services.undo, m_gesture);

        // LAST: the chrome context's ImGuiNri may hold a view over this
        // document's preview output, so that view has to be handled before
        // (or as part of) the texture's teardown -- DestroyPreviewContext
        // carries the ordering.
        DestroyPreviewContext();
    }

    void MeshDocument::ApplyMeshData(const Arcane::MeshAssetData& data)
    {
        m_data = data;
        // Coarse ledger, same rationale as SpriteDocument::ApplySpriteData:
        // undoing all the way back to the saved bytes still reads dirty. That
        // errs toward a redundant save, never toward silently dropping one.
        m_dirty = true;
        RebuildPreviewMesh();
    }

    void MeshDocument::PushDataEdit(std::string label, const Arcane::MeshAssetData& before)
    {
        // No stack (Play mode / an unwired document) or nothing actually
        // moved -> no step -- identical guard to SpriteDocument::PushDataEdit.
        if (!m_services.undo || before == m_data)
            return;
        m_services.undo->Push(std::make_unique<MeshDataEditCommand>(
            m_anchor, std::move(label), before, m_data));
    }

    void MeshDocument::RebuildPreviewMesh()
    {
        // Independent calls, deliberately: BuildMeshData already returns
        // nullopt exactly when ValidateMeshAsset refuses (MeshAsset.hpp's own
        // contract), but the STRING reason only comes from the validator, and
        // the panel needs both -- "no geometry" (PreviewMesh) and "here is
        // why" (ValidationReason) are two different things a caller reads.
        m_validationReason = Arcane::ValidateMeshAsset(m_data);
        m_previewMesh = Arcane::BuildMeshData(m_data);
    }

    bool MeshDocument::Save()
    {
        if (!Arcane::SaveMeshAsset(m_path, m_data))
            return false;   // failed save: m_dirty stays set, edits are not silently lost
        m_dirty = false;
        // No cache to invalidate -- see the file-top comment on MeshDocument.hpp.
        return true;
    }

    void MeshDocument::Tick(double)
    {
        // The whole render phase. A missing vehicle simply means no preview
        // this Tick -- the same degraded-not-fatal outcome EnsurePreviewContext
        // and RenderPreview both already carry.
        if (m_preview)
            RenderPreview();
    }

    // =========================================================================
    // THE PREVIEW
    // =========================================================================
    void MeshDocument::EnsurePreviewContext()
    {
        // The seam is null in every headless test (no EditorApp at all),
        // which is the whole gate: this function is a no-op there.
        if (m_preview || !m_services.nriDevice || !m_services.hostConfig)
            return;

        // Default NodeSet{} -- this preview draws no chrome, no game HUD and
        // has nothing to pick, exactly like ShaderEditorDocument's own
        // graph-preview vehicle.
        m_preview = Arcane::NriGraphContext::CreateOffscreen(
            *m_services.hostConfig, *m_services.nriDevice, kPreviewSize, kPreviewSize);
        if (!m_preview)
        {
            // Degraded, not fatal: no preview image, already logged + latched
            // inside CreateOffscreen.
            ARC_WARN("MeshDocument '{}': the preview context could not be created -- "
                     "this document shows no preview", m_title);
        }
    }

    void MeshDocument::RenderPreview()
    {
        if (!m_preview)
            return;

        // ALWAYS records a frame below, even when m_previewMesh is nullopt
        // (an invalid param set) -- fix-round Finding 2. Returning early here
        // (the pre-fix shape) left the offscreen texture holding whatever
        // undefined bytes CreateOffscreen's creation left in it, because
        // OffscreenTextureId() reports non-zero -- "there is a texture" -- the
        // INSTANT CreateOffscreen succeeds, independent of whether any frame
        // has ever rendered into it (NriGraphContext.cpp:582-588). `scene`
        // stays default-constructed (empty instances, identity view/
        // projection) on the invalid-data path: NriGraphContext's
        // `wantsMesh` gate (`shape.mesh != nullptr && !shape.mesh->
        // instances.empty()`, NriGraphContext.cpp:1186) reads an EMPTY span
        // as "no mesh pass this frame", so MeshNode is never even declared
        // and only Batch2DNode's plain clear colour (kCanvasClear,
        // Batch2DNode.cpp:45) lands in the texture -- clean, not garbage.
        Arcane::MeshInstance instanceStorage[1]{};
        Arcane::MeshSceneDesc scene;
        if (m_previewMesh)
        {
            // Frame the WHOLE mesh regardless of source/topology: a
            // bounding-sphere radius from ComputeMeshBounds' AABB (half the
            // diagonal, a safe over-estimate for any view direction) placed
            // at a distance that keeps it inside the vertical FOV, plus a
            // margin so the mesh never touches the frame edge.
            const Arcane::MeshBounds bounds = Arcane::ComputeMeshBounds(*m_previewMesh);
            const glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
            float radius = glm::length(bounds.max - bounds.min) * 0.5f;
            if (!(radius > 1e-4f))
                radius = 0.5f;   // degenerate-bounds guard; unreached by any of the five generators today

            constexpr float kFovYDegrees = 45.0f;
            constexpr float kMargin = 1.5f;
            const float distance = radius / std::sin(glm::radians(kFovYDegrees * 0.5f)) * kMargin;
            // A fixed three-quarter viewing direction -- there is no camera
            // authoring here (F4's job), so one angle that reads every one of
            // the five sources reasonably is enough; +Y up matches the
            // engine-wide convention (SceneCamera.hpp).
            const glm::vec3 dir = glm::normalize(glm::vec3(1.0f, 0.75f, 1.0f));
            const glm::vec3 eye = center + dir * distance;

            instanceStorage[0].mesh = &(*m_previewMesh);
            instanceStorage[0].model = glm::mat4(1.0f);   // unit geometry -- see MeshAsset.hpp's UNIT RULE
            // Neutral white: resolving `material`'s actual baseColor/texture
            // into this preview is F2b/F2c's bindless-table territory (the
            // material Guid field here is authored, not rendered-through) --
            // deliberately out of this task's scope. The preview's job is to
            // show the SHAPE.
            instanceStorage[0].baseColor = glm::vec4(1.0f);
            scene.instances = std::span<const Arcane::MeshInstance>(instanceStorage, 1);

            // RH, depth [0,1] -- the same convention SceneCamera.hpp's
            // PerspectiveProjection/ActivePerspectiveSceneCamera document at
            // length; restated by direct call here rather than by including
            // that ECS-facing header, which this device-less, registry-free
            // preview has no other reason to depend on.
            scene.view = glm::lookAtRH(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
            scene.projection = glm::perspectiveRH_ZO(glm::radians(kFovYDegrees), 1.0f,
                                                      0.05f, distance * 4.0f + 1.0f);
            scene.lightDirection = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));
            scene.lightColor = glm::vec3(1.0f);
            // Brighter than MeshSceneDesc's own default ambient (0.05): a
            // lone preview mesh has no fill light of any kind, and the
            // default reads as near-black on its unlit side. A one-off UX
            // choice for this window only -- it has no bearing on how a
            // scene's own mesh instances light (those keep MeshSceneDesc's
            // real default).
            scene.ambient = glm::vec3(0.12f);
        }

        Arcane::NriGraphContext::FrameDesc frame;
        frame.mesh = &scene;   // batch/post/globals all left null: no 2D content, no chain

        const auto outcome = m_preview->RenderFrameOffscreen(frame);
        if (outcome == Arcane::NriGraphContext::FrameOutcome::Failed)
        {
            ARC_ERROR("MeshDocument '{}': the preview frame failed -- dropping this "
                      "document's preview vehicle", m_title);
            DestroyPreviewContext();
        }
    }

    std::uint64_t MeshDocument::PreviewTextureId() const noexcept
    {
        return m_preview ? m_preview->OffscreenTextureId() : 0;
    }

    void MeshDocument::DestroyPreviewContext()
    {
        if (!m_preview)
            return;

        // ===== THE VEHICLE IS RETIRED, NOT DESTROYED (see Services::
        // retireGraphPreview / DocServices' identical field for the full
        // reasoning) =====
        if (m_services.retireGraphPreview)
        {
            m_services.retireGraphPreview(std::move(m_preview));
            return;
        }

        // ===== NO SINK: THE CROSS-CONTEXT INVALIDATE, OWED *BEFORE* =====
        // Reached only where no further frame will be recorded (shutdown's
        // drain, or the headless tests, which build no vehicle at all and so
        // never reach this branch either).
        if (m_services.chromeHud)
            (void)m_services.chromeHud->InvalidateUserTextureNow(m_preview->OffscreenOutput());
        m_preview.reset();
    }

    // =========================================================================
    // THE FORM
    // =========================================================================
    void MeshDocument::Draw(bool& requestClose)
    {
        // FIRST local, so it destructs LAST -- see EditGesture::ScopeGuard
        // (EditGesture.hpp:280-285) and SpriteDocument::Draw's identical
        // placement for why: it covers the early return below (a collapsed
        // window or a background tab, where no widget inside can report its
        // own deactivation).
        const EditGesture::ScopeGuard gestureGuard{ m_services.undo, m_gesture };

        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(420.0f, 640.0f), ImGuiCond_FirstUseEver);
        const ImGuiWindowFlags flags = Dirty() ? ImGuiWindowFlags_UnsavedDocument : 0;
        if (!ImGui::Begin(m_windowLabel.c_str(), &open, flags))
        {
            m_windowFocused = false;
            ImGui::End();
            requestClose = !open;
            return;
        }
        m_windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))
            Save();
        if (ImGui::Button("Save"))
            Save();
        ImGui::SameLine();
        ImGui::TextDisabled(Dirty() ? "(unsaved)" : "(saved)");
        ImGui::Separator();

        // ---- preview -------------------------------------------------------
        // ORDER IS LOAD-BEARING (fix-round Finding 1): m_validationReason is
        // checked FIRST, ahead of the texture id. A vehicle's offscreen
        // texture is non-null (PreviewTextureId() != 0) the INSTANT
        // CreateOffscreen succeeds -- long before any frame has rendered
        // into it -- so in ANY session that actually has a GPU device, the
        // texture id is non-zero from the moment this document opens,
        // regardless of whether the current data is valid. Checking the id
        // first made the reason branch dead code in exactly the real-editor
        // case it exists for: a hand-edited invalid file is the ONE
        // situation ValidateMeshAsset's refusal is reachable at all (every
        // WIDGET below is bounded at its floor), and that is precisely when
        // the user needs to read why. RenderPreview() (see its own comment)
        // still records a frame for the invalid case, so the texture holds a
        // clean cleared image rather than garbage -- but this document shows
        // the REASON over that image regardless, because a clean blank
        // picture answers "what" and not "why".
        ImGui::BeginChild("##meshpreview", ImVec2(0.0f, 220.0f), ImGuiChildFlags_Borders);
        const std::uint64_t texId = PreviewTextureId();
        if (m_validationReason)
        {
            // Same severity colour ProblemsPanel/Console use for
            // DiagSeverity::Error (InspectorView.cpp's dangling-reference
            // styling cites the same literal -- no named Theme constant
            // exists for it yet).
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(0.90f, 0.35f, 0.35f, 1.0f), "%s", m_validationReason->c_str());
            ImGui::PopTextWrapPos();
        }
        else if (texId != 0)
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float fit = (std::min)(avail.x, avail.y) / static_cast<float>(kPreviewSize);
            const float side = static_cast<float>(kPreviewSize) * (fit > 0.0f ? fit : 1.0f);
            ImGui::Image(static_cast<ImTextureID>(texId), ImVec2(side, side));
        }
        else if (!m_services.nriDevice || !m_services.hostConfig)
        {
            // The headless-test case, and any real session with no NRI
            // device at all -- EnsurePreviewContext() never even attempted
            // CreateOffscreen (fix-round Finding 3: this branch used to also
            // catch the two cases below, which DO have a device).
            ImGui::TextDisabled("(no preview -- no GPU device)");
        }
        else
        {
            // A device exists, but there is no vehicle: either
            // CreateOffscreen refused at construction, or a prior frame
            // failed and DestroyPreviewContext dropped it (RenderPreview's
            // FrameOutcome::Failed branch) -- both already logged (ARC_WARN/
            // ARC_ERROR) at the point they happened. Deliberately NOT
            // claiming "no GPU device" here, which fix-round Finding 3
            // caught as false in exactly the situation someone would be
            // debugging a real device.
            ImGui::TextDisabled("(no preview -- the preview vehicle is unavailable; see the log)");
        }
        ImGui::EndChild();
        ImGui::Separator();

        // Continuous-drag bracket (widget-layer Task 7 idiom) -- identical
        // shape to SpriteDocument::Draw's `bracket` lambda. Call it
        // IMMEDIATELY after each drag widget's field write.
        const auto bracket = [&](const char* label)
        {
            EditGesture::BeginOnActivate(m_services.undo, m_gesture,
                [&] { return std::string(label); },
                [&]
                {
                    return std::function<void()>(
                        [this, label = std::string(label), before = m_data]
                        { PushDataEdit(label, before); });
                });
            EditGesture::EndOnDeactivate(m_services.undo, m_gesture);
        };

        // Single-frame commit for non-drag edits (the source combo, the
        // material drag-drop/clear): there is no ActiveId to bracket around a
        // Selectable click or a drop, so this pushes directly instead of
        // through EditGesture -- the same "no gesture bracketing needed"
        // idiom ShaderEditorDocument::SetParamWithUndo documents for its own
        // texture-param drops.
        const auto commit = [&](const char* label, const Arcane::MeshAssetData& before)
        {
            if (before == m_data)
                return;
            m_dirty = true;
            RebuildPreviewMesh();
            PushDataEdit(label, before);
        };

        // ---- source ---------------------------------------------------------
        static constexpr Arcane::MeshSource kSources[] = {
            Arcane::MeshSource::Plane, Arcane::MeshSource::Cube, Arcane::MeshSource::UvSphere,
            Arcane::MeshSource::Cylinder, Arcane::MeshSource::Capsule,
        };
        if (ImGui::BeginCombo("Source", SourceLabel(m_data.source)))
        {
            for (Arcane::MeshSource s : kSources)
            {
                if (ImGui::Selectable(SourceLabel(s), m_data.source == s))
                {
                    const Arcane::MeshAssetData before = m_data;
                    m_data.source = s;
                    commit("Change Source", before);
                }
            }
            ImGui::EndCombo();
        }

        // ---- per-source topology --------------------------------------------
        // ONLY the fields THIS source's generator reads (MeshAsset.hpp's
        // table; MeshBuilder.hpp's per-field comments name the same set) --
        // the flat tagged struct's per-source meaning is a UI concern too, so
        // a Plane shows no rings/segments and a Cube shows no topology at
        // all. Every widget's floor is exactly ValidateMeshAsset's per-source
        // rule, so normal interaction can never drive this document into a
        // refused state -- the widget's max is a generous, unvalidated UI-only
        // ceiling against a mis-drag producing a million-triangle mesh.
        //
        // Two tiny local helpers carry the repeated shape (RangedDrag* takes
        // an `int`/`float*`, the field is `uint32_t`/`float`; write back, then
        // bracket): rings and segments each appear on two sources below, and
        // without this every one of the four would repeat the same five lines.
        bool changed = false;
        const auto dragUint = [&](const char* label, std::uint32_t& field,
                                  double lo, double hi) -> bool
        {
            int v = static_cast<int>(field);
            const bool ch = RangedDragInt(label, &v, Astra::Range(lo, hi, 1.0));
            field = static_cast<std::uint32_t>(v);
            bracket(label);
            return ch;
        };
        const auto dragFloatField = [&](const char* label, float& field, float speed,
                                        double lo, double hi) -> bool
        {
            const bool ch = RangedDragFloat(label, &field, speed, Astra::Range(lo, hi));
            bracket(label);
            return ch;
        };

        switch (m_data.source)
        {
            case Arcane::MeshSource::Plane:
                changed |= dragUint("Subdivisions", m_data.subdivisions, 1.0, 64.0);
                break;
            case Arcane::MeshSource::Cube:
                ImGui::TextDisabled("(no topology parameters)");
                break;
            case Arcane::MeshSource::UvSphere:
                changed |= dragUint("Rings", m_data.rings, 3.0, 128.0);
                changed |= dragUint("Segments", m_data.segments, 3.0, 128.0);
                break;
            case Arcane::MeshSource::Cylinder:
                // Deliberately NOT `rings` -- BuildCylinder never reads it
                // (MeshAssetData::rings' own comment: "UvSphere; Capsule").
                changed |= dragUint("Segments", m_data.segments, 3.0, 128.0);
                break;
            case Arcane::MeshSource::Capsule:
                // The cap-ring floor is 2, not 3 -- a two-step arc still
                // closes a hemisphere; UvSphere's floor is 3 because ITS
                // rings span pole-to-pole (a full sphere, twice the arc).
                changed |= dragUint("Rings", m_data.rings, 2.0, 64.0);
                changed |= dragUint("Segments", m_data.segments, 3.0, 128.0);
                changed |= dragFloatField("Length Ratio", m_data.capsuleLengthRatio, 0.02f, 1.0, 20.0);
                break;
        }
        if (changed)
        {
            m_dirty = true;
            RebuildPreviewMesh();
        }

        // ---- material --------------------------------------------------------
        ImGui::Separator();
        ImGui::TextUnformatted("Material");
        ImGui::SameLine();
        std::string display = "(none)";
        if (m_data.material.IsValid())
        {
            display = m_data.material.ToString();
            if (m_services.runtime)
                if (const Arcane::Project* project = m_services.runtime->CurrentProject())
                    if (const auto mount = project->Registry().Resolve(m_data.material))
                        display = *mount;
        }
        ImGui::TextDisabled("%s", display.c_str());

        // Drop target: accept a .arcmat asset dragged from the Asset Browser
        // -- the whole of this field's "picker", same drop-only shape
        // ShaderEditorDocument::DrawTextureParam uses for a texture param.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetDragType))
            {
                const auto* payload = static_cast<const AssetDragPayload*>(p->Data);
                if (payload->kind == AssetKind::Material)
                {
                    const Arcane::MeshAssetData before = m_data;
                    m_data.material = payload->guid;
                    commit("Assign Material", before);
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (m_data.material.IsValid())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("x##clearmaterial"))
            {
                const Arcane::MeshAssetData before = m_data;
                m_data.material = Arcane::Guid::Nil();
                commit("Clear Material", before);
            }
        }

        ImGui::End();
        requestClose = !open;
    }
}

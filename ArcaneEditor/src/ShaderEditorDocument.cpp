#include "ShaderEditorDocument.hpp"

#include "AssetBrowser.hpp"
#include "MaterialParamWidgets.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/ShaderConventions.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
        Arcane::MaterialSurface SurfaceOf(int surface)
        {
            return surface == 1 ? Arcane::MaterialSurface::Sprite
                                : Arcane::MaterialSurface::Fullscreen;
        }

        // One param edit as an undo step. The live edit already happened (the
        // ICommand contract); Undo restores the BEFORE override state (value or
        // no-override), Redo re-applies the AFTER. Doc-identity (review M3): the
        // command holds the DOCUMENT weakly and forwards by name hash to its
        // CURRENT instance -- a recompile swaps the instance but the step stays
        // live; closing the document expires the anchor (the step goes inert).
        class ParamEditCommand final : public Arcane::ICommand
        {
        public:
            ParamEditCommand(std::weak_ptr<ShaderEditorDocument*> anchor,
                             std::uint32_t nameHash, std::string label,
                             bool hadBefore, Arcane::MatParamValue before,
                             bool hasAfter, Arcane::MatParamValue after)
                : m_anchor(std::move(anchor)), m_nameHash(nameHash),
                  m_label(std::move(label)), m_hadBefore(hadBefore),
                  m_before(before), m_hasAfter(hasAfter), m_after(after)
            {
            }

            void Undo() override { Apply(m_hadBefore, m_before); }
            void Redo() override { Apply(m_hasAfter, m_after); }
            const char* Label() const override { return m_label.c_str(); }

        private:
            void Apply(bool hasValue, const Arcane::MatParamValue& value)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplyParamEdit(m_nameHash, hasValue, value);
            }

            std::weak_ptr<ShaderEditorDocument*> m_anchor;
            std::uint32_t m_nameHash;
            std::string   m_label;
            bool          m_hadBefore;
            Arcane::MatParamValue m_before;
            bool          m_hasAfter;
            Arcane::MatParamValue m_after;
        };

        std::uint64_t StageKey(const Arcane::Guid& id, bool vertex)
        {
            // Coalesce per (document, stage): a newer submit of the SAME stage
            // supersedes; the two stages never cancel each other.
            return (id.hi ^ (id.lo * 1099511628211ull)) ^ (vertex ? 0x1u : 0x2u);
        }
    }

    // InputTextMultiline over std::string (the imgui_stdlib resize pattern) +
    // pending cursor jump: the CallbackAlways pass moves the cursor to the
    // requested line once the widget is active -- stb_textedit then scrolls the
    // cursor into view, which is exactly click-to-jump. UserData is the
    // DOCUMENT (per-doc state, review m2 -- a shared static could deliver one
    // document's jump to another on a same-frame focus race); this forwarder is
    // the declared friend, so it may touch the members directly.
    struct SnippetCallbackForwarder
    {
        static int Callback(ImGuiInputTextCallbackData* data)
        {
            auto* doc = static_cast<ShaderEditorDocument*>(data->UserData);
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                doc->m_snippet.resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = doc->m_snippet.data();
            }
            else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways &&
                     doc->m_callbackJumpLine > 0)
            {
                int line = 1;
                int pos = 0;
                while (line < doc->m_callbackJumpLine && pos < data->BufTextLen)
                {
                    if (data->Buf[pos] == '\n')
                        ++line;
                    ++pos;
                }
                data->CursorPos = pos;
                data->SelectionStart = data->SelectionEnd = pos;
                doc->m_callbackJumpLine = 0;
            }
            return 0;
        }
    };

    ShaderEditorDocument::ShaderEditorDocument(DocServices services,
                                               std::filesystem::path path,
                                               Arcane::MaterialAssetData data)
        : m_services(services), m_path(std::move(path)), m_data(std::move(data))
    {
        m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name;
        m_windowLabel = m_title + (m_data.IsInstance() ? " (Instance)###matdoc_"
                                                       : " (Material)###matdoc_") +
                        m_data.id.ToString();
        m_snippet = m_data.snippet;
        m_anchor = std::make_shared<ShaderEditorDocument*>(this);

        // Device-less services (headless tests) skip the preview cleanly; the
        // document still parses, compiles, and saves.
        if (m_services.device && m_services.shaders)
        {
            m_preview = Arcane::OffscreenCanvas::Create(m_services.device,
                                                        *m_services.shaders, 512, 512);
            m_pass = Arcane::FullscreenMaterialPass::Create(m_services.device);
            if (!m_preview || !m_pass)
                ARC_WARN("ShaderEditorDocument '{}': preview resources failed", m_title);
        }

        if (!IsInstance() || ResolveParentChain())
        {
            // Preview surface follows the asset's kind (instances inherit the
            // chain BASE's kind -- the base owns the snippet and the surface).
            const std::string& kind =
                IsInstance() && !m_parentChain.empty() ? m_parentChain.back().kind
                                                       : m_data.kind;
            m_surface = Arcane::MaterialSurfaceForKind(kind) ==
                                Arcane::MaterialSurface::Sprite ? 1 : 0;
            Rebuild();   // parse + first compile; the pass binds when results drain
        }
    }

    bool ShaderEditorDocument::ResolveParentChain()
    {
        m_parentChain.clear();
        const Arcane::Project* project =
            m_services.runtime ? m_services.runtime->CurrentProject() : nullptr;
        if (!project)
        {
            m_parseErrors = { "instance materials need an open project (parent lookup)" };
            return false;
        }

        Arcane::Guid cursor = m_data.parent;
        std::vector<Arcane::Guid> visited{ m_data.id };
        while (cursor.IsValid())
        {
            for (const Arcane::Guid& seen : visited)
            {
                if (seen == cursor)
                {
                    m_parseErrors = { "parent chain contains a cycle" };
                    return false;
                }
            }
            visited.push_back(cursor);

            const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(cursor));
            if (!path)
            {
                m_parseErrors = { "parent material " + cursor.ToString() +
                                  " is not in the asset registry" };
                return false;
            }
            auto parent = Arcane::LoadMaterialAsset(*path);
            if (!parent)
            {
                m_parseErrors = { "parent material failed to load: " +
                                  path->generic_string() };
                return false;
            }
            cursor = parent->parent;
            m_parentChain.push_back(std::move(*parent));
        }

        if (m_parentChain.empty() || m_parentChain.back().IsInstance())
        {
            m_parseErrors = { "parent chain never reaches a base material" };
            return false;
        }
        return true;
    }

    const std::string& ShaderEditorDocument::SnippetSource() const
    {
        return IsInstance() && !m_parentChain.empty() ? m_parentChain.back().snippet
                                                      : m_snippet;
    }

    void ShaderEditorDocument::Rebuild()
    {
        if (!m_services.compiler || !m_services.sources)
            return;

        const char* templateFile = Arcane::MaterialTemplateFile(SurfaceOf(m_surface));
        const auto templateText = m_services.sources->Get(templateFile);
        if (!templateText)
        {
            m_parseErrors = { std::string("template not found: ") + templateFile };
            return;
        }

        Arcane::MaterialBuildResult build =
            Arcane::BuildMaterialShaderSource(*templateText, SnippetSource(), m_title,
                                              SurfaceOf(m_surface));
        m_parseErrors = std::move(build.errors);
        m_pendingTemplate = std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
        m_metas = std::move(build.metas);

        Arcane::ShaderCompileRequest req;
        req.debugName = m_title + ".hlsl";
        req.sourceUtf8 = build.hlsl;
        req.entry = Arcane::kPsEntry;
        req.profile = Arcane::kPsProfile;
        req.coalesceKey = StageKey(m_data.id, /*vertex=*/false);
        m_psJob = m_services.compiler->Submit(req, Now());
        req.entry = Arcane::kVsEntry;
        req.profile = Arcane::kVsProfile;
        req.coalesceKey = StageKey(m_data.id, /*vertex=*/true);
        m_vsJob = m_services.compiler->Submit(std::move(req), Now());
        m_vsBytes.clear();
        m_psBytes.clear();
    }

    bool ShaderEditorDocument::ConsumeResult(const Arcane::ShaderCompileResult& result)
    {
        const bool isVs = result.jobId == m_vsJob && m_vsJob != 0;
        const bool isPs = result.jobId == m_psJob && m_psJob != 0;
        if (!isVs && !isPs)
            return false;

        const auto& target = m_services.backend == Arcane::GraphicsBackend::Vulkan
                                 ? result.spirv : result.dxil;
        if (isPs)
            m_diags = target.diags;   // the ps stage carries the designer-relevant diags
        if (target.succeeded)
        {
            (isVs ? m_vsBytes : m_psBytes) = target.bytecode;
            BindIfComplete();
        }
        return true;
    }

    void ShaderEditorDocument::BindIfComplete()
    {
        const bool sprite = m_surface == 1;
        if (m_vsBytes.empty() || m_psBytes.empty() || !m_pendingTemplate ||
            (sprite ? !m_preview : !m_pass))
            return;

        nvrhi::ShaderHandle vs = m_services.device->createShader(
            nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex)
                .setEntryName(Arcane::kVsEntry).setDebugName((m_title + "_vs").c_str()),
            m_vsBytes.data(), m_vsBytes.size());
        nvrhi::ShaderHandle ps = m_services.device->createShader(
            nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel)
                .setEntryName(Arcane::kPsEntry).setDebugName((m_title + "_ps").c_str()),
            m_psBytes.data(), m_psBytes.size());
        m_vsBytes.clear();
        m_psBytes.clear();
        if (!vs || !ps)
            return;
        // Fullscreen surface binds the pass here (a failure keeps last-good
        // AND the previous instance). Sprite surface registers with the
        // preview canvas's batcher AFTER the fresh instance exists below.
        if (!sprite && !m_pass->SetMaterial(m_pendingTemplate, vs, ps))
            return;

        // Promote pending -> bound. Instance mode first layers the parent chain
        // (base's saved values innermost) so resolution walks child override ->
        // parents -> //@param default. Then migrate this document's own
        // overrides by name hash (first bind applies the asset's saved values;
        // params a snippet edit dropped are rejected by Set and retired).
        auto fresh = std::make_shared<Arcane::MaterialInstance>(m_pendingTemplate);
        for (auto it = m_parentChain.rbegin(); it != m_parentChain.rend(); ++it)
        {
            Arcane::ApplyMaterialParams(*it, *fresh);
            fresh = std::make_shared<Arcane::MaterialInstance>(
                std::shared_ptr<const Arcane::MaterialInstance>(fresh));
        }
        if (m_instance)
        {
            for (const auto& [hash, value] : m_instance->Overrides())
                fresh->Set(hash, value);
        }
        else
        {
            Arcane::ApplyMaterialParams(m_data, *fresh);
        }
        // Re-baseline the dirty verdict against the FRESH instance's serial
        // space; unsaved param edits ride across the swap as the base flag.
        const bool paramsWereDirty = ParamsDirty();
        m_instance = std::move(fresh);
        m_paramsBaseDirty = paramsWereDirty;
        m_savedParamSerial = m_instance->EffectiveSerial();
        m_boundTemplate = m_pendingTemplate;
        m_boundMetas = m_metas;

        if (sprite)
        {
            m_previewVs = vs;
            m_previewPs = ps;
            RefreshSpritePreviewBinding();
        }
    }

    void ShaderEditorDocument::RefreshSpritePreviewBinding()
    {
        // (Re)register the preview material on the canvas's OWN batcher: the
        // shared instance pointer keeps live param edits flowing (PackCB reads
        // it at End()); texture params resolve fresh HERE, so a pick lands by
        // calling this again -- no recompile.
        if (!m_preview || !m_previewVs || !m_previewPs || !m_instance || !m_boundTemplate)
            return;
        Arcane::Material2DDesc desc;
        desc.vs = m_previewVs;
        desc.ps = m_previewPs;
        desc.templ = m_boundTemplate;
        desc.instance = m_instance;
        const std::vector<Arcane::Guid> texGuids = m_instance->ResolveTextures();
        desc.paramTextures.resize(texGuids.size());
        if (m_services.runtime)
            for (std::size_t i = 0; i < texGuids.size(); ++i)
                if (texGuids[i].IsValid())
                    desc.paramTextures[i] = m_services.runtime->AssetsFacade().GetTexture(
                        Arcane::AssetId::FromGuid(texGuids[i]));

        Arcane::Batcher2D& batcher = m_preview->Batch();
        if (m_previewSpriteMaterial != Arcane::Batcher2D::kInvalidMaterialId)
        {
            if (!batcher.UpdateMaterial(m_previewSpriteMaterial, std::move(desc)))
                ARC_WARN("ShaderEditorDocument '{}': sprite preview update failed", m_title);
            return;
        }
        m_previewSpriteMaterial = batcher.RegisterMaterial(std::move(desc));
    }

    bool ShaderEditorDocument::HasErrors() const
    {
        if (!m_parseErrors.empty())
            return true;
        for (const Arcane::ShaderDiag& d : m_diags)
            if (d.severity == Arcane::ShaderDiagSeverity::Error)
                return true;
        return false;
    }

    bool ShaderEditorDocument::Save()
    {
        m_data.snippet = m_snippet;
        // Review M1: before the first successful bind there is no instance to
        // harvest values from -- keep the loaded params instead of wiping them.
        if (m_instance && m_boundTemplate)
        {
            m_data.params.clear();
            for (const auto& [hash, value] : m_instance->Overrides())
                if (const Arcane::ParamDecl* d = m_boundTemplate->Find(hash))
                    m_data.params.emplace_back(d->name, value);
        }
        if (!Arcane::SaveMaterialAsset(m_path, m_data))
            return false;
        // Idempotent: heals assets opened from paths the registry has never seen
        // (created outside the editor flows, or before this session).
        if (m_services.runtime)
            m_services.runtime->RegisterCreatedAsset(m_path);
        m_dirty = false;
        m_paramsBaseDirty = false;
        m_savedParamSerial = m_instance ? m_instance->EffectiveSerial() : 0;
        if (m_services.onAssetSaved)
            m_services.onAssetSaved(m_data.id);
        return true;
    }

    bool ShaderEditorDocument::ParamsDirty() const
    {
        return m_paramsBaseDirty ||
               (m_instance && m_instance->EffectiveSerial() != m_savedParamSerial);
    }

    void ShaderEditorDocument::ApplyParamEdit(std::uint32_t nameHash, bool hasValue,
                                              const Arcane::MatParamValue& value)
    {
        if (!m_instance)
            return;
        // A param the current snippet dropped is rejected by Set -- the step
        // no-ops rather than corrupting an unrelated instance.
        if (hasValue)
            m_instance->Set(nameHash, value);
        else
            m_instance->ClearOverride(nameHash);
        // Undo/redo of a TEXTURE param must re-bind the sprite preview too.
        if (m_surface == 1 && m_boundTemplate)
            if (const Arcane::ParamDecl* d = m_boundTemplate->Find(nameHash);
                d && d->type == Arcane::MatParamType::Texture)
                RefreshSpritePreviewBinding();
    }

    bool ShaderEditorDocument::PreviewReady() const
    {
        return m_surface == 1
                   ? m_previewSpriteMaterial != Arcane::Batcher2D::kInvalidMaterialId
                   : m_pass && m_pass->Ready();
    }

    void ShaderEditorDocument::Tick(double dt)
    {
        m_animTime += dt;
        if (!PreviewReady() || !m_instance || !m_preview)
            return;

        // Texture params must be GPU-resident BEFORE the canvas list records:
        // the Assets facade uploads through its own transient command list, and
        // an executeCommandList issued while ANOTHER list is open loses the
        // upload -- the texture stays empty (the [gpu] texparam test pins this
        // contract). Memoized, so steady-state cost is a hash lookup per param.
        if (m_services.runtime)
            for (const Arcane::Guid& g : m_instance->ResolveTextures())
                if (g.IsValid())
                    (void)m_services.runtime->AssetsFacade().GetTexture(
                        Arcane::AssetId::FromGuid(g));

        Arcane::GlobalParams globals;
        globals.time = static_cast<float>(m_animTime);
        globals.deltaTime = static_cast<float>(dt);
        globals.viewportWidth = static_cast<float>(m_preview->Width());
        globals.viewportHeight = static_cast<float>(m_preview->Height());

        if (m_surface == 1)
        {
            // Sprite surface: the material on a centered quad over a
            // checkerboard, through the canvas's own Batcher2D -- the SAME
            // pipeline family scene sprites use.
            m_preview->Draw(
                [&](Arcane::Batcher2D& b)
                {
                    b.SetGlobals(globals);
                    const float w = (float)m_preview->Width();
                    const float h = (float)m_preview->Height();
                    const float cell = 32.0f;
                    const glm::vec4 light(0.16f, 0.16f, 0.19f, 1.0f);
                    for (int y = 0; y * cell < h; ++y)
                        for (int x = 0; x * cell < w; ++x)
                            if ((x + y) & 1)
                                b.Rect(glm::vec2(x * cell, y * cell),
                                       glm::vec2(cell, cell), light);
                    const float s = 0.8f * (std::min)(w, h);
                    b.QuadMaterial(m_previewSpriteMaterial,
                                   glm::vec2((w - s) * 0.5f, (h - s) * 0.5f),
                                   glm::vec2(s, s), nullptr,
                                   glm::vec2(0.0f), glm::vec2(1.0f),
                                   glm::vec4(1.0f));
                },
                glm::vec4(0.09f, 0.09f, 0.11f, 1.0f));
            return;
        }

        m_preview->DrawPass(
            [&](nvrhi::ICommandList* cl, nvrhi::IFramebuffer* fb)
            {
                m_pass->Render(cl, fb, *m_instance, globals,
                               m_services.runtime ? &m_services.runtime->AssetsFacade()
                                                  : nullptr);
            },
            glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    }

    void ShaderEditorDocument::Draw(bool& requestClose)
    {
        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
        ImGuiWindowFlags flags = Dirty() ? ImGuiWindowFlags_UnsavedDocument : 0;
        if (!ImGui::Begin(m_windowLabel.c_str(), &open, flags))
        {
            ImGui::End();
            requestClose = !open;
            return;
        }

        DrawToolbar();

        const float contentH = ImGui::GetContentRegionAvail().y;
        const float leftW = ImGui::GetContentRegionAvail().x * 0.55f;

        ImGui::BeginChild("##left", ImVec2(leftW, contentH));
        if (IsInstance())
        {
            // Instance mode: params-only -- the source belongs to the base.
            const float errorsH = 110.0f;
            ImGui::BeginChild("##instparams",
                              ImVec2(0, ImGui::GetContentRegionAvail().y - errorsH));
            DrawParamsPanel();
            ImGui::EndChild();
            DrawErrorsPanel();
        }
        else
        {
            const float errorsH = 140.0f;
            DrawSnippetEditor(ImGui::GetContentRegionAvail().y - errorsH);
            DrawErrorsPanel();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##right", ImVec2(0, contentH));
        {
            if (IsInstance())
            {
                DrawPreviewPanel(ImGui::GetContentRegionAvail().y);
            }
            else
            {
                DrawPreviewPanel(ImGui::GetContentRegionAvail().y * 0.55f);
                DrawParamsPanel();
            }
        }
        ImGui::EndChild();

        ImGui::End();
        requestClose = !open;
    }

    void ShaderEditorDocument::DrawToolbar()
    {
        // Error-guarded save (UE's pre-apply guard shape): saving broken WIP is
        // allowed, but only through an explicit confirm.
        if (ImGui::Button("Save"))
        {
            if (HasErrors())
                m_confirmSaveWithErrors = true;
            else
                Save();
        }
        if (!IsInstance())
        {
            // Structural (snippet) controls are base-material-only; an instance
            // recompiles nothing -- it only re-values the parent's shader.
            ImGui::SameLine();
            ImGui::Checkbox("Live", &m_live);
            ImGui::SameLine();
            if (ImGui::Button("Compile"))
                Rebuild();
        }
        else if (!m_parentChain.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("instance of '%s'", m_parentChain.back().name.c_str());
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        // Preview-surface selector (Slice 8). On a base material this is a
        // STRUCTURAL edit: it re-kinds the asset (the surface is what the
        // material is FOR) and recompiles under the other template. Instances
        // preview under the switched surface without touching the base.
        int surface = m_surface;
        if (ImGui::Combo("##surface", &surface, "Fullscreen\0Sprite\0") &&
            surface != m_surface)
        {
            m_surface = surface;
            m_previewSpriteMaterial = Arcane::Batcher2D::kInvalidMaterialId;
            m_previewVs = nullptr;
            m_previewPs = nullptr;
            if (!IsInstance())
            {
                m_data.kind = m_surface == 1 ? "sprite" : "fullscreen";
                m_dirty = true;
            }
            Rebuild();
        }
        ImGui::SameLine();
        if (HasErrors())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "errors");
        else if (PreviewReady())
            ImGui::TextDisabled("ok");
        else
            ImGui::TextDisabled("compiling...");
        ImGui::Separator();

        if (m_confirmSaveWithErrors)
        {
            ImGui::OpenPopup("Save With Errors?##matdoc");
            m_confirmSaveWithErrors = false;
        }
        if (ImGui::BeginPopupModal("Save With Errors?##matdoc", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("This material has compile errors. Save anyway?");
            ImGui::Separator();
            if (ImGui::Button("Save Anyway"))
            {
                Save();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void ShaderEditorDocument::DrawSnippetEditor(float height)
    {
        if (m_jumpToLine > 0)
        {
            m_callbackJumpLine = m_jumpToLine;
            m_jumpToLine = 0;
            m_focusSnippet = true;
        }

        if (m_focusSnippet)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusSnippet = false;
        }
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput |
                                          ImGuiInputTextFlags_CallbackResize |
                                          ImGuiInputTextFlags_CallbackAlways;
        // +1 capacity: ImGui writes the terminator into the buffer it is given.
        if (ImGui::InputTextMultiline("##snippet", m_snippet.data(), m_snippet.capacity() + 1,
                                      ImVec2(-1.0f, height), flags,
                                      &SnippetCallbackForwarder::Callback, this))
        {
            m_dirty = true;
            if (m_live)
                Rebuild();
        }
    }

    void ShaderEditorDocument::DrawErrorsPanel()
    {
        ImGui::BeginChild("##errors", ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (const std::string& e : m_parseErrors)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", e.c_str());
        int i = 0;
        for (const Arcane::ShaderDiag& d : m_diags)
        {
            const bool isError = d.severity == Arcane::ShaderDiagSeverity::Error;
            const ImVec4 color = isError ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f)
                                         : ImVec4(0.9f, 0.8f, 0.4f, 1.0f);
            // Review M2: the message must sit BEFORE the "##" -- ImGui renders
            // nothing past it (everything after is only the widget ID).
            char head[48];
            std::snprintf(head, sizeof(head), "%s(%d): ",
                          isError ? "error" : "warning", d.line);
            const std::string label = head + d.message + "##diag" + std::to_string(i++);
            // Selectable row -> jump the text cursor to the diagnostic's line.
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if (ImGui::Selectable(label.c_str(), false))
                m_jumpToLine = d.line > 0 ? d.line : 1;
            ImGui::PopStyleColor();
        }
        if (m_parseErrors.empty() && m_diags.empty())
            ImGui::TextDisabled("no diagnostics");
        ImGui::EndChild();
    }

    void ShaderEditorDocument::DrawPreviewPanel(float height)
    {
        ImGui::BeginChild("##preview", ImVec2(0, height), ImGuiChildFlags_Borders);
        if (m_preview && PreviewReady())
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float texW = static_cast<float>(m_preview->Width());
            const float texH = static_cast<float>(m_preview->Height());
            const float scale = (std::min)(avail.x > 0 ? avail.x / texW : 1.0f,
                                           avail.y > 0 ? avail.y / texH : 1.0f);
            const float s = scale > 0.0f ? scale : 1.0f;
            ImGui::Image(static_cast<ImTextureID>(m_preview->TextureId()),
                         ImVec2(texW * s, texH * s));
        }
        else
        {
            ImGui::TextDisabled("compiling...");
        }
        ImGui::EndChild();
    }

    // One texture param row: current binding + [pick] popup over the project's
    // texture assets + a browser-drag drop target. All three routes land in
    // SetParamWithUndo (single-step undo, no gesture bracketing needed).
    void ShaderEditorDocument::DrawTextureParam(const Arcane::ParamDecl& d,
                                                const Arcane::MatParamValue& current)
    {
        const Arcane::Project* project =
            m_services.runtime ? m_services.runtime->CurrentProject() : nullptr;

        std::string display = "(none)";
        if (current.tex.IsValid())
        {
            display = current.tex.ToString();
            if (project)
                if (const auto mount = project->Registry().Resolve(current.tex))
                    display = *mount;
        }
        ImGui::TextUnformatted(d.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", display.c_str());

        // Drop target: accept a texture asset dragged from the browser.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetDragType))
            {
                const auto* payload = static_cast<const AssetDragPayload*>(p->Data);
                if (payload->kind == AssetKind::Texture)
                    SetParamWithUndo(d, Arcane::MatParamValue::MakeTexture(payload->guid));
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        const std::string pickId = "pick##" + d.name;
        const std::string popupId = "##texpick_" + d.name;
        if (ImGui::SmallButton(pickId.c_str()))
            ImGui::OpenPopup(popupId.c_str());
        if (ImGui::BeginPopup(popupId.c_str()))
        {
            if (!project)
            {
                ImGui::TextDisabled("no project open");
            }
            else
            {
                if (ImGui::Selectable("(none)"))
                    SetParamWithUndo(d, Arcane::MatParamValue::MakeTexture(Arcane::Guid::Nil()));
                for (const AssetEntry& e : BuildAssetEntries(project->Registry()))
                {
                    if (e.kind != AssetKind::Texture)
                        continue;
                    const std::string label = e.name + "##" + e.mountPath;
                    if (ImGui::Selectable(label.c_str(), e.guid == current.tex))
                        SetParamWithUndo(d, Arcane::MatParamValue::MakeTexture(e.guid));
                }
            }
            ImGui::EndPopup();
        }
    }

    void ShaderEditorDocument::SetParamWithUndo(const Arcane::ParamDecl& d,
                                                const Arcane::MatParamValue& value)
    {
        if (!m_instance)
            return;
        const bool hadBefore = m_instance->HasOverride(d.nameHash);
        Arcane::MatParamValue before;
        if (hadBefore)
            m_instance->GetParam(d.nameHash, before);
        if (!m_instance->Set(d.nameHash, value))
            return;
        if (m_services.undo)
            m_services.undo->Push(std::make_unique<ParamEditCommand>(
                m_anchor, d.nameHash, "Edit " + d.name,
                hadBefore, before, /*hasAfter=*/true, value));
        // Sprite surface binds texture params at registration -- a pick needs
        // a binding refresh (numeric params flow live through the CB).
        if (m_surface == 1 && d.type == Arcane::MatParamType::Texture)
            RefreshSpritePreviewBinding();
    }

    void ShaderEditorDocument::DrawParamsPanel()
    {
        ImGui::BeginChild("##params", ImVec2(0, 0), ImGuiChildFlags_Borders);
        if (!m_instance || !m_boundTemplate)
        {
            ImGui::TextDisabled("params appear after the first successful compile");
            ImGui::EndChild();
            return;
        }

        // Before-state captured on widget activation (m_gesture*); the undo step
        // is pushed on release-after-edit (one drag = one step).
        if (IsInstance())
            ImGui::Checkbox("Only overridden", &m_showOnlyOverridden);

        const auto& params = m_boundTemplate->Params();
        for (std::size_t i = 0; i < params.size(); ++i)
        {
            const Arcane::ParamDecl& d = params[i];
            if (IsInstance() && m_showOnlyOverridden && !m_instance->HasOverride(d.nameHash))
                continue;

            // Instance mode: the per-param override checkbox (UE's model) --
            // checking materializes an override at the currently-resolved value,
            // unchecking clears it (parent/default shows through). Undoable.
            if (IsInstance())
            {
                bool ov = m_instance->HasOverride(d.nameHash);
                const std::string ovId = "##ov_" + d.name;
                if (ImGui::Checkbox(ovId.c_str(), &ov))
                {
                    if (ov)
                    {
                        Arcane::MatParamValue resolved;
                        if (m_instance->GetParam(d.nameHash, resolved))
                            SetParamWithUndo(d, resolved);
                    }
                    else
                    {
                        Arcane::MatParamValue before;
                        m_instance->GetParam(d.nameHash, before);
                        m_instance->ClearOverride(d.nameHash);
                        if (m_services.undo)
                            m_services.undo->Push(std::make_unique<ParamEditCommand>(
                                m_anchor, d.nameHash, "Reset " + d.name,
                                /*hadBefore=*/true, before, /*hasAfter=*/false,
                                Arcane::MatParamValue{}));
                    }
                }
                ImGui::SameLine();
            }
            const Arcane::ParamMeta meta = i < m_boundMetas.size() ? m_boundMetas[i]
                                                                   : Arcane::ParamMeta{};
            Arcane::MatParamValue value;
            if (!m_instance->GetParam(d.nameHash, value))
                continue;

            bool edited = false;
            switch (WidgetFor(d.type))
            {
                case ParamWidget::SliderFloat:
                    edited = ImGui::SliderFloat(d.name.c_str(), &value.f[0],
                                                meta.sliderMin, meta.sliderMax);
                    break;
                case ParamWidget::DragFloat2:
                    edited = ImGui::DragFloat2(d.name.c_str(), value.f, 0.01f);
                    break;
                case ParamWidget::DragFloat4:
                    edited = ImGui::DragFloat4(d.name.c_str(), value.f, 0.01f);
                    break;
                case ParamWidget::ColorEdit:
                    edited = ImGui::ColorEdit4(d.name.c_str(), value.f);
                    break;
                case ParamWidget::TexturePicker:
                    DrawTextureParam(d, value);
                    break;
            }

            if (ImGui::IsItemActivated())
            {
                m_gestureHadBefore = m_instance->HasOverride(d.nameHash);
                m_gestureBefore = Arcane::MatParamValue{};
                if (m_gestureHadBefore)
                    m_instance->GetParam(d.nameHash, m_gestureBefore);
            }

            if (edited)
            {
                // LIVE: straight into the instance -> next Tick packs the CB.
                // No recompile -- the whole point of the declared-param model.
                m_instance->Set(d.nameHash, value);
            }

            if (ImGui::IsItemDeactivatedAfterEdit() && m_services.undo)
            {
                Arcane::MatParamValue after;
                if (m_instance->GetParam(d.nameHash, after))
                {
                    m_services.undo->Push(std::make_unique<ParamEditCommand>(
                        m_anchor, d.nameHash, "Edit " + d.name,
                        m_gestureHadBefore, m_gestureBefore, /*hasAfter=*/true, after));
                }
            }

            // Reset-to-default: clears the override so the //@param default (or
            // a parent's value, Slice 7) shows through. Undoable.
            if (m_instance->HasOverride(d.nameHash))
            {
                ImGui::SameLine();
                std::string resetId = "x##reset_" + d.name;
                if (ImGui::SmallButton(resetId.c_str()))
                {
                    Arcane::MatParamValue before;
                    m_instance->GetParam(d.nameHash, before);
                    m_instance->ClearOverride(d.nameHash);
                    if (m_services.undo)
                        m_services.undo->Push(std::make_unique<ParamEditCommand>(
                            m_anchor, d.nameHash, "Reset " + d.name,
                            /*hadBefore=*/true, before, /*hasAfter=*/false,
                            Arcane::MatParamValue{}));
                }
            }
        }
        ImGui::EndChild();
    }
}

#include "ShaderEditorDocument.hpp"

#include "MaterialParamWidgets.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Material/MaterialSource.hpp>
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
        constexpr const char* kTemplateFile = "materials/fullscreen_material.hlsl";

        // One param edit as an undo step. The live edit already happened (the
        // ICommand contract); Undo restores the BEFORE override state (value or
        // no-override), Redo re-applies the AFTER. Holds the instance weakly:
        // closing the document orphans the step harmlessly (no dangling).
        class ParamEditCommand final : public Arcane::ICommand
        {
        public:
            ParamEditCommand(std::weak_ptr<Arcane::MaterialInstance> instance,
                             std::uint32_t nameHash, std::string label,
                             bool hadBefore, Arcane::MatParamValue before,
                             bool hasAfter, Arcane::MatParamValue after)
                : m_instance(std::move(instance)), m_nameHash(nameHash),
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
                auto inst = m_instance.lock();
                if (!inst)
                    return;   // document closed -- the step is inert
                if (hasValue)
                    inst->Set(m_nameHash, value);
                else
                    inst->ClearOverride(m_nameHash);
            }

            std::weak_ptr<Arcane::MaterialInstance> m_instance;
            std::uint32_t m_nameHash;
            std::string   m_label;
            bool          m_hadBefore;
            Arcane::MatParamValue m_before;
            bool          m_hasAfter;
            Arcane::MatParamValue m_after;
        };

        // InputTextMultiline over std::string (the imgui_stdlib resize pattern)
        // + pending cursor jump: the CallbackAlways pass moves the cursor to the
        // requested line once the widget is active -- stb_textedit then scrolls
        // the cursor into view, which is exactly click-to-jump.
        struct SnippetCallbackCtx
        {
            std::string* text = nullptr;
            int jumpToLine = 0;   // 1-based; consumed on first callback pass
        };

        int SnippetCallback(ImGuiInputTextCallbackData* data)
        {
            auto* ctx = static_cast<SnippetCallbackCtx*>(data->UserData);
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                ctx->text->resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = ctx->text->data();
            }
            else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways && ctx->jumpToLine > 0)
            {
                int line = 1;
                int pos = 0;
                while (line < ctx->jumpToLine && pos < data->BufTextLen)
                {
                    if (data->Buf[pos] == '\n')
                        ++line;
                    ++pos;
                }
                data->CursorPos = pos;
                data->SelectionStart = data->SelectionEnd = pos;
                ctx->jumpToLine = 0;
            }
            return 0;
        }

        std::uint64_t StageKey(const Arcane::Guid& id, bool vertex)
        {
            // Coalesce per (document, stage): a newer submit of the SAME stage
            // supersedes; the two stages never cancel each other.
            return (id.hi ^ (id.lo * 1099511628211ull)) ^ (vertex ? 0x1u : 0x2u);
        }
    }

    ShaderEditorDocument::ShaderEditorDocument(DocServices services,
                                               std::filesystem::path path,
                                               Arcane::MaterialAssetData data)
        : m_services(services), m_path(std::move(path)), m_data(std::move(data))
    {
        m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name;
        m_windowLabel = m_title + " (Material)###matdoc_" + m_data.id.ToString();
        m_snippet = m_data.snippet;

        m_preview = Arcane::OffscreenCanvas::Create(m_services.device,
                                                    *m_services.shaders, 512, 512);
        m_pass = Arcane::FullscreenMaterialPass::Create(m_services.device);
        if (!m_preview || !m_pass)
            ARC_WARN("ShaderEditorDocument '{}': preview resources failed", m_title);

        Rebuild();   // parse + first compile; the pass binds when results drain
    }

    void ShaderEditorDocument::Rebuild()
    {
        if (!m_services.compiler || !m_services.sources)
            return;

        const auto templateText = m_services.sources->Get(kTemplateFile);
        if (!templateText)
        {
            m_parseErrors = { std::string("template not found: ") + kTemplateFile };
            return;
        }

        Arcane::MaterialBuildResult build =
            Arcane::BuildMaterialShaderSource(*templateText, m_snippet, m_title);
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
        if (m_vsBytes.empty() || m_psBytes.empty() || !m_pass || !m_pendingTemplate)
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
        if (!vs || !ps || !m_pass->SetMaterial(m_pendingTemplate, vs, ps))
            return;

        // Promote pending -> bound and migrate the instance: overrides carry
        // over by name hash (params the new snippet dropped are rejected by
        // Set and silently retired). First bind applies the asset's saved values.
        auto fresh = std::make_shared<Arcane::MaterialInstance>(m_pendingTemplate);
        if (m_instance)
        {
            for (const auto& [hash, value] : m_instance->Overrides())
                fresh->Set(hash, value);
        }
        else
        {
            Arcane::ApplyMaterialParams(m_data, *fresh);
        }
        m_instance = std::move(fresh);
        m_boundTemplate = m_pendingTemplate;
        m_boundMetas = m_metas;
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
        m_data.params.clear();
        if (m_instance && m_boundTemplate)
        {
            for (const auto& [hash, value] : m_instance->Overrides())
                if (const Arcane::ParamDecl* d = m_boundTemplate->Find(hash))
                    m_data.params.emplace_back(d->name, value);
        }
        if (!Arcane::SaveMaterialAsset(m_path, m_data))
            return false;
        m_dirty = false;
        return true;
    }

    void ShaderEditorDocument::Tick(double dt)
    {
        m_animTime += dt;
        if (!m_pass || !m_pass->Ready() || !m_instance || !m_preview)
            return;

        Arcane::GlobalParams globals;
        globals.time = static_cast<float>(m_animTime);
        globals.deltaTime = static_cast<float>(dt);
        globals.viewportWidth = static_cast<float>(m_preview->Width());
        globals.viewportHeight = static_cast<float>(m_preview->Height());
        m_preview->DrawPass(
            [&](nvrhi::ICommandList* cl, nvrhi::IFramebuffer* fb)
            {
                m_pass->Render(cl, fb, *m_instance, globals, m_services.assets);
            },
            glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    }

    void ShaderEditorDocument::Draw(bool& requestClose)
    {
        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
        ImGuiWindowFlags flags = m_dirty ? ImGuiWindowFlags_UnsavedDocument : 0;
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
        {
            const float errorsH = 140.0f;
            DrawSnippetEditor(ImGui::GetContentRegionAvail().y - errorsH);
            DrawErrorsPanel();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##right", ImVec2(0, contentH));
        {
            DrawPreviewPanel(ImGui::GetContentRegionAvail().y * 0.55f);
            DrawParamsPanel();
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
        ImGui::SameLine();
        ImGui::Checkbox("Live", &m_live);
        ImGui::SameLine();
        if (ImGui::Button("Compile"))
            Rebuild();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        // Preview-surface selector: fullscreen today; "Sprite" joins in Slice 8.
        int surface = 0;
        ImGui::Combo("##surface", &surface, "Fullscreen\0");
        ImGui::SameLine();
        if (HasErrors())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "errors");
        else if (m_pass && m_pass->Ready())
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
        static SnippetCallbackCtx ctx;   // per-frame rebind; ImGui is single-threaded
        ctx.text = &m_snippet;
        if (m_jumpToLine > 0)
        {
            ctx.jumpToLine = m_jumpToLine;
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
                                      ImVec2(-1.0f, height), flags, &SnippetCallback, &ctx))
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
            char label[64];
            std::snprintf(label, sizeof(label), "%s(%d): ##diag%d",
                          isError ? "error" : "warning", d.line, i++);
            // Selectable row -> jump the text cursor to the diagnostic's line.
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if (ImGui::Selectable((std::string(label) + d.message).c_str(), false))
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
        if (m_preview && m_pass && m_pass->Ready())
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

    void ShaderEditorDocument::DrawParamsPanel()
    {
        ImGui::BeginChild("##params", ImVec2(0, 0), ImGuiChildFlags_Borders);
        if (!m_instance || !m_boundTemplate)
        {
            ImGui::TextDisabled("params appear after the first successful compile");
            ImGui::EndChild();
            return;
        }

        // Before-state captured on widget activation; the undo step is pushed on
        // release-after-edit (one drag = one step). Static is safe: one widget is
        // active at a time and ImGui is single-threaded.
        static bool s_hadBefore = false;
        static Arcane::MatParamValue s_before;

        const auto& params = m_boundTemplate->Params();
        for (std::size_t i = 0; i < params.size(); ++i)
        {
            const Arcane::ParamDecl& d = params[i];
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
                {
                    // Guid text field for now; the asset-browser picker + drag
                    // target arrive with Slice 6.
                    char buf[64];
                    const std::string s = value.tex.IsValid() ? value.tex.ToString() : "";
                    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
                    if (ImGui::InputText(d.name.c_str(), buf, sizeof(buf),
                                         ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        if (const auto g = Arcane::Guid::FromString(buf))
                        {
                            value = Arcane::MatParamValue::MakeTexture(*g);
                            edited = true;
                        }
                        else if (buf[0] == '\0')
                        {
                            value = Arcane::MatParamValue::MakeTexture(Arcane::Guid::Nil());
                            edited = true;
                        }
                    }
                    break;
                }
            }

            if (ImGui::IsItemActivated())
            {
                s_hadBefore = m_instance->HasOverride(d.nameHash);
                s_before = Arcane::MatParamValue{};
                if (s_hadBefore)
                    m_instance->GetParam(d.nameHash, s_before);
            }

            if (edited)
            {
                // LIVE: straight into the instance -> next Tick packs the CB.
                // No recompile -- the whole point of the declared-param model.
                m_instance->Set(d.nameHash, value);
                m_dirty = true;
            }

            if (ImGui::IsItemDeactivatedAfterEdit() && m_services.undo)
            {
                Arcane::MatParamValue after;
                if (m_instance->GetParam(d.nameHash, after))
                {
                    m_services.undo->Push(std::make_unique<ParamEditCommand>(
                        m_instance, d.nameHash, "Edit " + d.name,
                        s_hadBefore, s_before, /*hasAfter=*/true, after));
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
                    m_dirty = true;
                    if (m_services.undo)
                        m_services.undo->Push(std::make_unique<ParamEditCommand>(
                            m_instance, d.nameHash, "Reset " + d.name,
                            /*hadBefore=*/true, before, /*hasAfter=*/false,
                            Arcane::MatParamValue{}));
                }
            }
        }
        ImGui::EndChild();
    }
}

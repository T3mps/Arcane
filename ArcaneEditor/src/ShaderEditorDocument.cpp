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
#include <Arcane/Material/MaterialGraph.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/ShaderConventions.hpp>

#include <imgui.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
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

        std::uint64_t StageKey(const Arcane::Guid& id, bool vertex,
                               std::size_t pass = 0)
        {
            // Coalesce per (document, stage, pass): a newer submit of the SAME
            // stage supersedes; stages never cancel each other. The pass bits
            // sit at << 8, clear of the stage bits and the sprite cache's
            // guid^0x4/0x8 keys.
            return (id.hi ^ (id.lo * 1099511628211ull)) ^ (vertex ? 0x1u : 0x2u) ^
                   (static_cast<std::uint64_t>(pass) << 8);
        }

        // ---- Graph canvas plumbing (Slice 9) ----
        namespace ed = ax::NodeEditor;

        // Pass-canvas fixed ids (chain index c = node id c+1; these sit far
        // above any realistic pass count).
        constexpr std::uint32_t kPassOutputNodeId = 900000;
        constexpr std::uint32_t kPassOutputLinkId = 800000;

        // Pin id encoding: node id * 1000 + slot band. Inputs at +1.., outputs
        // at +501.. (a node type never has anywhere near 500 pins).
        constexpr std::uint64_t kPinInBase = 1, kPinOutBase = 501;
        ed::PinId InPin(std::uint32_t node, std::uint32_t pin)
        { return ed::PinId(node * 1000ull + kPinInBase + pin); }
        ed::PinId OutPin(std::uint32_t node, std::uint32_t pin)
        { return ed::PinId(node * 1000ull + kPinOutBase + pin); }
        struct DecodedPin
        {
            std::uint32_t node = 0, pin = 0;
            bool isInput = false, valid = false;
        };
        DecodedPin DecodePin(ed::PinId id)
        {
            DecodedPin d;
            if (!id)
                return d;
            const std::uint64_t v = static_cast<std::uint64_t>(id.Get());
            d.node = static_cast<std::uint32_t>(v / 1000ull);
            const std::uint64_t rem = v % 1000ull;
            d.isInput = rem < kPinOutBase;
            d.pin = static_cast<std::uint32_t>(d.isInput ? rem - kPinInBase
                                                         : rem - kPinOutBase);
            d.valid = d.node != 0;
            return d;
        }

        // Case-insensitive substring match for the create-menu search field.
        bool ContainsInsensitive(const char* hay, const char* needle)
        {
            const std::size_t n = std::strlen(needle);
            if (n == 0)
                return true;
            for (const char* p = hay; *p; ++p)
            {
                std::size_t i = 0;
                while (i < n && p[i] &&
                       std::tolower(static_cast<unsigned char>(p[i])) ==
                           std::tolower(static_cast<unsigned char>(needle[i])))
                    ++i;
                if (i == n)
                    return true;
            }
            return false;
        }

        // Would adding data-flow edge from->to close a cycle? Yes iff `from` is
        // already downstream of `to`. (Codegen re-detects as the backstop; this
        // check is what makes the canvas REFUSE the wire silently, SG-style.)
        bool WouldCycle(const Arcane::MaterialGraph& g, std::uint32_t from, std::uint32_t to)
        {
            if (from == to)
                return true;
            std::vector<std::uint32_t> stack{ to };
            std::unordered_set<std::uint32_t> seen;
            while (!stack.empty())
            {
                const std::uint32_t cur = stack.back();
                stack.pop_back();
                if (cur == from)
                    return true;
                if (!seen.insert(cur).second)
                    continue;
                for (const Arcane::GraphLink& l : g.links)
                    if (l.fromNode == cur)
                        stack.push_back(l.toNode);
            }
            return false;
        }

        // One graph gesture as an undo step (same doc-identity anchor pattern
        // as ParamEditCommand). Whole-graph before/after: our graphs are tens
        // of nodes -- the SG full-snapshot-undo pathology was per-edit JSON
        // reserialization + full preview regeneration, neither of which this
        // does (ApplyGraphState reuses the debounced compile loop).
        class GraphEditCommand final : public Arcane::ICommand
        {
        public:
            GraphEditCommand(std::weak_ptr<ShaderEditorDocument*> anchor, std::string label,
                             std::size_t pass,
                             std::optional<Arcane::MaterialGraph> before,
                             std::optional<Arcane::MaterialGraph> after)
                : m_anchor(std::move(anchor)), m_label(std::move(label)), m_pass(pass),
                  m_before(std::move(before)), m_after(std::move(after))
            {
            }

            void Undo() override { Apply(m_before); }
            void Redo() override { Apply(m_after); }
            const char* Label() const override { return m_label.c_str(); }

        private:
            void Apply(const std::optional<Arcane::MaterialGraph>& state)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplyGraphState(m_pass, state);
            }

            std::weak_ptr<ShaderEditorDocument*> m_anchor;
            std::string m_label;
            std::size_t m_pass;   // which pass's graph the step edits (0 = base)
            std::optional<Arcane::MaterialGraph> m_before, m_after;
        };
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
                // ActiveSnippet: pass chains route the editor at the selected
                // pass's buffer (pass 0 = m_snippet, unchanged single-pass).
                std::string& buf = doc->ActiveSnippet();
                buf.resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = buf.data();
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
            // Regenerate every graph-owned pass (deterministic codegen == the
            // saved snippets, so this leaves the doc clean) then compile; for
            // text-only docs the regen loop no-ops into a plain Rebuild.
            RegenerateFromGraph();
        }
    }

    ShaderEditorDocument::~ShaderEditorDocument()
    {
        if (m_graphCtx)
        {
            ed::DestroyEditor(m_graphCtx);
            m_graphCtx = nullptr;
        }
        if (m_passCanvasCtx)
        {
            ed::DestroyEditor(m_passCanvasCtx);
            m_passCanvasCtx = nullptr;
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

        // Invalidate every in-flight job (both paths): a result for a source
        // this Rebuild replaced must not bind.
        m_vsJob = m_psJob = 0;
        m_vsBytes.clear();
        m_psBytes.clear();
        m_passJobs.clear();

        if (ChainMode())
        {
            std::vector<Arcane::MaterialChainPassDesc> descs;
            descs.reserve(1 + m_data.passes.size());
            descs.push_back({ m_snippet, {} });
            for (const Arcane::MaterialPass& p : m_data.passes)
                descs.push_back({ p.snippet, p.inputs });

            Arcane::MaterialChainBuildResult build =
                Arcane::BuildMaterialChainSource(*templateText, descs, m_title);
            m_passInputs = std::move(build.passInputs);
            m_chainInputSlots = build.chainInputSlots;
            m_parseErrors = std::move(build.errors);
            for (std::size_t p = 0; p < build.passErrors.size(); ++p)
                for (const std::string& e : build.passErrors[p])
                    m_parseErrors.push_back(PassLabel(p) + ": " + e);

            m_passLineOffsets.assign(build.hlsl.size(), 0);
            for (std::size_t p = 0; p < build.hlsl.size(); ++p)
                if (const std::size_t at = build.hlsl[p].find(descs[p].snippet);
                    at != std::string::npos)
                    m_passLineOffsets[p] = static_cast<int>(std::count(
                        build.hlsl[p].begin(),
                        build.hlsl[p].begin() + static_cast<std::ptrdiff_t>(at), '\n'));
            m_snippetLineOffset = m_passLineOffsets.empty() ? 0 : m_passLineOffsets[0];
            m_pendingTemplate =
                std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
            m_metas = std::move(build.metas);

            m_passJobs.resize(build.hlsl.size());
            for (std::size_t p = 0; p < build.hlsl.size(); ++p)
            {
                Arcane::ShaderCompileRequest req;
                req.debugName = m_title + "_p" + std::to_string(p) + ".hlsl";
                req.sourceUtf8 = build.hlsl[p];
                req.entry = Arcane::kPsEntry;
                req.profile = Arcane::kPsProfile;
                req.coalesceKey = StageKey(m_data.id, /*vertex=*/false, p);
                m_passJobs[p].psJob = m_services.compiler->Submit(req, Now());
                req.entry = Arcane::kVsEntry;
                req.profile = Arcane::kVsProfile;
                req.coalesceKey = StageKey(m_data.id, /*vertex=*/true, p);
                m_passJobs[p].vsJob = m_services.compiler->Submit(std::move(req), Now());
            }
            return;
        }

        Arcane::MaterialBuildResult build =
            Arcane::BuildMaterialShaderSource(*templateText, SnippetSource(), m_title,
                                              SurfaceOf(m_surface));
        m_parseErrors = std::move(build.errors);
        // The snippet rides verbatim into the stitched source -- its line
        // offset maps compiler diag lines back into snippet space (error-row
        // jump; graph node badges via the codegen line map).
        m_snippetLineOffset = 0;
        if (const std::size_t at = build.hlsl.find(SnippetSource());
            at != std::string::npos)
            m_snippetLineOffset = static_cast<int>(
                std::count(build.hlsl.begin(), build.hlsl.begin() + at, '\n'));
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
        // Chain-mode jobs first (mutually exclusive with the single-path ids --
        // Rebuild clears whichever set is not in play).
        for (std::size_t p = 0; p < m_passJobs.size(); ++p)
        {
            PassJobs& pj = m_passJobs[p];
            const bool chainVs = result.jobId == pj.vsJob && pj.vsJob != 0;
            const bool chainPs = result.jobId == pj.psJob && pj.psJob != 0;
            if (!chainVs && !chainPs)
                continue;
            const auto& target = m_services.backend == Arcane::GraphicsBackend::Vulkan
                                     ? result.spirv : result.dxil;
            if (chainPs)
            {
                pj.diags = target.diags;
                if (p == 0)
                    m_diags = target.diags;   // single-path mirror
                // Badges belong to the ACTIVE pass's canvas (any pass may be
                // graph-owned now).
                RebuildDiagBadges();
            }
            if (target.succeeded)
            {
                (chainVs ? pj.vsBytes : pj.psBytes) = target.bytecode;
                BindIfComplete();
            }
            return true;
        }

        const bool isVs = result.jobId == m_vsJob && m_vsJob != 0;
        const bool isPs = result.jobId == m_psJob && m_psJob != 0;
        if (!isVs && !isPs)
            return false;

        const auto& target = m_services.backend == Arcane::GraphicsBackend::Vulkan
                                 ? result.spirv : result.dxil;
        if (isPs)
        {
            m_diags = target.diags;   // the ps stage carries the designer-relevant diags
            RebuildDiagBadges();
        }
        if (target.succeeded)
        {
            (isVs ? m_vsBytes : m_psBytes) = target.bytecode;
            BindIfComplete();
        }
        return true;
    }

    void ShaderEditorDocument::BindIfComplete()
    {
        if (ChainMode())
        {
            BindChainIfComplete();
            return;
        }
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

        PromotePendingInstance();

        if (sprite)
        {
            m_previewVs = vs;
            m_previewPs = ps;
            RefreshSpritePreviewBinding();
        }
    }

    void ShaderEditorDocument::PromotePendingInstance()
    {
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
    }

    void ShaderEditorDocument::BindChainIfComplete()
    {
        if (!m_pendingTemplate || m_passJobs.empty() || !m_services.device)
            return;
        for (const PassJobs& pj : m_passJobs)
            if (pj.vsBytes.empty() || pj.psBytes.empty())
                return;   // a stage is still in flight (or failed -- last-good stays)

        if (!m_chain)
            m_chain = Arcane::FullscreenMaterialChain::Create(m_services.device);
        if (!m_chain)
            return;

        std::vector<Arcane::FullscreenMaterialChain::PassShaders> shaders;
        shaders.reserve(m_passJobs.size());
        for (std::size_t p = 0; p < m_passJobs.size(); ++p)
        {
            PassJobs& pj = m_passJobs[p];
            const std::string stem = m_title + "_p" + std::to_string(p);
            Arcane::FullscreenMaterialChain::PassShaders ps;
            ps.vs = m_services.device->createShader(
                nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex)
                    .setEntryName(Arcane::kVsEntry).setDebugName((stem + "_vs").c_str()),
                pj.vsBytes.data(), pj.vsBytes.size());
            ps.ps = m_services.device->createShader(
                nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel)
                    .setEntryName(Arcane::kPsEntry).setDebugName((stem + "_ps").c_str()),
                pj.psBytes.data(), pj.psBytes.size());
            if (p < m_passInputs.size())
                ps.inputs = m_passInputs[p];
            pj.vsBytes.clear();
            pj.psBytes.clear();
            if (!ps.vs || !ps.ps)
                return;
            shaders.push_back(std::move(ps));
        }
        // Atomic: a rejected chain keeps the previous one bound AND the
        // previous instance (same last-good shape as SetMaterial).
        if (!m_chain->SetChain(m_pendingTemplate, shaders, m_chainInputSlots))
            return;

        PromotePendingInstance();
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
        for (const PassJobs& pj : m_passJobs)
            for (const Arcane::ShaderDiag& d : pj.diags)
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
        if (m_surface == 1)
            return m_previewSpriteMaterial != Arcane::Batcher2D::kInvalidMaterialId;
        if (ChainMode())
            return m_chain && m_chain->Ready();
        return m_pass && m_pass->Ready();
    }

    std::string& ShaderEditorDocument::ActiveSnippet()
    {
        if (m_activePass > 0 &&
            m_activePass <= static_cast<int>(m_data.passes.size()))
            return m_data.passes[static_cast<std::size_t>(m_activePass) - 1].snippet;
        return m_snippet;
    }

    std::string ShaderEditorDocument::PassLabel(std::size_t pass) const
    {
        if (pass == 0)
            return "base";
        if (pass <= m_data.passes.size())
        {
            const std::string& n = m_data.passes[pass - 1].name;
            if (!n.empty())
                return n;
        }
        return "pass " + std::to_string(pass);
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
                Arcane::Assets* assets =
                    m_services.runtime ? &m_services.runtime->AssetsFacade() : nullptr;
                if (ChainMode())
                {
                    // View-any-intermediate: the chain truncates at the viewed
                    // pass, which renders into the preview canvas itself.
                    const std::size_t view =
                        m_viewPass < 0 ? static_cast<std::size_t>(-1)
                                       : static_cast<std::size_t>(m_viewPass);
                    m_chain->Render(cl, fb, *m_instance, globals, assets, view);
                }
                else
                    m_pass->Render(cl, fb, *m_instance, globals, assets);
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
            // Pass canvas: fullscreen base materials only (sprite chains are
            // refused; instances re-value the base's chain). Even a single-pass
            // material shows base -> Output -- the pipeline affordance and the
            // right-click Add Pass entry point.
            if (m_surface == 0)
                DrawPassCanvas(170.0f);
            if (m_activePass > static_cast<int>(m_data.passes.size()))
                m_activePass = 0;   // stale selection after an outside reload
            // The canvas serves whichever pass is active and graph-owned;
            // text-owned passes get the text editor.
            if (ActiveGraphOwned() && !m_showGeneratedText)
                DrawGraphPanel(ImGui::GetContentRegionAvail().y - errorsH);
            else
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
                RegenerateFromGraph();   // regenerates graph passes, then Rebuild
            if (ActiveGraphOwned())
            {
                // Read-only generated-code view (UE's HLSL window / SG's View
                // Generated Shader). No convert-out: graphs are THE authoring
                // tier; freeform HLSL lives in Custom nodes.
                ImGui::SameLine();
                ImGui::Checkbox("HLSL", &m_showGeneratedText);
            }
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
        // Pass chains are fullscreen-only: the selector LOCKS while extra
        // passes exist (no refusal path can lose data).
        const bool surfaceLocked = !IsInstance() && !m_data.passes.empty();
        if (surfaceLocked)
            ImGui::BeginDisabled();
        int surface = m_surface;
        const bool surfacePicked =
            ImGui::Combo("##surface", &surface, "Fullscreen\0Sprite\0");
        if (surfaceLocked)
        {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("pass chains are fullscreen-only -- remove the "
                                  "extra passes to change the surface");
        }
        if (surfacePicked && surface != m_surface)
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
            // Graph docs must re-CODEGEN, not just restitch -- the surface
            // gates node validity (VertexColor/SpriteTexture are sprite-only).
            RegenerateFromGraph();
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
        // Chain mode: say which pass the buffer belongs to (the pass canvas
        // above is the selector).
        if (ChainMode())
            ImGui::TextDisabled("editing: %s",
                                PassLabel(static_cast<std::size_t>(m_activePass)).c_str());
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
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput |
                                    ImGuiInputTextFlags_CallbackResize |
                                    ImGuiInputTextFlags_CallbackAlways;
        // Graph-owned passes: the text is GENERATED -- readable (and jumpable)
        // but never hand-edited.
        if (ActiveGraphOwned())
            flags |= ImGuiInputTextFlags_ReadOnly;
        // Per-pass widget identity: switching passes swaps the buffer under
        // the widget, which must not inherit the previous pass's edit state.
        ImGui::PushID(m_activePass);
        std::string& snippet = ActiveSnippet();
        // +1 capacity: ImGui writes the terminator into the buffer it is given.
        if (ImGui::InputTextMultiline("##snippet", snippet.data(), snippet.capacity() + 1,
                                      ImVec2(-1.0f, height), flags,
                                      &SnippetCallbackForwarder::Callback, this))
        {
            m_dirty = true;
            if (m_live)
                Rebuild();
        }
        ImGui::PopID();
    }

    bool ShaderEditorDocument::PassWireWouldCycle(std::uint32_t source,
                                                  std::uint32_t consumer) const
    {
        // Adding consumer.inputs += source cycles iff `consumer` is already
        // upstream of `source` (walk source's input ancestry).
        if (source == consumer)
            return true;
        std::vector<std::uint32_t> stack{ source };
        std::unordered_set<std::uint32_t> seen;
        while (!stack.empty())
        {
            const std::uint32_t c = stack.back();
            stack.pop_back();
            if (c == consumer)
                return true;
            if (!seen.insert(c).second || c == 0)
                continue;
            if (c - 1 < m_data.passes.size())
                for (std::uint32_t in : m_data.passes[c - 1].inputs)
                    stack.push_back(in);
        }
        return false;
    }

    bool ShaderEditorDocument::TopoSortPasses()
    {
        // Stable Kahn over chain indices (base = 0 is always first). Positions
        // ride each MaterialPass; active/view indices remap.
        const std::size_t n = m_data.passes.size();
        if (n == 0)
            return true;
        std::vector<std::uint32_t> order;   // new sequence of OLD chain indices
        std::vector<bool> placed(n + 1, false);
        placed[0] = true;
        bool progress = true;
        while (order.size() < n && progress)
        {
            progress = false;
            for (std::uint32_t c = 1; c <= n; ++c)
            {
                if (placed[c])
                    continue;
                bool ready = true;
                for (std::uint32_t in : m_data.passes[c - 1].inputs)
                    ready = ready && in <= n && placed[in];
                if (!ready)
                    continue;
                placed[c] = true;
                order.push_back(c);
                progress = true;
            }
        }
        if (order.size() < n)
            return false;   // cycle -- the canvas refuses these at wire time

        std::vector<std::uint32_t> remap(n + 1, 0);
        for (std::size_t i = 0; i < order.size(); ++i)
            remap[order[i]] = static_cast<std::uint32_t>(i) + 1;
        std::vector<Arcane::MaterialPass> sorted;
        sorted.reserve(n);
        for (std::uint32_t old : order)
            sorted.push_back(std::move(m_data.passes[old - 1]));
        for (Arcane::MaterialPass& p : sorted)
            for (std::uint32_t& in : p.inputs)
                in = remap[in];
        m_data.passes = std::move(sorted);
        if (m_activePass > 0 && m_activePass <= static_cast<int>(n))
            m_activePass = static_cast<int>(remap[static_cast<std::uint32_t>(m_activePass)]);
        if (m_viewPass > 0 && m_viewPass <= static_cast<int>(n))
            m_viewPass = static_cast<int>(remap[static_cast<std::uint32_t>(m_viewPass)]);
        return true;
    }

    void ShaderEditorDocument::DrawPassCanvas(float height)
    {
        // The pass DAG as a canvas (replaces the pass bar): chain index c is
        // node id c+1, the Output node is kPassOutputNodeId, and the WIRES ARE
        // THE DATA -- a link into input pin s of a pass IS inputs[s]. Edits are
        // structural (recompile), not undoable -- same standing as text edits.
        if (!m_passCanvasCtx)
        {
            ed::Config cfg;
            cfg.SettingsFile = nullptr;
            m_passCanvasCtx = ed::CreateEditor(&cfg);
            m_passCanvasSeeded = false;
        }
        const std::size_t total = 1 + m_data.passes.size();
        auto nodeOf = [](std::size_t chain) { return static_cast<std::uint32_t>(chain) + 1; };

        ed::SetCurrentEditor(m_passCanvasCtx);
        ed::Begin("##passcanvas", ImVec2(0.0f, height));

        const bool seededThisFrame = !m_passCanvasSeeded;
        if (seededThisFrame)
        {
            // Never-laid-out data (all zeros, incl. pre-canvas files): a simple
            // left-to-right row.
            bool anyPos = m_data.chainBaseX != 0.0f || m_data.chainBaseY != 0.0f ||
                          m_data.chainOutX != 0.0f || m_data.chainOutY != 0.0f;
            for (const Arcane::MaterialPass& p : m_data.passes)
                anyPos = anyPos || p.posX != 0.0f || p.posY != 0.0f;
            if (!anyPos)
            {
                m_data.chainBaseX = 40.0f;
                m_data.chainBaseY = 40.0f;
                for (std::size_t k = 0; k < m_data.passes.size(); ++k)
                {
                    m_data.passes[k].posX = 40.0f + 190.0f * static_cast<float>(k + 1);
                    m_data.passes[k].posY = 40.0f;
                }
                m_data.chainOutX = 40.0f + 190.0f * static_cast<float>(total);
                m_data.chainOutY = 40.0f;
            }
            ed::SetNodePosition(nodeOf(0), ImVec2(m_data.chainBaseX, m_data.chainBaseY));
            for (std::size_t k = 0; k < m_data.passes.size(); ++k)
                ed::SetNodePosition(nodeOf(k + 1),
                                    ImVec2(m_data.passes[k].posX, m_data.passes[k].posY));
            ed::SetNodePosition(kPassOutputNodeId,
                                ImVec2(m_data.chainOutX, m_data.chainOutY));
            m_passCanvasSeeded = true;
        }

        // ---- nodes
        Arcane::FullscreenMaterialChain* chain = ChainMode() ? m_chain.get() : nullptr;
        for (std::size_t c = 0; c < total; ++c)
        {
            const std::uint32_t nodeId = nodeOf(c);
            ed::BeginNode(ed::NodeId(nodeId));
            ImGui::PushID(static_cast<int>(nodeId));

            bool passError = false;
            if (c < m_passJobs.size())
                for (const Arcane::ShaderDiag& d : m_passJobs[c].diags)
                    passError = passError ||
                                d.severity == Arcane::ShaderDiagSeverity::Error;
            const std::string title =
                (m_activePass == static_cast<int>(c) ? "> " : "") + PassLabel(c);
            if (passError)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "(!) %s", title.c_str());
            else
                ImGui::TextUnformatted(title.c_str());

            // Extra passes rename in-node (stable-buffer commit, no undo --
            // structural-edit standing).
            if (c >= 1)
            {
                Arcane::MaterialPass& pass = m_data.passes[c - 1];
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s", pass.name.c_str());
                if (m_passNameEditIdx == static_cast<int>(c))
                    std::memcpy(buf, m_nameBuf, sizeof(buf));
                ImGui::SetNextItemWidth(120.0f);
                ImGui::InputText("##passname", buf, sizeof(buf));
                if (ImGui::IsItemActive())
                {
                    m_passNameEditIdx = static_cast<int>(c);
                    std::memcpy(m_nameBuf, buf, sizeof(m_nameBuf));
                }
                else if (m_passNameEditIdx == static_cast<int>(c))
                {
                    const bool commit = ImGui::IsItemDeactivatedAfterEdit();
                    m_passNameEditIdx = -1;
                    if (commit && pass.name != m_nameBuf)
                    {
                        pass.name = m_nameBuf;
                        m_dirty = true;
                    }
                }
            }

            // Input pins: one per wired slot + a spare that accepts a new wire.
            const std::size_t inputCount =
                c >= 1 ? m_data.passes[c - 1].inputs.size() : 0;
            if (c >= 1)
            {
                for (std::size_t s = 0; s < inputCount; ++s)
                {
                    ed::BeginPin(InPin(nodeId, static_cast<std::uint32_t>(s)),
                                 ed::PinKind::Input);
                    ImGui::Text("-> in%zu", s);
                    ed::EndPin();
                }
                if (inputCount < Arcane::kMaxPassInputs)
                {
                    ed::BeginPin(InPin(nodeId, static_cast<std::uint32_t>(inputCount)),
                                 ed::PinKind::Input);
                    ImGui::TextDisabled("-> +");
                    ed::EndPin();
                }
            }

            // Live thumbnail: the pass's own intermediate (chain mode; LINEAR,
            // so HDR clamps -- fine for a thumbnail). Single-pass materials
            // show the tonemapped preview on the base node instead.
            nvrhi::ITexture* thumb = chain ? chain->PassOutput(c) : nullptr;
            ImTextureID thumbId = 0;
            if (thumb)
                thumbId = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(thumb));
            else if (c == 0 && m_preview && PreviewReady())
                thumbId = static_cast<ImTextureID>(m_preview->TextureId());
            if (thumbId)
                ImGui::Image(thumbId, ImVec2(72.0f, 72.0f));

            ed::BeginPin(OutPin(nodeId, 0), ed::PinKind::Output);
            ImGui::TextUnformatted("out ->");
            ed::EndPin();

            ImGui::PopID();
            ed::EndNode();
        }

        // The Output node: shows the final image; its wire marks the LAST pass
        // (execution order's tail = what single-material consumers see).
        ed::BeginNode(ed::NodeId(kPassOutputNodeId));
        ImGui::TextUnformatted("Output");
        ed::BeginPin(InPin(kPassOutputNodeId, 0), ed::PinKind::Input);
        ImGui::TextUnformatted("-> final");
        ed::EndPin();
        if (nvrhi::ITexture* finalTex = chain ? chain->PassOutput(total - 1) : nullptr)
            ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(finalTex)),
                         ImVec2(72.0f, 72.0f));
        ed::EndNode();

        // ---- links (derived from the data each frame; ids = list index + 1)
        std::vector<std::pair<std::uint32_t, std::uint32_t>> linkSlots;   // consumer, slot
        for (std::size_t c = 1; c < total; ++c)
            for (std::size_t s = 0; s < m_data.passes[c - 1].inputs.size(); ++s)
            {
                const std::uint32_t src = m_data.passes[c - 1].inputs[s];
                linkSlots.emplace_back(static_cast<std::uint32_t>(c),
                                       static_cast<std::uint32_t>(s));
                ed::Link(ed::LinkId(linkSlots.size()),
                         OutPin(nodeOf(src), 0),
                         InPin(nodeOf(c), static_cast<std::uint32_t>(s)));
            }
        ed::Link(ed::LinkId(kPassOutputLinkId), OutPin(nodeOf(total - 1), 0),
                 InPin(kPassOutputNodeId, 0));

        // ---- wire edits
        bool structural = false;
        if (ed::BeginCreate())
        {
            ed::PinId aId, bId;
            if (ed::QueryNewLink(&aId, &bId))
            {
                const DecodedPin a = DecodePin(aId);
                const DecodedPin b = DecodePin(bId);
                const DecodedPin& out = a.isInput ? b : a;
                const DecodedPin& in = a.isInput ? a : b;
                bool valid = a.valid && b.valid && a.isInput != b.isInput &&
                             out.node != kPassOutputNodeId &&
                             out.node >= 1 && out.node <= total;
                const std::uint32_t source = out.node - 1;
                if (valid && in.node == kPassOutputNodeId)
                {
                    // Make `source` the final pass: legal only when nothing
                    // reads it (a consumer must execute after it).
                    bool hasDependent = false;
                    for (const Arcane::MaterialPass& p : m_data.passes)
                        for (std::uint32_t pin : p.inputs)
                            hasDependent = hasDependent || pin == source;
                    if (source == 0 || source == total - 1 || hasDependent)
                        ed::RejectNewItem();
                    else if (ed::AcceptNewItem())
                    {
                        Arcane::MaterialPass moved =
                            std::move(m_data.passes[source - 1]);
                        m_data.passes.erase(m_data.passes.begin() +
                                            static_cast<std::ptrdiff_t>(source - 1));
                        m_data.passes.push_back(std::move(moved));
                        for (Arcane::MaterialPass& p : m_data.passes)
                            for (std::uint32_t& pin : p.inputs)
                                pin = pin == source
                                          ? static_cast<std::uint32_t>(total - 1)
                                          : pin > source ? pin - 1 : pin;
                        if (m_activePass == static_cast<int>(source))
                            m_activePass = static_cast<int>(total - 1);
                        else if (m_activePass > static_cast<int>(source))
                            --m_activePass;
                        m_viewPass = -1;
                        structural = true;
                    }
                }
                else
                {
                    const std::uint32_t consumer = in.node - 1;
                    valid = valid && in.node >= 2 && in.node <= total &&
                            in.pin <= m_data.passes[consumer - 1].inputs.size() &&
                            in.pin < Arcane::kMaxPassInputs &&
                            !PassWireWouldCycle(source, consumer);
                    if (!valid)
                        ed::RejectNewItem();
                    else if (ed::AcceptNewItem())
                    {
                        std::vector<std::uint32_t>& ins =
                            m_data.passes[consumer - 1].inputs;
                        if (in.pin < ins.size())
                            ins[in.pin] = source;   // silent replace
                        else
                            ins.push_back(source);  // the spare pin
                        TopoSortPasses();
                        structural = true;
                    }
                }
            }
        }
        ed::EndCreate();   // UNCONDITIONAL (the material-canvas lesson)

        // ---- deletions: links = unwire a slot; nodes = remove the pass
        std::vector<std::pair<std::uint32_t, std::uint32_t>> unwire;
        std::vector<std::uint32_t> removePasses;   // chain indices
        if (ed::BeginDelete())
        {
            ed::LinkId lid;
            while (ed::QueryDeletedLink(&lid))
            {
                const std::size_t idx = static_cast<std::size_t>(lid.Get()) - 1;
                if (lid.Get() == kPassOutputLinkId || idx >= linkSlots.size())
                    ed::RejectDeletedItem();   // the final wire is structural
                else if (ed::AcceptDeletedItem())
                    unwire.push_back(linkSlots[idx]);
            }
            ed::NodeId nid;
            while (ed::QueryDeletedNode(&nid))
            {
                const std::uint32_t id = static_cast<std::uint32_t>(nid.Get());
                if (id < 2 || id > total)   // base + Output are fixed
                {
                    ed::RejectDeletedItem();
                    continue;
                }
                if (ed::AcceptDeletedItem())
                    removePasses.push_back(id - 1);
            }
        }
        ed::EndDelete();   // UNCONDITIONAL

        if (!unwire.empty() || !removePasses.empty())
        {
            // Unwire first (descending slot so indices stay valid), then remove
            // passes (descending chain index), fixing every reference.
            std::sort(unwire.rbegin(), unwire.rend());
            for (const auto& [consumer, slot] : unwire)
            {
                std::vector<std::uint32_t>& ins = m_data.passes[consumer - 1].inputs;
                if (slot < ins.size())
                    ins.erase(ins.begin() + slot);
            }
            std::sort(removePasses.rbegin(), removePasses.rend());
            for (std::uint32_t r : removePasses)
            {
                m_data.passes.erase(m_data.passes.begin() +
                                    static_cast<std::ptrdiff_t>(r - 1));
                for (Arcane::MaterialPass& p : m_data.passes)
                {
                    std::erase(p.inputs, r);
                    for (std::uint32_t& in : p.inputs)
                        if (in > r)
                            --in;
                }
                if (m_activePass >= static_cast<int>(r))
                    --m_activePass;
            }
            m_activePass = std::clamp(m_activePass, 0,
                                      static_cast<int>(m_data.passes.size()));
            m_viewPass = -1;
            structural = true;
        }

        // ---- selection -> edited pass; double-click -> viewed pass. Only on
        // selection CHANGES -- re-asserting every frame would stomp the
        // errors-panel's own pass switching.
        {
            if (ed::HasSelectionChanged())
            {
                std::vector<ed::NodeId> sel(
                    static_cast<std::size_t>(std::max(0, ed::GetSelectedObjectCount())));
                if (!sel.empty())
                {
                    const int count = ed::GetSelectedNodes(sel.data(),
                                                           static_cast<int>(sel.size()));
                    if (count == 1)
                    {
                        const std::uint32_t id =
                            static_cast<std::uint32_t>(sel[0].Get());
                        if (id >= 1 && id <= total)
                            m_activePass = static_cast<int>(id - 1);
                    }
                }
            }
            const std::uint32_t dbl =
                static_cast<std::uint32_t>(ed::GetDoubleClickedNode().Get());
            if (dbl == kPassOutputNodeId)
                m_viewPass = -1;
            else if (dbl >= 1 && dbl <= total)
                m_viewPass = dbl == total ? -1 : static_cast<int>(dbl - 1);
        }

        // ---- context menus (Suspend: popups live in screen space)
        ed::Suspend();
        {
            ed::NodeId ctxNode;
            if (ed::ShowNodeContextMenu(&ctxNode))
            {
                m_passCtxNode = static_cast<std::uint32_t>(ctxNode.Get());
                ImGui::OpenPopup("##passnodemenu");
            }
            else if (ed::ShowBackgroundContextMenu())
            {
                const ImVec2 p = ImGui::GetMousePos();
                m_passPopupX = p.x;
                m_passPopupY = p.y;
                ImGui::OpenPopup("##passbgmenu");
            }
            if (ImGui::BeginPopup("##passnodemenu"))
            {
                const std::uint32_t id = m_passCtxNode;
                if (id == kPassOutputNodeId)
                {
                    if (ImGui::MenuItem("View Final"))
                        m_viewPass = -1;
                }
                else if (id >= 1 && id <= total)
                {
                    if (ImGui::MenuItem("View This Pass"))
                        m_viewPass = id == total ? -1 : static_cast<int>(id - 1);
                    if (id >= 2 && ImGui::MenuItem("Remove Pass"))
                    {
                        const std::uint32_t r = id - 1;
                        m_data.passes.erase(m_data.passes.begin() +
                                            static_cast<std::ptrdiff_t>(r - 1));
                        for (Arcane::MaterialPass& p : m_data.passes)
                        {
                            std::erase(p.inputs, r);
                            for (std::uint32_t& in : p.inputs)
                                if (in > r)
                                    --in;
                        }
                        if (m_activePass >= static_cast<int>(r))
                            --m_activePass;
                        m_activePass = std::clamp(
                            m_activePass, 0, static_cast<int>(m_data.passes.size()));
                        m_viewPass = -1;
                        structural = true;
                    }
                }
                ImGui::EndPopup();
            }
            if (ImGui::BeginPopup("##passbgmenu"))
            {
                if (ImGui::MenuItem("Add Pass"))
                {
                    Arcane::MaterialPass p;
                    p.name = "pass " + std::to_string(m_data.passes.size() + 1);
                    // UE model: new passes are GRAPH-owned. Starter = Pass
                    // Input (slot 0) wired to Output -- a visible passthrough,
                    // never an empty canvas. The snippet regenerates from it.
                    Arcane::MaterialGraph pg;
                    Arcane::GraphNode out;
                    out.id = 1;
                    out.type = Arcane::GraphNodeType::Output;
                    out.posX = 360.0f;
                    out.posY = 120.0f;
                    Arcane::GraphNode in;
                    in.id = 2;
                    in.type = Arcane::GraphNodeType::PassInput;
                    in.posX = 100.0f;
                    in.posY = 120.0f;
                    pg.nodes = { out, in };
                    Arcane::GraphLink link;
                    link.fromNode = 2;
                    link.toNode = 1;
                    pg.links.push_back(link);
                    pg.nextId = 3;
                    p.graph = std::move(pg);
                    // Default wiring: read the current final (linear extend).
                    p.inputs = { static_cast<std::uint32_t>(total - 1) };
                    const ImVec2 canvasPos =
                        ed::ScreenToCanvas(ImVec2(m_passPopupX, m_passPopupY));
                    p.posX = canvasPos.x;
                    p.posY = canvasPos.y;
                    m_data.passes.push_back(std::move(p));
                    m_activePass = static_cast<int>(m_data.passes.size());
                    structural = true;
                }
                ImGui::EndPopup();
            }
        }
        ed::Resume();

        // ---- position readback (skip the seed frame)
        if (!seededThisFrame && !structural)
        {
            auto readback = [&](std::uint32_t nodeId, float& x, float& y)
            {
                const ImVec2 p = ed::GetNodePosition(ed::NodeId(nodeId));
                if (p.x != x || p.y != y)
                {
                    x = p.x;
                    y = p.y;
                    m_dirty = true;
                }
            };
            readback(nodeOf(0), m_data.chainBaseX, m_data.chainBaseY);
            for (std::size_t k = 0; k < m_data.passes.size(); ++k)
                readback(nodeOf(k + 1), m_data.passes[k].posX, m_data.passes[k].posY);
            readback(kPassOutputNodeId, m_data.chainOutX, m_data.chainOutY);
        }

        ed::End();
        ed::SetCurrentEditor(nullptr);

        if (structural)
        {
            // Indices moved under the canvas: re-seed node ids from the data
            // next frame, then re-CODEGEN (rewires change which PassInput
            // slots are valid) and recompile the chain.
            m_passCanvasSeeded = false;
            m_dirty = true;
            if (m_live)
                RegenerateFromGraph();
        }
    }

    void ShaderEditorDocument::DrawErrorsPanel()
    {
        ImGui::BeginChild("##errors", ImVec2(0, 0), ImGuiChildFlags_Borders);
        // Graph-level codegen errors first (badged on the canvas too), for
        // EVERY pass's graph; clicking a row switches to that pass and selects
        // + navigates to the offending node.
        int gi = 0;
        bool anyGraphError = false;
        for (std::size_t c = 0; c < m_passGraphErrors.size(); ++c)
        {
            for (const Arcane::GraphError& e : m_passGraphErrors[c])
            {
                anyGraphError = true;
                std::string head =
                    m_data.passes.empty() ? std::string("graph: ")
                                          : PassLabel(c) + " graph: ";
                if (e.nodeId != 0)
                {
                    std::optional<Arcane::MaterialGraph>& g = GraphOptAt(c);
                    const Arcane::GraphNode* n = g ? g->FindNode(e.nodeId) : nullptr;
                    head += (n ? std::string(Arcane::GraphNodeInfo(n->type).display)
                               : std::string("node")) +
                            " #" + std::to_string(e.nodeId) + ": ";
                }
                const std::string label =
                    head + e.message + "##gdiag" + std::to_string(gi++);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
                if (ImGui::Selectable(label.c_str(), false))
                {
                    m_activePass = static_cast<int>(c);
                    if (e.nodeId != 0)
                        m_focusNode = e.nodeId;
                }
                ImGui::PopStyleColor();
            }
        }
        for (const std::string& e : m_parseErrors)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", e.c_str());
        // Chain mode: per-pass compile diags (labeled rows; clicking switches
        // the editor to that pass and jumps). m_diags mirrors pass 0 for the
        // graph badges, so the single-path loop below is skipped.
        if (ChainMode())
        {
            int ci = 0;
            for (std::size_t p = 0; p < m_passJobs.size(); ++p)
            {
                const int offset = p < m_passLineOffsets.size() ? m_passLineOffsets[p] : 0;
                for (const Arcane::ShaderDiag& d : m_passJobs[p].diags)
                {
                    const bool isError = d.severity == Arcane::ShaderDiagSeverity::Error;
                    const ImVec4 color = isError ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f)
                                                 : ImVec4(0.9f, 0.8f, 0.4f, 1.0f);
                    const int snippetLine = d.line > offset ? d.line - offset : 1;
                    char head[80];
                    std::snprintf(head, sizeof(head), "%s: %s(%d): ",
                                  PassLabel(p).c_str(),
                                  isError ? "error" : "warning", snippetLine);
                    const std::string label =
                        head + d.message + "##cdiag" + std::to_string(ci++);
                    ImGui::PushStyleColor(ImGuiCol_Text, color);
                    if (ImGui::Selectable(label.c_str(), false))
                    {
                        m_activePass = static_cast<int>(p);
                        m_jumpToLine = snippetLine;
                        if (p < m_passLineNodeIds.size() && !m_showGeneratedText)
                        {
                            const std::size_t idx =
                                static_cast<std::size_t>(snippetLine) - 1;
                            const auto& lineMap = m_passLineNodeIds[p];
                            if (idx < lineMap.size() && lineMap[idx] != 0)
                                m_focusNode = lineMap[idx];
                        }
                    }
                    ImGui::PopStyleColor();
                }
            }
            bool anyChainDiag = false;
            for (const PassJobs& pj : m_passJobs)
                anyChainDiag = anyChainDiag || !pj.diags.empty();
            if (!anyGraphError && m_parseErrors.empty() && !anyChainDiag)
                ImGui::TextDisabled("no diagnostics");
            ImGui::EndChild();
            return;
        }
        int i = 0;
        for (const Arcane::ShaderDiag& d : m_diags)
        {
            const bool isError = d.severity == Arcane::ShaderDiagSeverity::Error;
            const ImVec4 color = isError ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f)
                                         : ImVec4(0.9f, 0.8f, 0.4f, 1.0f);
            // Diag lines live in STITCHED-source space; the snippet offset maps
            // them back into the buffer the designer sees.
            const int snippetLine = d.line > m_snippetLineOffset
                                        ? d.line - m_snippetLineOffset : 1;
            // Review M2: the message must sit BEFORE the "##" -- ImGui renders
            // nothing past it (everything after is only the widget ID).
            char head[48];
            std::snprintf(head, sizeof(head), "%s(%d): ",
                          isError ? "error" : "warning", snippetLine);
            const std::string label = head + d.message + "##diag" + std::to_string(i++);
            // Selectable row -> jump the text cursor (graph docs: select the
            // statement's node when the line map knows it).
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if (ImGui::Selectable(label.c_str(), false))
            {
                m_jumpToLine = snippetLine;
                if (IsGraphOwned() && !m_showGeneratedText &&
                    !m_passLineNodeIds.empty())
                {
                    const std::size_t idx = static_cast<std::size_t>(snippetLine) - 1;
                    const auto& lineMap = m_passLineNodeIds[0];
                    if (idx < lineMap.size() && lineMap[idx] != 0)
                        m_focusNode = lineMap[idx];
                }
            }
            ImGui::PopStyleColor();
        }
        if (!anyGraphError && m_parseErrors.empty() && m_diags.empty())
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

    // ------------------------------------------------------------ graph mode
    std::optional<Arcane::MaterialGraph>& ShaderEditorDocument::GraphOptAt(std::size_t pass)
    {
        if (pass > 0 && pass <= m_data.passes.size())
            return m_data.passes[pass - 1].graph;
        return m_data.graph;
    }

    std::optional<Arcane::MaterialGraph>& ShaderEditorDocument::ActiveGraphOpt()
    {
        return GraphOptAt(static_cast<std::size_t>(std::max(0, m_activePass)));
    }

    void ShaderEditorDocument::RegenerateFromGraph()
    {
        const std::size_t total = 1 + m_data.passes.size();
        m_passGraphErrors.assign(total, std::vector<Arcane::GraphError>{});
        m_passLineNodeIds.assign(total, std::vector<std::uint32_t>{});
        bool anyError = false;
        for (std::size_t c = 0; c < total; ++c)
        {
            std::optional<Arcane::MaterialGraph>& g = GraphOptAt(c);
            if (!g)
                continue;
            // The pass context: PassInput nodes may only read slots the pass
            // canvas actually wired (the base has none).
            const std::uint32_t avail =
                c == 0 ? 0u
                       : static_cast<std::uint32_t>(m_data.passes[c - 1].inputs.size());
            Arcane::GraphCodegenResult r =
                Arcane::GenerateGraphSnippet(*g, SurfaceOf(m_surface), avail);
            if (!r.Ok())
            {
                m_passGraphErrors[c] = std::move(r.errors);
                anyError = true;
                continue;
            }
            m_passLineNodeIds[c] = std::move(r.lineNodeIds);
            (c == 0 ? m_snippet : m_data.passes[c - 1].snippet) = std::move(r.snippet);
        }
        if (!anyError)
            Rebuild();   // any codegen error keeps last-good bound; badges show why
    }

    void ShaderEditorDocument::ApplyGraphState(std::size_t pass,
                                               std::optional<Arcane::MaterialGraph> state)
    {
        if (pass > m_data.passes.size())
            return;   // the pass was removed since the step was pushed
        GraphOptAt(pass) = std::move(state);
        if (static_cast<int>(pass) == m_activePass)
            m_graphPositionsApplied = false;   // re-seed the canvas from the data
        m_dirty = true;
        RegenerateFromGraph();
    }

    void ShaderEditorDocument::PushGraphUndo(const char* label,
                                             std::optional<Arcane::MaterialGraph> before)
    {
        if (m_services.undo)
            m_services.undo->Push(std::make_unique<GraphEditCommand>(
                m_anchor, label, static_cast<std::size_t>(std::max(0, m_activePass)),
                std::move(before), ActiveGraphOpt()));
    }

    bool ShaderEditorDocument::NodeBadged(std::uint32_t nodeId) const
    {
        const std::size_t c = static_cast<std::size_t>(std::max(0, m_activePass));
        if (c < m_passGraphErrors.size())
            for (const Arcane::GraphError& e : m_passGraphErrors[c])
                if (e.nodeId == nodeId)
                    return true;
        for (std::uint32_t id : m_diagBadgeNodes)
            if (id == nodeId)
                return true;
        return false;
    }

    void ShaderEditorDocument::RebuildDiagBadges()
    {
        // Compile-diag badges for the ACTIVE pass's canvas: that pass's diags,
        // line offset, and line map (single-path docs are pass 0 throughout).
        m_diagBadgeNodes.clear();
        const std::size_t c = static_cast<std::size_t>(std::max(0, m_activePass));
        if (c >= m_passLineNodeIds.size() || m_passLineNodeIds[c].empty())
            return;
        const std::vector<Arcane::ShaderDiag>* diags = &m_diags;
        int offset = m_snippetLineOffset;
        if (ChainMode() && c < m_passJobs.size())
        {
            diags = &m_passJobs[c].diags;
            if (c < m_passLineOffsets.size())
                offset = m_passLineOffsets[c];
        }
        const std::vector<std::uint32_t>& lineMap = m_passLineNodeIds[c];
        for (const Arcane::ShaderDiag& d : *diags)
        {
            if (d.severity != Arcane::ShaderDiagSeverity::Error)
                continue;
            // stitched line -> snippet line -> statement's node (the line map).
            const int snippetLine = d.line - offset;
            const std::size_t idx = static_cast<std::size_t>(snippetLine) - 1;
            if (snippetLine >= 1 && idx < lineMap.size() && lineMap[idx] != 0)
                m_diagBadgeNodes.push_back(lineMap[idx]);
        }
    }

    void ShaderEditorDocument::DrawGraphPanel(float height)
    {
        if (!ActiveGraphOwned())
            return;
        if (!m_graphCtx)
        {
            ed::Config cfg;
            cfg.SettingsFile = nullptr;   // layout persists in the .arcmat, not an ini
            m_graphCtx = ed::CreateEditor(&cfg);
        }
        // ONE context serves every pass's graph: on a pass switch, re-seed
        // positions from the data (they persist per graph) and drop the
        // selection + stale badges -- node ids are only unique per graph.
        const bool switchedPass = m_graphShownPass != m_activePass;
        if (switchedPass)
        {
            m_graphShownPass = m_activePass;
            m_graphPositionsApplied = false;
            RebuildDiagBadges();
        }
        Arcane::MaterialGraph& g = *ActiveGraphOpt();

        ed::SetCurrentEditor(m_graphCtx);
        ed::Begin("##graphcanvas", ImVec2(0.0f, height));
        if (switchedPass)
            ed::ClearSelection();

        const bool seededThisFrame = !m_graphPositionsApplied;
        if (seededThisFrame)
        {
            for (const Arcane::GraphNode& n : g.nodes)
                ed::SetNodePosition(n.id, ImVec2(n.posX, n.posY));
            m_graphPositionsApplied = true;
        }

        for (Arcane::GraphNode& n : g.nodes)
            DrawGraphNode(n);

        // Links: ids are the vector index + 1, stable within this frame (the
        // deletion pass collects indices and erases after the queries).
        for (std::size_t i = 0; i < g.links.size(); ++i)
        {
            const Arcane::GraphLink& l = g.links[i];
            ed::Link(ed::LinkId(i + 1), OutPin(l.fromNode, l.fromPin),
                     InPin(l.toNode, l.toPin));
        }

        HandleGraphEdits();

        // Errors-panel click: select + frame the offending node.
        if (m_focusNode != 0)
        {
            ed::SelectNode(ed::NodeId(m_focusNode));
            ed::NavigateToSelection(true);
            m_focusNode = 0;
        }

        // Background context menu -> create node (Suspend: popups live in
        // normal ImGui space, not canvas space).
        ed::Suspend();
        if (ed::ShowBackgroundContextMenu())
        {
            m_wireActive = false;   // plain create -- no wire to connect
            const ImVec2 p = ImGui::GetMousePos();
            m_graphPopupX = p.x;
            m_graphPopupY = p.y;
            ImGui::OpenPopup("##graphcreate");
        }
        if (m_wireCreateRequest)
        {
            m_wireCreateRequest = false;
            m_wireActive = true;
            const ImVec2 p = ImGui::GetMousePos();
            m_graphPopupX = p.x;
            m_graphPopupY = p.y;
            ImGui::OpenPopup("##graphcreate");
        }

        // Custom-node body editor: a MODAL in suspended (screen) space -- the
        // in-node widget can only be a preview (child windows drift under the
        // canvas transform). Apply commits ONE undo step.
        if (m_bodyEditRequest != 0)
        {
            if (const Arcane::GraphNode* n = g.FindNode(m_bodyEditRequest))
            {
                m_bodyEditNode = m_bodyEditRequest;
                std::snprintf(m_bodyBuf, sizeof(m_bodyBuf), "%s", n->customBody.c_str());
                ImGui::OpenPopup("Edit HLSL##graphbody");
            }
            m_bodyEditRequest = 0;
        }
        ImGui::SetNextWindowSize(ImVec2(560.0f, 380.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Edit HLSL##graphbody", nullptr))
        {
            ImGui::TextDisabled("Function body. Inputs arrive as the node's pins; params "
                                "and Time are directly visible. End with a return.");
            ImGui::InputTextMultiline("##bodyedit", m_bodyBuf, sizeof(m_bodyBuf),
                                      ImVec2(-1.0f, ImGui::GetContentRegionAvail().y - 34.0f),
                                      ImGuiInputTextFlags_AllowTabInput);
            if (ImGui::Button("Apply"))
            {
                if (Arcane::GraphNode* n = g.FindNode(m_bodyEditNode);
                    n && n->customBody != m_bodyBuf)
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    n->customBody = m_bodyBuf;
                    m_dirty = true;
                    if (m_live)
                        RegenerateFromGraph();
                    PushGraphUndo("Edit HLSL Body", std::move(before));
                }
                m_bodyEditNode = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                m_bodyEditNode = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##graphcreate"))
        {
            // The searcher: type-to-filter, Enter creates the first match.
            if (ImGui::IsWindowAppearing())
            {
                m_createSearch[0] = '\0';
                ImGui::SetKeyboardFocusHere();
            }
            const bool enter = ImGui::InputTextWithHint(
                "##nodesearch", "Search...", m_createSearch, sizeof(m_createSearch),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Separator();

            const Arcane::GraphNodeTypeInfo* chosen = nullptr;
            const Arcane::GraphNodeTypeInfo* first = nullptr;
            for (const Arcane::GraphNodeTypeInfo& info : Arcane::AllGraphNodeInfos())
            {
                if (info.type == Arcane::GraphNodeType::Output)
                    continue;   // exactly one, seeded at creation, undeletable
                const bool spriteOnly = info.type == Arcane::GraphNodeType::VertexColor ||
                                        info.type == Arcane::GraphNodeType::SpriteTexture;
                if (spriteOnly && m_surface != 1)
                    continue;
                // Pass Input samples upstream pass outputs -- pass graphs only.
                if (info.type == Arcane::GraphNodeType::PassInput && m_activePass == 0)
                    continue;
                // Wire-invoked: only types with a pin on the wire's far side.
                // Every pin is numeric (adaptation absorbs widths), so
                // compatibility is purely structural. A fresh Custom node has
                // no inputs, so it only appears for input-side drags.
                if (m_wireActive)
                {
                    const bool hasFarPin = m_wireIsInput ? !info.outputs.empty()
                                                         : !info.inputs.empty();
                    if (!hasFarPin)
                        continue;
                }
                if (!ContainsInsensitive(info.display, m_createSearch))
                    continue;
                if (!first)
                    first = &info;
                if (ImGui::MenuItem(info.display))
                    chosen = &info;
            }
            if (enter && first)
            {
                chosen = first;
                ImGui::CloseCurrentPopup();
            }
            if (chosen)
            {
                const Arcane::GraphNodeTypeInfo& info = *chosen;
                std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                Arcane::GraphNode n;
                n.id = g.MintId();
                n.type = info.type;
                if (info.type == Arcane::GraphNodeType::ConstColor ||
                    info.type == Arcane::GraphNodeType::ConstFloat4)
                {
                    n.value[0] = n.value[1] = n.value[2] = n.value[3] = 1.0f;
                }
                if (info.type == Arcane::GraphNodeType::Param ||
                    info.type == Arcane::GraphNodeType::TextureSample)
                {
                    // Unique default name ("Param3"/"Tex3") -- dup names are a
                    // shared decl, which a fresh node should not silently join.
                    const char* stem =
                        info.type == Arcane::GraphNodeType::Param ? "Param" : "Tex";
                    std::string name;
                    for (std::uint32_t k = n.id;; ++k)
                    {
                        name = stem + std::to_string(k);
                        bool taken = false;
                        for (const Arcane::GraphNode& other : g.nodes)
                            taken = taken || other.paramName == name;
                        if (!taken)
                            break;
                    }
                    n.paramName = name;
                    n.paramType = info.type == Arcane::GraphNodeType::Param
                                      ? Arcane::MatParamType::Float
                                      : Arcane::MatParamType::Texture;
                }
                if (info.type == Arcane::GraphNodeType::Custom)
                {
                    // Magenta until written -- the classic "custom shader
                    // pending" placeholder. Bodies can read params/Time
                    // directly (they land after the cbuffer declarations).
                    n.customBody = "return float4(1.0, 0.0, 1.0, 1.0);";
                    n.customOutWidth = 4;
                }
                const ImVec2 canvasPos =
                    ed::ScreenToCanvas(ImVec2(m_graphPopupX, m_graphPopupY));
                n.posX = canvasPos.x;
                n.posY = canvasPos.y;
                ed::SetNodePosition(n.id, canvasPos);
                const std::uint32_t newId = n.id;
                g.nodes.push_back(std::move(n));
                // Wire-invoked: auto-connect the first far-side pin. Input
                // drags replace silently, exactly like a hand-drawn wire; a
                // fresh node's single edge can never cycle.
                if (m_wireActive && g.FindNode(m_wireNode))
                {
                    Arcane::GraphLink l;
                    if (m_wireIsInput)
                    {
                        std::erase_if(g.links, [&](const Arcane::GraphLink& x)
                                      { return x.toNode == m_wireNode &&
                                               x.toPin == m_wirePin; });
                        l.fromNode = newId;
                        l.fromPin = 0;
                        l.toNode = m_wireNode;
                        l.toPin = m_wirePin;
                    }
                    else
                    {
                        l.fromNode = m_wireNode;
                        l.fromPin = m_wirePin;
                        l.toNode = newId;
                        l.toPin = 0;
                    }
                    g.links.push_back(l);
                }
                m_dirty = true;
                if (m_live)
                    RegenerateFromGraph();
                PushGraphUndo("Add Node", std::move(before));
            }
            ImGui::EndPopup();
        }
        else
            m_wireActive = false;   // popup closed without a pick
        ed::Resume();

        // Position readback (skipped the seeding frame -- the canvas would
        // report pre-seed positions). Pure moves dirty the asset, no recompile.
        if (!seededThisFrame)
        {
            for (Arcane::GraphNode& n : g.nodes)
            {
                const ImVec2 p = ed::GetNodePosition(ed::NodeId(n.id));
                if (p.x != n.posX || p.y != n.posY)
                {
                    n.posX = p.x;
                    n.posY = p.y;
                    m_dirty = true;
                }
            }
        }

        ed::End();
        ed::SetCurrentEditor(nullptr);
    }

    void ShaderEditorDocument::DrawGraphNode(Arcane::GraphNode& n)
    {
        const Arcane::GraphNodeTypeInfo& info = Arcane::GraphNodeInfo(n.type);
        ed::BeginNode(ed::NodeId(n.id));
        ImGui::PushID(static_cast<int>(n.id));

        if (NodeBadged(n.id))
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "(!) %s", info.display);
        else
            ImGui::TextUnformatted(info.display);

        // Gesture helpers (used by pin rows AND payload widgets below). Value
        // drags bracket a whole-graph gesture (before on activation, one undo
        // step on release); popup-widgets (combos, color pickers) cannot live
        // inside the canvas, so types use cycle buttons.
        auto gestureBegin = [&]
        {
            if (ImGui::IsItemActivated())
                m_graphGestureBefore = ActiveGraphOpt();
        };
        auto gestureEnd = [&](const char* label)
        {
            if (ImGui::IsItemDeactivatedAfterEdit() && m_graphGestureBefore)
                PushGraphUndo(label, std::move(m_graphGestureBefore));
            else if (ImGui::IsItemDeactivated())
                m_graphGestureBefore.reset();
        };
        auto valueEdited = [&]
        {
            m_dirty = true;
            if (m_live)
                RegenerateFromGraph();
        };

        for (std::uint32_t pin = 0; pin < Arcane::GraphNodeInputCount(n); ++pin)
        {
            ed::BeginPin(InPin(n.id, pin), ed::PinKind::Input);
            ImGui::Text("-> %s", Arcane::GraphNodeInputPin(n, pin).name);
            ed::EndPin();
            // Custom pins are user-authored: width cycle + remove beside each.
            if (n.type == Arcane::GraphNodeType::Custom)
            {
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(pin));
                Arcane::GraphCustomPin& cp = n.customPins[pin];
                const char* wname = cp.width == 1 ? "f1" : cp.width == 2 ? "f2" : "f4";
                if (ImGui::SmallButton(wname))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    cp.width = cp.width == 1 ? 2 : cp.width == 2 ? 4 : 1;
                    valueEdited();
                    PushGraphUndo("Pin Width", std::move(before));
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                {
                    // Remove the pin: drop its links, re-index links to later
                    // pins (toPin is a bare index into this node's pin list).
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    Arcane::MaterialGraph& gg = *ActiveGraphOpt();
                    std::erase_if(gg.links, [&](const Arcane::GraphLink& l)
                                  { return l.toNode == n.id && l.toPin == pin; });
                    for (Arcane::GraphLink& l : gg.links)
                        if (l.toNode == n.id && l.toPin > pin)
                            --l.toPin;
                    n.customPins.erase(n.customPins.begin() + pin);
                    valueEdited();
                    PushGraphUndo("Remove Pin", std::move(before));
                    ImGui::PopID();
                    break;   // pin list changed under this loop -- redraw next frame
                }
                ImGui::PopID();
            }
        }

        switch (n.type)
        {
            case Arcane::GraphNodeType::ConstFloat:
            {
                ImGui::SetNextItemWidth(90.0f);
                const bool changed = ImGui::DragFloat("##v", &n.value[0], 0.01f);
                gestureBegin();
                if (changed) valueEdited();
                gestureEnd("Edit Value");
                break;
            }
            case Arcane::GraphNodeType::ConstFloat2:
            {
                ImGui::SetNextItemWidth(140.0f);
                const bool changed = ImGui::DragFloat2("##v", n.value, 0.01f);
                gestureBegin();
                if (changed) valueEdited();
                gestureEnd("Edit Value");
                break;
            }
            case Arcane::GraphNodeType::ConstFloat4:
            case Arcane::GraphNodeType::ConstColor:
            {
                ImGui::SetNextItemWidth(220.0f);
                const bool changed = ImGui::DragFloat4("##v", n.value, 0.01f);
                gestureBegin();
                if (changed) valueEdited();
                gestureEnd("Edit Value");
                if (n.type == Arcane::GraphNodeType::ConstColor)
                {
                    ImGui::SameLine();
                    // Preview swatch only (LINEAR floats; the full picker is a
                    // popup and popups cannot open inside the canvas).
                    ImGui::ColorButton("##swatch",
                                       ImVec4(n.value[0], n.value[1], n.value[2], n.value[3]),
                                       ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
                }
                break;
            }
            case Arcane::GraphNodeType::Param:
            case Arcane::GraphNodeType::TextureSample:
            {
                // Name: stable buffer while the InputText is active; committed
                // as ONE undoable edit on deactivate-after-edit.
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s", n.paramName.c_str());
                if (m_nameEditNode == n.id)
                    std::memcpy(buf, m_nameBuf, sizeof(buf));
                ImGui::SetNextItemWidth(110.0f);
                ImGui::InputText("##pname", buf, sizeof(buf));
                if (ImGui::IsItemActive())
                {
                    m_nameEditNode = n.id;
                    std::memcpy(m_nameBuf, buf, sizeof(m_nameBuf));
                }
                else if (m_nameEditNode == n.id)
                {
                    const bool commit = ImGui::IsItemDeactivatedAfterEdit();
                    m_nameEditNode = 0;
                    if (commit && n.paramName != m_nameBuf)
                    {
                        std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                        n.paramName = m_nameBuf;
                        valueEdited();
                        PushGraphUndo("Rename Param", std::move(before));
                    }
                }

                if (n.type == Arcane::GraphNodeType::Param)
                {
                    // Type cycle button (popup-free combo stand-in).
                    static constexpr const char* kTypeNames[] = { "float", "float2",
                                                                  "float4", "color" };
                    static constexpr Arcane::MatParamType kTypes[] = {
                        Arcane::MatParamType::Float, Arcane::MatParamType::Float2,
                        Arcane::MatParamType::Float4, Arcane::MatParamType::Color,
                    };
                    int typeIdx = 0;
                    for (int t = 0; t < 4; ++t)
                        if (kTypes[t] == n.paramType)
                            typeIdx = t;
                    if (ImGui::SmallButton(kTypeNames[typeIdx]))
                    {
                        std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                        n.paramType = kTypes[(typeIdx + 1) % 4];
                        n.paramDefault.type = n.paramType;
                        valueEdited();
                        PushGraphUndo("Param Type", std::move(before));
                    }

                    // Default value at the decl's width.
                    const int lanes =
                        static_cast<int>(Arcane::ComponentCount(n.paramType));
                    ImGui::SetNextItemWidth(lanes == 1 ? 90.0f : lanes == 2 ? 140.0f : 220.0f);
                    bool changed = false;
                    if (lanes == 1)
                        changed = ImGui::DragFloat("##pdef", &n.paramDefault.f[0], 0.01f);
                    else if (lanes == 2)
                        changed = ImGui::DragFloat2("##pdef", n.paramDefault.f, 0.01f);
                    else
                        changed = ImGui::DragFloat4("##pdef", n.paramDefault.f, 0.01f);
                    gestureBegin();
                    if (changed) valueEdited();
                    gestureEnd("Param Default");

                    bool ranged = n.hasRange;
                    if (ImGui::Checkbox("range", &ranged))
                    {
                        std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                        n.hasRange = ranged;
                        valueEdited();
                        PushGraphUndo("Param Range", std::move(before));
                    }
                    if (n.hasRange)
                    {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(120.0f);
                        float mm[2] = { n.rangeMin, n.rangeMax };
                        const bool rchanged = ImGui::DragFloat2("##prange", mm, 0.05f);
                        gestureBegin();
                        if (rchanged)
                        {
                            n.rangeMin = mm[0];
                            n.rangeMax = mm[1];
                            valueEdited();
                        }
                        gestureEnd("Param Range");
                    }
                }
                break;
            }
            case Arcane::GraphNodeType::PassInput:
            {
                // Which wired slot to sample: cycle button (validity against
                // the pass's actual wiring is codegen's job -- the badge says
                // when a slot is not wired).
                const char* slotName[4] = { "in0", "in1", "in2", "in3" };
                if (ImGui::SmallButton(slotName[n.passInputSlot %
                                                Arcane::kMaxPassInputs]))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    n.passInputSlot = (n.passInputSlot + 1) % Arcane::kMaxPassInputs;
                    valueEdited();
                    PushGraphUndo("Input Slot", std::move(before));
                }
                break;
            }
            case Arcane::GraphNodeType::Swizzle:
            {
                // Mask edit: same stable-buffer commit as param names (the
                // buffer is shared -- only one InputText is active at a time,
                // and node ids are unique across types).
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s", n.swizzleMask.c_str());
                if (m_nameEditNode == n.id)
                    std::memcpy(buf, m_nameBuf, sizeof(buf));
                ImGui::SetNextItemWidth(70.0f);
                ImGui::InputText("##mask", buf, sizeof(buf));
                if (ImGui::IsItemActive())
                {
                    m_nameEditNode = n.id;
                    std::memcpy(m_nameBuf, buf, sizeof(m_nameBuf));
                }
                else if (m_nameEditNode == n.id)
                {
                    const bool commit = ImGui::IsItemDeactivatedAfterEdit();
                    m_nameEditNode = 0;
                    if (commit && n.swizzleMask != m_nameBuf)
                    {
                        std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                        n.swizzleMask = m_nameBuf;
                        valueEdited();
                        PushGraphUndo("Edit Swizzle", std::move(before));
                    }
                }
                break;
            }
            case Arcane::GraphNodeType::Custom:
            {
                // Add-pin + output width; the pin rows above carry the per-pin
                // width/remove controls.
                if (ImGui::SmallButton("+ pin"))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    Arcane::GraphCustomPin p;
                    for (std::uint32_t k = 1;; ++k)
                    {
                        p.name = "p" + std::to_string(k);
                        bool taken = false;
                        for (const Arcane::GraphCustomPin& other : n.customPins)
                            taken = taken || other.name == p.name;
                        if (!taken)
                            break;
                    }
                    n.customPins.push_back(std::move(p));
                    valueEdited();
                    PushGraphUndo("Add Pin", std::move(before));
                }
                ImGui::SameLine();
                const char* ow = n.customOutWidth == 1 ? "out: f1"
                                : n.customOutWidth == 2 ? "out: f2" : "out: f4";
                if (ImGui::SmallButton(ow))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    n.customOutWidth = n.customOutWidth == 1 ? 2
                                       : n.customOutWidth == 2 ? 4 : 1;
                    valueEdited();
                    PushGraphUndo("Output Width", std::move(before));
                }

                // Body PREVIEW only, as plain draw-list text (an in-node
                // InputTextMultiline is a CHILD WINDOW -- it doesn't ride the
                // canvas transform, so its text drifts while the node drags and
                // ignores zoom). Editing happens in a Suspend'ed popup (normal
                // ImGui space), opened by the button below.
                {
                    std::string_view bodyText = n.customBody;
                    int shown = 0;
                    while (!bodyText.empty() && shown < 8)
                    {
                        const std::size_t nl = bodyText.find('\n');
                        std::string_view lineText = bodyText.substr(0, nl);
                        if (!lineText.empty() && lineText.back() == '\r')
                            lineText.remove_suffix(1);
                        std::string display(lineText.substr(0, 48));
                        if (lineText.size() > 48)
                            display += "...";
                        ImGui::TextDisabled("%s", display.c_str());
                        ++shown;
                        if (nl == std::string_view::npos)
                            break;
                        bodyText.remove_prefix(nl + 1);
                    }
                    if (!bodyText.empty() && shown == 8)
                        ImGui::TextDisabled("...");
                }
                if (ImGui::SmallButton("Edit HLSL..."))
                    m_bodyEditRequest = n.id;
                break;
            }
            default:
                break;
        }

        for (std::uint32_t pin = 0; pin < Arcane::GraphNodeOutputCount(n); ++pin)
        {
            ed::BeginPin(OutPin(n.id, pin), ed::PinKind::Output);
            ImGui::Text("        %s ->", Arcane::GraphNodeOutputPin(n, pin).name);
            ed::EndPin();
        }

        ImGui::PopID();
        ed::EndNode();
    }

    void ShaderEditorDocument::HandleGraphEdits()
    {
        Arcane::MaterialGraph& g = *ActiveGraphOpt();

        // Wire creation: one edge per input (silent replace), outputs fan out,
        // cycles refused silently at connect time (all SG rules). Every numeric
        // pin connects to every numeric pin -- the adaptation table absorbs
        // width differences, so validity is purely structural.
        if (ed::BeginCreate())
        {
            ed::PinId aId, bId;
            if (ed::QueryNewLink(&aId, &bId))
            {
                const DecodedPin a = DecodePin(aId);
                const DecodedPin b = DecodePin(bId);
                bool valid = a.valid && b.valid && a.isInput != b.isInput;
                const DecodedPin& out = a.isInput ? b : a;
                const DecodedPin& in = a.isInput ? a : b;
                if (valid)
                    valid = g.FindNode(out.node) && g.FindNode(in.node) &&
                            !WouldCycle(g, out.node, in.node);
                if (!valid)
                    ed::RejectNewItem();
                else if (ed::AcceptNewItem())
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    std::erase_if(g.links, [&](const Arcane::GraphLink& l)
                                  { return l.toNode == in.node && l.toPin == in.pin; });
                    Arcane::GraphLink l;
                    l.fromNode = out.node;
                    l.fromPin = out.pin;
                    l.toNode = in.node;
                    l.toPin = in.pin;
                    g.links.push_back(l);
                    m_dirty = true;
                    if (m_live)
                        RegenerateFromGraph();
                    PushGraphUndo("Connect", std::move(before));
                }
            }
            else if (ed::QueryNewNode(&aId))
            {
                // Wire released over empty canvas -> the create searcher.
                // Popups cannot open here (canvas space); stash the dragged
                // pin and let DrawGraphPanel's Suspend block open it.
                const DecodedPin from = DecodePin(aId);
                if (!from.valid || !g.FindNode(from.node))
                    ed::RejectNewItem();
                else if (ed::AcceptNewItem())
                {
                    m_wireNode = from.node;
                    m_wirePin = from.pin;
                    m_wireIsInput = from.isInput;
                    m_wireCreateRequest = true;
                }
            }
        }
        // UNCONDITIONAL: CreateItemAction::Begin() arms m_InActive even when it
        // returns false (idle frame); a skipped EndCreate() asserts on the NEXT
        // frame's BeginCreate() (the desk crash -- frame 1 fine, frame 2 abort).
        ed::EndCreate();

        // Deletion (multi-select = one undo step). Link ids are this frame's
        // indices -- collect first, erase in descending order after the
        // queries. The Output node refuses deletion (SG: blocks are fixed).
        std::vector<std::size_t> linkIdxs;
        std::vector<std::uint32_t> nodeIds;
        if (ed::BeginDelete())
        {
            ed::LinkId lid;
            while (ed::QueryDeletedLink(&lid))
            {
                const std::size_t idx = static_cast<std::size_t>(lid.Get()) - 1;
                if (idx < g.links.size() && ed::AcceptDeletedItem())
                    linkIdxs.push_back(idx);
                else if (idx >= g.links.size())
                    ed::RejectDeletedItem();
            }
            ed::NodeId nid;
            while (ed::QueryDeletedNode(&nid))
            {
                const std::uint32_t id = static_cast<std::uint32_t>(nid.Get());
                const Arcane::GraphNode* n = g.FindNode(id);
                if (!n || n->type == Arcane::GraphNodeType::Output)
                {
                    ed::RejectDeletedItem();
                    continue;
                }
                if (ed::AcceptDeletedItem())
                    nodeIds.push_back(id);
            }
        }
        // UNCONDITIONAL for the same reason as EndCreate() above.
        ed::EndDelete();

        if (!linkIdxs.empty() || !nodeIds.empty())
        {
            std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
            std::sort(linkIdxs.rbegin(), linkIdxs.rend());
            for (std::size_t idx : linkIdxs)
                g.links.erase(g.links.begin() + static_cast<std::ptrdiff_t>(idx));
            for (std::uint32_t id : nodeIds)
            {
                std::erase_if(g.nodes, [&](const Arcane::GraphNode& n)
                              { return n.id == id; });
                std::erase_if(g.links, [&](const Arcane::GraphLink& l)
                              { return l.fromNode == id || l.toNode == id; });
            }
            m_dirty = true;
            if (m_live)
                RegenerateFromGraph();
            PushGraphUndo("Delete", std::move(before));
        }

        // Copy/paste/cut/duplicate: ed's shortcut actions (canvas focus only).
        // WantTextInput keeps in-canvas text edits (param names, masks) from
        // being hijacked. Cut deletes through ed::DeleteNode so the delete
        // pass above owns the model erase + its undo step (next frame).
        if (ed::BeginShortcut())
        {
            if (!ImGui::GetIO().WantTextInput)
            {
                if (ed::AcceptCopy())
                {
                    const std::string clip = BuildGraphClipJson();
                    if (!clip.empty())
                        ImGui::SetClipboardText(clip.c_str());
                }
                else if (ed::AcceptCut())
                {
                    const std::string clip = BuildGraphClipJson();
                    if (!clip.empty())
                    {
                        ImGui::SetClipboardText(clip.c_str());
                        std::vector<ed::NodeId> sel(
                            static_cast<std::size_t>(std::max(0, ed::GetSelectedObjectCount())));
                        const int count = sel.empty() ? 0
                            : ed::GetSelectedNodes(sel.data(), static_cast<int>(sel.size()));
                        for (int i = 0; i < count; ++i)
                            ed::DeleteNode(sel[static_cast<std::size_t>(i)]);
                    }
                }
                else if (ed::AcceptPaste())
                    PasteGraphClipText(ImGui::GetClipboardText());
                else if (ed::AcceptDuplicate())
                {
                    const std::string clip = BuildGraphClipJson();
                    if (!clip.empty())
                        PasteGraphClipText(clip.c_str());
                }
            }
            ed::EndShortcut();
        }
    }

    std::string ShaderEditorDocument::BuildGraphClipJson()
    {
        const Arcane::MaterialGraph& g = *ActiveGraphOpt();
        std::vector<ed::NodeId> sel(
            static_cast<std::size_t>(std::max(0, ed::GetSelectedObjectCount())));
        if (sel.empty())
            return {};
        const int count = ed::GetSelectedNodes(sel.data(), static_cast<int>(sel.size()));

        Arcane::MaterialGraph sub;
        std::unordered_set<std::uint32_t> picked;
        for (int i = 0; i < count; ++i)
        {
            const std::uint32_t id =
                static_cast<std::uint32_t>(sel[static_cast<std::size_t>(i)].Get());
            const Arcane::GraphNode* node = g.FindNode(id);
            if (!node || node->type == Arcane::GraphNodeType::Output)
                continue;   // the one fixed node never travels
            if (picked.insert(id).second)
                sub.nodes.push_back(*node);
        }
        if (sub.nodes.empty())
            return {};
        // Internal links only -- both endpoints in the selection.
        for (const Arcane::GraphLink& l : g.links)
            if (picked.count(l.fromNode) && picked.count(l.toNode))
                sub.links.push_back(l);

        nlohmann::json j = Arcane::GraphToJson(sub);
        j["kind"] = "arcane-graph-clip";
        return j.dump();
    }

    void ShaderEditorDocument::PasteGraphClipText(const char* text)
    {
        if (!text || !*text)
            return;
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object() ||
            j.value("kind", std::string()) != "arcane-graph-clip")
            return;   // foreign clipboard content -- not ours, ignore silently
        const std::optional<Arcane::MaterialGraph> sub = Arcane::GraphFromJson(j);
        if (!sub)
            return;

        // Recenter the subgraph on the mouse (canvas space).
        float cx = 0.0f, cy = 0.0f;
        int count = 0;
        for (const Arcane::GraphNode& n : sub->nodes)
            if (n.type != Arcane::GraphNodeType::Output)
            {
                cx += n.posX;
                cy += n.posY;
                ++count;
            }
        if (count == 0)
            return;
        cx /= static_cast<float>(count);
        cy /= static_cast<float>(count);
        const ImVec2 at = ed::ScreenToCanvas(ImGui::GetMousePos());

        Arcane::MaterialGraph& g = *ActiveGraphOpt();
        std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
        std::unordered_map<std::uint32_t, std::uint32_t> remap;
        ed::ClearSelection();
        for (const Arcane::GraphNode& src : sub->nodes)
        {
            if (src.type == Arcane::GraphNodeType::Output)
                continue;
            Arcane::GraphNode n = src;
            n.id = g.MintId();   // FRESH ids -- clip ids may collide or be stale
            remap[src.id] = n.id;
            n.posX += at.x - cx;
            n.posY += at.y - cy;
            ed::SetNodePosition(n.id, ImVec2(n.posX, n.posY));
            ed::SelectNode(n.id, true);   // the paste becomes the selection
            g.nodes.push_back(std::move(n));
        }
        for (const Arcane::GraphLink& l : sub->links)
        {
            const auto f = remap.find(l.fromNode);
            const auto t = remap.find(l.toNode);
            if (f == remap.end() || t == remap.end())
                continue;   // endpoint did not paste
            Arcane::GraphLink nl;
            nl.fromNode = f->second;
            nl.fromPin = l.fromPin;
            nl.toNode = t->second;
            nl.toPin = l.toPin;
            g.links.push_back(nl);
        }
        m_dirty = true;
        if (m_live)
            RegenerateFromGraph();
        PushGraphUndo("Paste", std::move(before));
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

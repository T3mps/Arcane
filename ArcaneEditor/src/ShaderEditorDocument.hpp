#pragma once

// ShaderEditorDocument: the shader editor MVP (Slice 5) -- the first real
// EditorDocument. One window, four panels over one .armat asset:
//   snippet text (InputTextMultiline + click-to-jump via the input callback)
//   live preview (own OffscreenCanvas + FullscreenMaterialPass, animating Time)
//   params (auto-widgets from the //@param decls; edits write the CB LIVE, no
//           recompile, and land on the shared CommandStack as undo steps)
//   errors (structured ShaderDiag list; click jumps the text cursor)
// Live loop: a text edit resubmits both stages through the app-shared
// ShaderCompiler (the service's per-key debounce coalesces keystrokes); the
// LAST-GOOD pipeline keeps rendering while a compile is in flight or failing.
// EditorApp drains the service once per frame and offers each result to every
// document via ConsumeResult.

#include "EditorDocument.hpp"

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/FullscreenMaterialPass.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Arcane
{
    class Assets;
    class CommandStack;
    class ShaderLibrary;
}

namespace Arcane::Editor
{
    // Everything a document borrows from the app (all outlive the host's
    // document list -- see EditorApp member ordering).
    struct DocServices
    {
        nvrhi::IDevice*               device = nullptr;
        Arcane::ShaderLibrary*        shaders = nullptr;    // OffscreenCanvas tonemap
        Arcane::ShaderCompiler*       compiler = nullptr;   // app-shared service
        Arcane::ShaderSourceProvider* sources = nullptr;    // template text
        Arcane::Assets*               assets = nullptr;     // texture params by Guid
        Arcane::CommandStack*         undo = nullptr;       // the ONE undo history
        const double*                 clock = nullptr;      // app compile clock (Poll's `now`)
        Arcane::GraphicsBackend       backend{};
    };

    class ShaderEditorDocument final : public EditorDocument
    {
    public:
        ShaderEditorDocument(DocServices services, std::filesystem::path path,
                             Arcane::MaterialAssetData data);

        const std::string& Title() const override { return m_title; }
        Arcane::Guid AssetGuid() const override { return m_data.id; }
        bool Dirty() const override { return m_dirty; }
        bool Save() override;
        void Tick(double dt) override;
        void Draw(bool& requestClose) override;

        // True when the result belonged to this document's in-flight compiles.
        bool ConsumeResult(const Arcane::ShaderCompileResult& result);

    private:
        double Now() const { return m_services.clock ? *m_services.clock : 0.0; }
        void   Rebuild();          // parse + stitch + submit both stages (structural edit)
        void   BindIfComplete();   // both stages landed -> createShader + SetMaterial
        bool   HasErrors() const;

        void DrawToolbar();
        void DrawSnippetEditor(float height);
        void DrawErrorsPanel();
        void DrawPreviewPanel(float height);
        void DrawParamsPanel();

        DocServices                     m_services;
        std::filesystem::path           m_path;
        Arcane::MaterialAssetData       m_data;      // id/name/kind (+ save target)
        std::string                     m_title;     // display name
        std::string                     m_windowLabel;   // display###stable-guid-id
        std::string                     m_snippet;   // the live edit buffer
        bool                            m_dirty = false;
        bool                            m_live = true;    // auto-compile on edit
        bool                            m_confirmSaveWithErrors = false;

        // Authoring state: bound = what the pass renders (last-good); pending =
        // the latest Rebuild, promoted by BindIfComplete on dual-stage success.
        std::shared_ptr<Arcane::MaterialTemplate>  m_pendingTemplate;
        std::shared_ptr<Arcane::MaterialTemplate>  m_boundTemplate;
        std::shared_ptr<Arcane::MaterialInstance>  m_instance;   // over m_boundTemplate
        std::vector<Arcane::ParamMeta>             m_metas;      // parallel to pending->Params()
        std::vector<Arcane::ParamMeta>             m_boundMetas; // parallel to bound->Params()
        std::vector<std::string>                   m_parseErrors;
        std::vector<Arcane::ShaderDiag>            m_diags;      // last compile (active backend)

        std::unique_ptr<Arcane::OffscreenCanvas>        m_preview;
        std::unique_ptr<Arcane::FullscreenMaterialPass> m_pass;

        std::uint64_t m_vsJob = 0, m_psJob = 0;      // in-flight ids (0 = none)
        std::vector<std::uint8_t> m_vsBytes, m_psBytes;
        double m_animTime = 0.0;                     // preview Time uniform
        int    m_jumpToLine = 0;                     // 1-based; 0 = no pending jump
        bool   m_focusSnippet = false;               // focus the input so the jump lands

        friend struct SnippetCallbackForwarder;
    };
}

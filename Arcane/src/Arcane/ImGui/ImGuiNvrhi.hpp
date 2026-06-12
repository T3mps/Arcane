#pragma once

// First-party Dear ImGui renderer backend on NVRHI (the 1.92
// ImGuiBackendFlags_RendererHasTextures protocol; reference:
// ThirdParty/imgui/backends/imgui_impl_dx11.cpp). Engine-internal -- not
// exported; ImGuiLayer is the exported facade. Renders ImGui draw data
// into a caller-provided display-referred framebuffer through an OPEN
// command list (no ICommandList wrapping, no manual barriers).

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

struct ImDrawData;
struct ImTextureData;

namespace Arcane
{
    class ShaderLibrary;

    class ImGuiNvrhiRenderer
    {
    public:
        // Sets the renderer backend flags/name on the current ImGui context,
        // creates the sampler/layouts/input layout. Returns false (logged)
        // when the imgui shaders are missing or device objects fail.
        bool Init(nvrhi::IDevice* device, ShaderLibrary& shaders);

        // Destroys ImTextureData-owned textures (the dx11 reference's
        // InvalidateDeviceObjects shutdown path) and releases handles.
        void Shutdown();

        void RenderDrawData(ImDrawData* drawData,
                            nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* target);

    private:
        nvrhi::IGraphicsPipeline* GetPipeline(nvrhi::IFramebuffer* target);
        nvrhi::IBindingSet*       GetBindingSet(nvrhi::ITexture* texture);
        void UpdateTexture(ImTextureData* tex, nvrhi::ICommandList* commandList);
        void DestroyTexture(ImTextureData* tex);

        nvrhi::IDevice* m_device = nullptr;
        ShaderLibrary*  m_shaders = nullptr;

        nvrhi::SamplerHandle       m_sampler;
        nvrhi::BindingLayoutHandle m_bindingLayout;
        nvrhi::InputLayoutHandle   m_inputLayout;

        nvrhi::BufferHandle m_vertexBuffer;
        nvrhi::BufferHandle m_indexBuffer;
        size_t m_vertexCapacity = 0;   // in ImDrawVert
        size_t m_indexCapacity  = 0;   // in ImDrawIdx

        // CPU scratch for the concatenated vertex/index upload.
        std::vector<uint8_t> m_vtxScratch;
        std::vector<uint8_t> m_idxScratch;

        // Pipeline cache keyed by framebuffer hash; rebuilt on shader reload.
        std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_pipelines;
        uint64_t m_pipelineGeneration = 0;

        // Per-ITexture* binding-set cache (one set per atlas/user texture).
        // ABA safety is load-bearing: DestroyTexture evicts the cache entry
        // BEFORE releasing the texture handle, and a cached binding set pins
        // its texture -- so a live ITexture* key is always unique.
        std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_bindingSets;

        // ImTextureData-owned GPU textures (font atlas + any user textures).
        std::unordered_map<ImTextureData*, nvrhi::TextureHandle> m_textures;
    };
}

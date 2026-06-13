#include <Arcane/ImGui/ImGuiNvrhi.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <functional>

namespace Arcane
{
    namespace
    {
        // 16-byte push constant block matching imgui.hlsl (scale + translate).
        struct ImGuiPushConstants
        {
            float scale[2];
            float translate[2];
        };
        static_assert(sizeof(ImGuiPushConstants) == 16,
                      "imgui push constants are 16 bytes (scale + translate)");

        // ImDrawVert is float2 pos, float2 uv, ImU32 col = 20 bytes; the
        // input layout below depends on it exactly.
        static_assert(sizeof(ImDrawVert) == 20,
                      "ImDrawVert layout assumed by the imgui input layout");
        // ImDrawIdx is 16-bit -> R16_UINT index buffer.
        static_assert(sizeof(ImDrawIdx) == 2,
                      "ImDrawIdx assumed 16-bit (R16_UINT index buffer)");
    }

    bool ImGuiNvrhiRenderer::Init(nvrhi::IDevice* device, ShaderLibrary& shaders)
    {
        m_device  = device;
        m_shaders = &shaders;

        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = "imgui_impl_nvrhi";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

        auto samplerDesc = nvrhi::SamplerDesc()
            .setAllFilters(true)  // bilinear: ImGui default
            .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        m_sampler = m_device->createSampler(samplerDesc);

        auto layoutDesc = nvrhi::BindingLayoutDesc()
            .setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(
                0, sizeof(ImGuiPushConstants)))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Sampler(0));
        m_bindingLayout = m_device->createBindingLayout(layoutDesc);

        nvrhi::ShaderHandle vs =
            m_shaders->Get("imgui_vs", nvrhi::ShaderType::Vertex);
        if (!vs)
            return false;

        const nvrhi::VertexAttributeDesc attributes[] = {
            nvrhi::VertexAttributeDesc()
                .setName("POSITION")
                .setFormat(nvrhi::Format::RG32_FLOAT)
                .setOffset(offsetof(ImDrawVert, pos))
                .setElementStride(sizeof(ImDrawVert)),
            nvrhi::VertexAttributeDesc()
                .setName("TEXCOORD")
                .setFormat(nvrhi::Format::RG32_FLOAT)
                .setOffset(offsetof(ImDrawVert, uv))
                .setElementStride(sizeof(ImDrawVert)),
            nvrhi::VertexAttributeDesc()
                .setName("COLOR")
                .setFormat(nvrhi::Format::RGBA8_UNORM)
                .setOffset(offsetof(ImDrawVert, col))
                .setElementStride(sizeof(ImDrawVert)),
        };
        m_inputLayout = m_device->createInputLayout(
            attributes, (uint32_t)std::size(attributes), vs);

        if (!m_sampler || !m_bindingLayout || !m_inputLayout ||
            !m_shaders->Get("imgui_ps", nvrhi::ShaderType::Pixel))
        {
            ARC_ERROR("ImGuiNvrhiRenderer::Init failed (shaders or device objects)");
            return false;
        }
        return true;
    }

    void ImGuiNvrhiRenderer::DestroyTexture(ImTextureData* tex)
    {
        auto it = m_textures.find(tex);
        if (it != m_textures.end())
        {
            // Drop the cached binding set referencing this texture, then the
            // texture handle itself (NVRHI ref-counts the GPU resource).
            m_bindingSets.erase(it->second.Get());
            m_textures.erase(it);
        }
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }

    void ImGuiNvrhiRenderer::UpdateTexture(ImTextureData* tex,
                                           nvrhi::ICommandList* commandList)
    {
        if (tex->Status == ImTextureStatus_WantCreate)
        {
            // RGBA8_UNORM (NOT sRGB): ImGui colors are display-referred and
            // the target is the display-referred backbuffer. writeTexture +
            // keepInitialState handle barriers; no manual transitions.
            auto desc = nvrhi::TextureDesc()
                .setWidth((uint32_t)tex->Width)
                .setHeight((uint32_t)tex->Height)
                .setFormat(nvrhi::Format::RGBA8_UNORM)
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setKeepInitialState(true)
                .setDebugName("ImGuiTexture");
            nvrhi::TextureHandle handle = m_device->createTexture(desc);

            commandList->writeTexture(handle, 0, 0, tex->GetPixels(),
                                      (size_t)tex->GetPitch());

            tex->SetTexID((ImTextureID)(intptr_t)handle.Get());
            m_textures[tex] = handle;
            tex->SetStatus(ImTextureStatus_OK);
        }
        else if (tex->Status == ImTextureStatus_WantUpdates)
        {
            // Full re-upload from the CPU pixel buffer (region staging is a
            // later optimization; writeTexture is whole-subresource).
            auto it = m_textures.find(tex);
            if (it != m_textures.end())
                commandList->writeTexture(it->second, 0, 0, tex->GetPixels(),
                                          (size_t)tex->GetPitch());
            tex->SetStatus(ImTextureStatus_OK);
        }

        if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
            DestroyTexture(tex);
    }

    void ImGuiNvrhiRenderer::RenderDrawData(ImDrawData* drawData,
                                            nvrhi::ICommandList* commandList,
                                            nvrhi::IFramebuffer* target)
    {
        if (!drawData || drawData->DisplaySize.x <= 0.0f ||
            drawData->DisplaySize.y <= 0.0f)
            return;

        // 1) Texture protocol: catch up on creates/updates/destroys.
        if (drawData->Textures != nullptr)
            for (ImTextureData* tex : *drawData->Textures)
                if (tex->Status != ImTextureStatus_OK)
                    UpdateTexture(tex, commandList);

        if (drawData->TotalVtxCount <= 0)
            return;

        // 2) Grow-only VB/IB; concatenate all cmd lists into CPU scratch.
        const size_t vtxNeeded = (size_t)drawData->TotalVtxCount;
        const size_t idxNeeded = (size_t)drawData->TotalIdxCount;
        if (!m_vertexBuffer || m_vertexCapacity < vtxNeeded)
        {
            m_vertexCapacity = vtxNeeded + 5000;
            m_vertexBuffer = m_device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(m_vertexCapacity * sizeof(ImDrawVert))
                .setIsVertexBuffer(true)
                .setInitialState(nvrhi::ResourceStates::VertexBuffer)
                .setKeepInitialState(true)
                .setDebugName("ImGui.VB"));
        }
        if (!m_indexBuffer || m_indexCapacity < idxNeeded)
        {
            m_indexCapacity = idxNeeded + 10000;
            m_indexBuffer = m_device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(m_indexCapacity * sizeof(ImDrawIdx))
                .setIsIndexBuffer(true)
                .setInitialState(nvrhi::ResourceStates::IndexBuffer)
                .setKeepInitialState(true)
                .setDebugName("ImGui.IB"));
        }

        m_vtxScratch.resize(vtxNeeded * sizeof(ImDrawVert));
        m_idxScratch.resize(idxNeeded * sizeof(ImDrawIdx));
        {
            uint8_t* vtxDst = m_vtxScratch.data();
            uint8_t* idxDst = m_idxScratch.data();
            for (const ImDrawList* list : drawData->CmdLists)
            {
                const size_t vbytes = (size_t)list->VtxBuffer.Size * sizeof(ImDrawVert);
                const size_t ibytes = (size_t)list->IdxBuffer.Size * sizeof(ImDrawIdx);
                std::memcpy(vtxDst, list->VtxBuffer.Data, vbytes);
                std::memcpy(idxDst, list->IdxBuffer.Data, ibytes);
                vtxDst += vbytes;
                idxDst += ibytes;
            }
        }
        commandList->writeBuffer(m_vertexBuffer, m_vtxScratch.data(),
                                 m_vtxScratch.size());
        commandList->writeBuffer(m_indexBuffer, m_idxScratch.data(),
                                 m_idxScratch.size());

        // 3) Push constants: the VS outputs pos*scale + translate directly,
        // so scale.y is NEGATIVE (ImGui's y-down -> clip y-up). DisplayPos is
        // (0,0) for single-viewport apps but handled for generality.
        const float L = drawData->DisplayPos.x;
        const float T = drawData->DisplayPos.y;
        ImGuiPushConstants push{};
        push.scale[0] =  2.0f / drawData->DisplaySize.x;
        push.scale[1] = -2.0f / drawData->DisplaySize.y;
        push.translate[0] = -1.0f - L * push.scale[0];
        push.translate[1] =  1.0f - T * push.scale[1];

        nvrhi::IGraphicsPipeline* pipeline = GetPipeline(target);
        if (!pipeline)
            return;

        const nvrhi::FramebufferInfoEx& fbInfo = target->getFramebufferInfo();
        const nvrhi::Viewport fullViewport = fbInfo.getViewport();

        const ImVec2 clipOff   = drawData->DisplayPos;
        const ImVec2 clipScale = drawData->FramebufferScale;

        // 4) Per ImDrawList, per ImDrawCmd.
        uint32_t globalVtxOffset = 0;
        uint32_t globalIdxOffset = 0;
        for (const ImDrawList* list : drawData->CmdLists)
        {
            for (int ci = 0; ci < list->CmdBuffer.Size; ++ci)
            {
                const ImDrawCmd* cmd = &list->CmdBuffer[ci];
                // v1 does not register user callbacks; assert none slipped in.
                IM_ASSERT(cmd->UserCallback == nullptr &&
                          "ImGuiNvrhiRenderer: user draw callbacks unsupported");

                // Clip rect into framebuffer space; skip degenerate.
                const float clipMinX = (cmd->ClipRect.x - clipOff.x) * clipScale.x;
                const float clipMinY = (cmd->ClipRect.y - clipOff.y) * clipScale.y;
                const float clipMaxX = (cmd->ClipRect.z - clipOff.x) * clipScale.x;
                const float clipMaxY = (cmd->ClipRect.w - clipOff.y) * clipScale.y;
                if (clipMaxX <= clipMinX || clipMaxY <= clipMinY)
                    continue;

                nvrhi::ITexture* texture =
                    (nvrhi::ITexture*)cmd->GetTexID();

                auto state = nvrhi::GraphicsState()
                    .setPipeline(pipeline)
                    .setFramebuffer(target)
                    .addBindingSet(GetBindingSet(texture))
                    .setIndexBuffer({ m_indexBuffer, nvrhi::Format::R16_UINT, 0 })
                    .addVertexBuffer({ m_vertexBuffer, 0, 0 });
                // One full-target viewport; per-cmd scissor. nvrhi::Rect is
                // (minX, maxX, minY, maxY) -- verified in nvrhi.h.
                state.viewport.addViewport(fullViewport);
                state.viewport.addScissorRect(nvrhi::Rect(
                    (int)clipMinX, (int)clipMaxX, (int)clipMinY, (int)clipMaxY));

                commandList->setGraphicsState(state);
                commandList->setPushConstants(&push, sizeof(push));
                commandList->drawIndexed(nvrhi::DrawArguments()
                    .setVertexCount(cmd->ElemCount)
                    .setStartIndexLocation(globalIdxOffset + cmd->IdxOffset)
                    .setStartVertexLocation(globalVtxOffset + cmd->VtxOffset));
            }
            globalVtxOffset += (uint32_t)list->VtxBuffer.Size;
            globalIdxOffset += (uint32_t)list->IdxBuffer.Size;
        }
    }

    nvrhi::IBindingSet* ImGuiNvrhiRenderer::GetBindingSet(nvrhi::ITexture* texture)
    {
        nvrhi::BindingSetHandle& set = m_bindingSets[texture];
        if (!set)
        {
            set = m_device->createBindingSet(
                nvrhi::BindingSetDesc()
                    .addItem(nvrhi::BindingSetItem::PushConstants(
                        0, sizeof(ImGuiPushConstants)))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture))
                    .addItem(nvrhi::BindingSetItem::Sampler(0, m_sampler)),
                m_bindingLayout);
        }
        return set;
    }

    nvrhi::IGraphicsPipeline* ImGuiNvrhiRenderer::GetPipeline(
        nvrhi::IFramebuffer* target)
    {
        if (m_pipelineGeneration != m_shaders->Generation())
        {
            m_pipelines.clear();
            m_pipelineGeneration = m_shaders->Generation();
        }

        const nvrhi::FramebufferInfoEx& fbInfo = target->getFramebufferInfo();
        const size_t key = std::hash<nvrhi::FramebufferInfo>{}(fbInfo);

        nvrhi::GraphicsPipelineHandle& pipeline = m_pipelines[key];
        if (!pipeline)
        {
            nvrhi::ShaderHandle vs =
                m_shaders->Get("imgui_vs", nvrhi::ShaderType::Vertex);
            nvrhi::ShaderHandle ps =
                m_shaders->Get("imgui_ps", nvrhi::ShaderType::Pixel);
            if (!vs || !ps)
            {
                ARC_ERROR("ImGui shaders unavailable");
                return nullptr;
            }

            nvrhi::BlendState::RenderTarget blend;
            blend.enableBlend()
                .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);

            auto desc = nvrhi::GraphicsPipelineDesc()
                .setVertexShader(vs)
                .setPixelShader(ps)
                .setInputLayout(m_inputLayout)
                .addBindingLayout(m_bindingLayout);
            desc.primType = nvrhi::PrimitiveType::TriangleList;
            desc.renderState.rasterState.setCullNone();
            // ImGui relies on per-cmd dynamic scissor; enable it on the raster
            // state so the scissor rects we set actually clip.
            desc.renderState.rasterState.enableScissor();
            desc.renderState.depthStencilState.disableDepthTest();
            desc.renderState.depthStencilState.disableStencil();
            desc.renderState.blendState.setRenderTarget(0, blend);
            pipeline = m_device->createGraphicsPipeline(desc, fbInfo);
        }
        return pipeline;
    }

    void ImGuiNvrhiRenderer::Shutdown()
    {
        // Mirror the dx11 reference InvalidateDeviceObjects shutdown path
        // (imgui_impl_dx11.cpp): iterate the platform texture list rather than
        // only our own m_textures map. This catches any ImTextureData entries
        // that were never serviced -- e.g. a stuck WantCreate that arrived after
        // the last RenderDrawData, or a WantDestroy with UnusedFrames==0 -- and
        // leaves their status as Destroyed so a later re-Init is clean.
        //
        // Guard: the dx11 reference checks RefCount == 1 (only the platform list
        // holds a ref). We replicate that intent: only destroy textures this
        // backend owns. DestroyTexture is safe on a texture absent from m_textures
        // (an unserviced WantCreate has no handle to evict) -- it tolerates that.
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
            if (tex->RefCount == 1)
                DestroyTexture(tex);

        m_textures.clear();
        m_bindingSets.clear();
        m_pipelines.clear();
        m_vertexBuffer = nullptr;
        m_indexBuffer = nullptr;
        m_inputLayout = nullptr;
        m_bindingLayout = nullptr;
        m_sampler = nullptr;
    }
}

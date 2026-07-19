#include <Arcane/Render/PickBuffer.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

namespace Arcane
{
    namespace
    {
        // The id target is a single-channel 32-bit UINT: 0 == background, k ==
        // the k-th drawn silhouette's hit-proxy id (id = drawable index + 1).
        // Integer format so the PS writes an exact id (no float rounding) and
        // clearTextureUInt zeroes it between picks.
        constexpr nvrhi::Format kIdFormat = nvrhi::Format::R32_UINT;

        class PickBufferImpl final : public PickBuffer
        {
        public:
            PickBufferImpl(nvrhi::IDevice* device, ShaderLibrary& shaders)
                : m_device(device), m_shaders(shaders) {}

            bool Init(uint32_t width, uint32_t height)
            {
                m_commandList = m_device->createCommandList();
                if (!m_commandList)
                {
                    ARC_ERROR("PickBuffer: command list creation failed");
                    return false;
                }

                // 1x1 readback staging: Pick() copies a single pixel under the
                // cursor into it, so it is size-independent and never rebuilds on
                // Resize (the target does).
                auto stagingDesc = nvrhi::TextureDesc()
                    .setWidth(1).setHeight(1)
                    .setFormat(kIdFormat)
                    .setDebugName("PickBuffer.Readback");
                m_staging = m_device->createStagingTexture(
                    stagingDesc, nvrhi::CpuAccessMode::Read);
                if (!m_staging)
                {
                    ARC_ERROR("PickBuffer: staging texture creation failed");
                    return false;
                }

                return BuildTarget(width, height);
            }

            void Resize(uint32_t width, uint32_t height) override
            {
                if (width == 0 || height == 0 ||
                    (width == m_width && height == m_height))
                    return;

                // The caller owns frame pacing; tearing down a target the GPU may
                // still read requires an idle (OffscreenCanvas::Resize does the
                // same).
                m_device->waitForIdle();
                m_targetFb = nullptr;
                m_target   = nullptr;
                m_device->runGarbageCollection();
                BuildTarget(width, height);
            }

            uint32_t Width()  const override { return m_width;  }
            uint32_t Height() const override { return m_height; }

        private:
            bool BuildTarget(uint32_t width, uint32_t height)
            {
                // R32_UINT render target: the id pass writes per-instance ids
                // here; Pick() copies 1 pixel out. KeepInitialState lets NVRHI
                // auto-transition between RenderTarget and CopySource.
                auto desc = nvrhi::TextureDesc()
                    .setWidth(width).setHeight(height)
                    .setFormat(kIdFormat)
                    .setIsRenderTarget(true)
                    .setInitialState(nvrhi::ResourceStates::RenderTarget)
                    .setKeepInitialState(true)
                    .setDebugName("PickBuffer.IdTarget");
                m_target = m_device->createTexture(desc);
                if (!m_target)
                {
                    ARC_ERROR("PickBuffer: id target creation failed ({}x{})",
                              width, height);
                    return false;
                }

                m_targetFb = m_device->createFramebuffer(
                    nvrhi::FramebufferDesc().addColorAttachment(m_target));
                if (!m_targetFb)
                {
                    ARC_ERROR("PickBuffer: id framebuffer creation failed");
                    return false;
                }

                m_width  = width;
                m_height = height;
                return true;
            }

            nvrhi::IDevice*             m_device = nullptr;
            ShaderLibrary&              m_shaders;      // for the Task 3 id pipeline
            nvrhi::TextureHandle        m_target;       // R32_UINT id render target
            nvrhi::FramebufferHandle    m_targetFb;
            nvrhi::StagingTextureHandle m_staging;      // 1x1 readback
            nvrhi::CommandListHandle    m_commandList;
            uint32_t                    m_width  = 0;
            uint32_t                    m_height = 0;
        };
    }

    std::unique_ptr<PickBuffer> PickBuffer::Create(
        nvrhi::IDevice* device, ShaderLibrary& shaders,
        uint32_t width, uint32_t height)
    {
        if (!device || width == 0 || height == 0)
        {
            ARC_ERROR("PickBuffer::Create: bad args ({}x{})", width, height);
            return nullptr;
        }

        auto pb = std::make_unique<PickBufferImpl>(device, shaders);
        if (!pb->Init(width, height))
            return nullptr;
        return pb;
    }
}

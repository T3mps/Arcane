#include <Arcane/Render/Canvas.hpp>

#include <Arcane/Base/Log.hpp>

namespace Arcane
{
    namespace
    {
        constexpr nvrhi::Format kCanvasFormat = nvrhi::Format::RGBA16_FLOAT;

        class CanvasImpl final : public Canvas
        {
        public:
            explicit CanvasImpl(nvrhi::IDevice* device) : m_device(device) {}

            bool Init(uint32_t width, uint32_t height)
            {
                m_width  = width;
                m_height = height;
                auto desc = nvrhi::TextureDesc()
                    .setWidth(width)
                    .setHeight(height)
                    .setFormat(kCanvasFormat)
                    .setIsRenderTarget(true)
                    .setInitialState(nvrhi::ResourceStates::RenderTarget)
                    .setKeepInitialState(true)
                    .setDebugName("Canvas");
                m_texture = m_device->createTexture(desc);
                if (!m_texture)
                {
                    ARC_ERROR("Canvas texture creation failed ({}x{})", width, height);
                    return false;
                }
                m_framebuffer = m_device->createFramebuffer(
                    nvrhi::FramebufferDesc().addColorAttachment(m_texture));
                if (!m_framebuffer)
                {
                    ARC_ERROR("Canvas framebuffer creation failed");
                    return false;
                }
                return true;
            }

            nvrhi::ITexture*     Texture()     const override { return m_texture;     }
            nvrhi::IFramebuffer* Framebuffer() const override { return m_framebuffer; }
            uint32_t Width()  const override { return m_width;  }
            uint32_t Height() const override { return m_height; }

            void Resize(uint32_t width, uint32_t height) override
            {
                if ((width == m_width && height == m_height) ||
                    width == 0 || height == 0)
                    return;
                // The caller owns frame pacing; a resize mid-flight must not
                // free a texture the GPU still reads.
                m_device->waitForIdle();
                m_framebuffer = nullptr;
                m_texture     = nullptr;
                m_device->runGarbageCollection();
                Init(width, height);
            }

        private:
            nvrhi::IDevice*       m_device;
            nvrhi::TextureHandle     m_texture;
            nvrhi::FramebufferHandle m_framebuffer;
            uint32_t m_width  = 0;
            uint32_t m_height = 0;
        };
    }

    std::unique_ptr<Canvas> CreateCanvas(nvrhi::IDevice* device,
                                         uint32_t width, uint32_t height)
    {
        auto canvas = std::make_unique<CanvasImpl>(device);
        if (!canvas->Init(width, height))
            return nullptr;
        return canvas;
    }
}

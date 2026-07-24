#include <Arcane/Render/FullscreenMaterialChain.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/FullscreenMaterialPass.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Arcane
{
    namespace
    {
        class FullscreenMaterialChainImpl final : public FullscreenMaterialChain
        {
        public:
            explicit FullscreenMaterialChainImpl(nvrhi::IDevice* device)
                : m_device(device)
            {
            }

            bool SetChain(std::shared_ptr<const MaterialTemplate> templ,
                          std::span<const PassShaders> passes) override
            {
                if (!templ || passes.empty())
                {
                    ARC_WARN("FullscreenMaterialChain::SetChain: null template or empty chain");
                    return false;
                }

                // Build every pass into locals first -- a failure anywhere must
                // leave the previous (last-good) chain fully bound.
                std::vector<std::unique_ptr<FullscreenMaterialPass>> built;
                built.reserve(passes.size());
                for (const PassShaders& p : passes)
                {
                    auto pass = FullscreenMaterialPass::Create(m_device);
                    if (!pass || !pass->SetMaterial(templ, p.vs, p.ps, /*chainInput=*/true))
                    {
                        ARC_WARN("FullscreenMaterialChain::SetChain: pass {} rejected -- "
                                 "previous chain kept", built.size());
                        return false;
                    }
                    built.push_back(std::move(pass));
                }
                m_template = std::move(templ);
                m_passes = std::move(built);
                return true;
            }

            bool Ready() const override { return !m_passes.empty(); }
            std::size_t PassCount() const override { return m_passes.size(); }

            void Render(nvrhi::ICommandList* commandList,
                        nvrhi::IFramebuffer* target,
                        const MaterialInstance& instance,
                        const GlobalParams& globals,
                        Assets* assets,
                        std::size_t viewIndex) override
            {
                if (!Ready() || !commandList || !target)
                    return;
                const std::size_t last = std::min(viewIndex, m_passes.size() - 1);
                if (last > 0 && !EnsureIntermediates(target))
                    return;

                for (std::size_t i = 0; i <= last; ++i)
                {
                    nvrhi::IFramebuffer* out = i == last ? target : m_fb[i % 2].Get();
                    nvrhi::ITexture* input = i == 0 ? nullptr : m_tex[(i - 1) % 2].Get();
                    m_passes[i]->Render(commandList, out, instance, globals, assets, input);
                }
            }

        private:
            // Two linear intermediates sized to the target (the canvas format;
            // recreated on resize). Only chains with 2+ effective passes touch
            // them -- a single-pass chain renders straight into the target.
            bool EnsureIntermediates(nvrhi::IFramebuffer* target)
            {
                const nvrhi::FramebufferInfoEx& info = target->getFramebufferInfo();
                if (m_tex[0] && m_width == info.width && m_height == info.height)
                    return true;

                for (int i = 0; i < 2; ++i)
                {
                    auto desc = nvrhi::TextureDesc()
                        .setWidth(info.width).setHeight(info.height)
                        .setFormat(nvrhi::Format::RGBA16_FLOAT)
                        .setIsRenderTarget(true)
                        .setInitialState(nvrhi::ResourceStates::ShaderResource)
                        .setKeepInitialState(true)
                        .setDebugName(i == 0 ? "MaterialChainPing" : "MaterialChainPong");
                    m_tex[i] = m_device->createTexture(desc);
                    if (!m_tex[i])
                    {
                        ARC_WARN("FullscreenMaterialChain: intermediate creation failed "
                                 "({}x{})", info.width, info.height);
                        m_tex[0] = m_tex[1] = nullptr;
                        m_fb[0] = m_fb[1] = nullptr;
                        return false;
                    }
                    m_fb[i] = m_device->createFramebuffer(
                        nvrhi::FramebufferDesc().addColorAttachment(m_tex[i]));
                    if (!m_fb[i])
                    {
                        ARC_WARN("FullscreenMaterialChain: intermediate framebuffer failed");
                        m_tex[0] = m_tex[1] = nullptr;
                        m_fb[0] = m_fb[1] = nullptr;
                        return false;
                    }
                }
                m_width = info.width;
                m_height = info.height;
                return true;
            }

            nvrhi::IDevice* m_device;
            std::shared_ptr<const MaterialTemplate> m_template;
            std::vector<std::unique_ptr<FullscreenMaterialPass>> m_passes;

            nvrhi::TextureHandle     m_tex[2];
            nvrhi::FramebufferHandle m_fb[2];
            std::uint32_t m_width = 0, m_height = 0;
        };
    }

    std::unique_ptr<FullscreenMaterialChain> FullscreenMaterialChain::Create(
        nvrhi::IDevice* device)
    {
        if (!device)
        {
            ARC_ERROR("FullscreenMaterialChain::Create: null device");
            return nullptr;
        }
        return std::make_unique<FullscreenMaterialChainImpl>(device);
    }
}

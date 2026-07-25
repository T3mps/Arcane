#include <Arcane/Render/FullscreenMaterialChain.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/FullscreenMaterialPass.hpp>

#include <algorithm>
#include <string>

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
                          std::span<const PassShaders> passes,
                          std::uint32_t inputSlots) override
            {
                if (!templ || passes.empty())
                {
                    ARC_WARN("FullscreenMaterialChain::SetChain: null template or empty chain");
                    return false;
                }

                // Build every pass into locals first -- a failure anywhere must
                // leave the previous (last-good) chain fully bound.
                std::vector<std::unique_ptr<FullscreenMaterialPass>> built;
                std::vector<std::vector<std::uint32_t>> inputs;
                built.reserve(passes.size());
                inputs.reserve(passes.size());
                for (const PassShaders& p : passes)
                {
                    auto pass = FullscreenMaterialPass::Create(m_device);
                    if (!pass || !pass->SetMaterial(templ, p.vs, p.ps, inputSlots))
                    {
                        ARC_WARN("FullscreenMaterialChain::SetChain: pass {} rejected -- "
                                 "previous chain kept", built.size());
                        return false;
                    }
                    built.push_back(std::move(pass));
                    inputs.push_back(p.inputs);
                }
                m_template = std::move(templ);
                m_passes = std::move(built);
                m_inputs = std::move(inputs);
                // Intermediates outlive rebinds when the count matches (stable
                // thumbnails across recompiles); a count change resizes lazily.
                if (m_tex.size() != m_passes.size())
                {
                    m_tex.assign(m_passes.size(), nullptr);
                    m_fb.assign(m_passes.size(), nullptr);
                    m_width = m_height = 0;
                }
                return true;
            }

            bool Ready() const override { return !m_passes.empty(); }
            std::size_t PassCount() const override { return m_passes.size(); }

            nvrhi::ITexture* PassOutput(std::size_t pass) const override
            {
                return pass < m_tex.size() ? m_tex[pass].Get() : nullptr;
            }

            void Render(nvrhi::ICommandList* commandList,
                        nvrhi::IFramebuffer* target,
                        const MaterialInstance& instance,
                        const GlobalParams& globals,
                        Assets* assets,
                        std::size_t viewIndex) override
            {
                if (!Ready() || !commandList || !target)
                    return;
                if (!EnsureIntermediates(target))
                    return;
                const std::size_t view = (std::min)(viewIndex, m_passes.size() - 1);

                std::vector<nvrhi::ITexture*> sources;
                for (std::size_t i = 0; i < m_passes.size(); ++i)
                {
                    // Bind this pass's upstream outputs (validated earlier-only,
                    // so every source intermediate is already rendered).
                    sources.clear();
                    for (std::uint32_t in : m_inputs[i])
                        sources.push_back(in < i ? m_tex[in].Get() : nullptr);
                    m_passes[i]->Render(commandList, m_fb[i], instance, globals,
                                        assets, sources);
                    if (i == view)
                        m_passes[i]->Render(commandList, target, instance, globals,
                                            assets, sources);
                }
            }

        private:
            // One linear intermediate per pass, sized to the target (the canvas
            // format; recreated on resize). Persistent: PassOutput hands them
            // to the editor as node thumbnails.
            bool EnsureIntermediates(nvrhi::IFramebuffer* target)
            {
                const nvrhi::FramebufferInfoEx& info = target->getFramebufferInfo();
                const bool sized = m_width == info.width && m_height == info.height;
                if (sized && !m_tex.empty() && m_tex[0])
                    return true;

                for (std::size_t i = 0; i < m_passes.size(); ++i)
                {
                    auto desc = nvrhi::TextureDesc()
                        .setWidth(info.width).setHeight(info.height)
                        .setFormat(nvrhi::Format::RGBA16_FLOAT)
                        .setIsRenderTarget(true)
                        .setInitialState(nvrhi::ResourceStates::ShaderResource)
                        .setKeepInitialState(true)
                        .setDebugName(("MaterialChainPass" + std::to_string(i)).c_str());
                    m_tex[i] = m_device->createTexture(desc);
                    m_fb[i] = m_tex[i] ? m_device->createFramebuffer(
                                             nvrhi::FramebufferDesc()
                                                 .addColorAttachment(m_tex[i]))
                                       : nullptr;
                    if (!m_tex[i] || !m_fb[i])
                    {
                        ARC_WARN("FullscreenMaterialChain: intermediate {} creation "
                                 "failed ({}x{})", i, info.width, info.height);
                        m_tex.assign(m_passes.size(), nullptr);
                        m_fb.assign(m_passes.size(), nullptr);
                        m_width = m_height = 0;
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
            std::vector<std::vector<std::uint32_t>> m_inputs;   // parallel to m_passes

            std::vector<nvrhi::TextureHandle>     m_tex;   // parallel to m_passes
            std::vector<nvrhi::FramebufferHandle> m_fb;
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

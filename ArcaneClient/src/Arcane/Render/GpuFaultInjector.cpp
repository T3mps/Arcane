#include <Arcane/Render/GpuFaultInjector.hpp>

#if !defined(ARCANE_DIST)

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // GpuPassScope -- the breadcrumb this exists to produce
#include <Arcane/Render/ShaderLibrary.hpp>

namespace Arcane
{
    namespace
    {
        // Mirrors FaultCB in data/shaders/gpu_fault.hlsl. Four uints; HLSL
        // packs them into one 16-byte register, which is also the D3D12
        // constant-buffer size floor, so no padding member is needed.
        struct FaultCB
        {
            std::uint32_t iterations = 0;
            std::uint32_t oobElement = 0;
            std::uint32_t seed       = 0;
            std::uint32_t sinkMask   = 0;
        };

        // The sink is small on purpose: it exists to be a legal UAV binding and
        // to be far too small for the store the shader actually performs. A
        // power of two so the in-range store can mask instead of modulo.
        constexpr std::uint32_t kSinkElements = 256u;
        constexpr std::uint32_t kSinkStride   = sizeof(std::uint32_t);

        // 256 groups x 64 threads = 16384 threads. Enough to occupy every SM on
        // any consumer part, so the fault is a property of the workload rather
        // than of how wide the adapter happens to be. Occupancy is not what
        // makes this fault -- the per-thread serial chain is -- but a saturated
        // GPU makes the TDR unambiguous rather than a fight with other work.
        constexpr std::uint32_t kThreadGroups = 256u;

        // ~4.29 billion serially dependent iterations. Each is a 32-bit
        // multiply-add whose result the next one needs, so per-thread runtime is
        // (iterations x dependent-imad latency) and cannot be parallelised down:
        // on the order of tens of seconds even on a fast part, against a 2 s
        // default TdrDelay. Bounded rather than infinite so a machine with TDR
        // disabled recovers on its own -- see the shader's header comment.
        constexpr std::uint32_t kIterations = 0xFFFFFFFFu;

        // Element index ~4 GiB past the base of a 1 KiB buffer. Well beyond any
        // suballocation the resource could share a page with, and low enough
        // that the shader's `gOobElement + (acc & 0xFF)` cannot wrap a uint32
        // back into range.
        constexpr std::uint32_t kOobElement = 1u << 30;

        class GpuFaultInjectorImpl final : public GpuFaultInjector
        {
        public:
            bool Init(nvrhi::IDevice* device, ShaderLibrary& shaders)
            {
                // `device` stays a LOCAL: everything this object needs from it is
                // built here, and Fire only records into a caller-supplied list.
                // A stored device pointer would be state with no reader.
                if (!device)
                {
                    ARC_ERROR("GpuFaultInjector: null device");
                    return false;
                }

                nvrhi::ShaderHandle cs = shaders.Get("gpu_fault_cs", nvrhi::ShaderType::Compute);
                if (!cs)
                {
                    // ShaderLibrary already logged which artifact was missing.
                    ARC_ERROR("GpuFaultInjector: gpu_fault_cs unavailable -- "
                              "run data/shaders/compile-shaders.bat");
                    return false;
                }

                // b0 = FaultCB, u0 = the sink. Compute visibility -- the first
                // and only compute binding layout in the engine.
                m_layout = device->createBindingLayout(nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::Compute)
                    .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0))
                    .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)));
                if (!m_layout)
                {
                    ARC_ERROR("GpuFaultInjector: binding layout creation failed");
                    return false;
                }

                // Non-volatile CB written once per Fire via writeBuffer;
                // KeepInitialState lets NVRHI auto-transition
                // CopyDest <-> ConstantBuffer (the SelectionOutline idiom).
                m_cb = device->createBuffer(nvrhi::BufferDesc()
                    .setByteSize(sizeof(FaultCB))
                    .setIsConstantBuffer(true)
                    .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                    .setKeepInitialState(true)
                    .setDebugName("GpuFaultInjector.FaultCB"));

                m_sink = device->createBuffer(nvrhi::BufferDesc()
                    .setByteSize(kSinkElements * kSinkStride)
                    .setStructStride(kSinkStride)
                    .setCanHaveUAVs(true)
                    .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                    .setKeepInitialState(true)
                    .setDebugName("GpuFaultInjector.Sink"));

                if (!m_cb || !m_sink)
                {
                    ARC_ERROR("GpuFaultInjector: buffer creation failed");
                    return false;
                }

                m_bindings = device->createBindingSet(nvrhi::BindingSetDesc()
                    .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_cb))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_sink)),
                    m_layout);
                if (!m_bindings)
                {
                    ARC_ERROR("GpuFaultInjector: binding set creation failed");
                    return false;
                }

                m_pipeline = device->createComputePipeline(nvrhi::ComputePipelineDesc()
                    .setComputeShader(cs)
                    .addBindingLayout(m_layout));
                if (!m_pipeline)
                {
                    ARC_ERROR("GpuFaultInjector: compute pipeline creation failed");
                    return false;
                }

                return true;
            }

            void Fire(nvrhi::ICommandList* commandList) override
            {
                if (!commandList || !m_pipeline || !m_bindings)
                {
                    ARC_ERROR("GpuFaultInjector::Fire: not armed -- nothing dispatched");
                    return;
                }

                // Loud and unconditional. This is the one log line that will be
                // in the console when the session dies, and the difference
                // between "the diagnostics arc works" and "the editor crashed"
                // is whether a reader can find it.
                ARC_WARN("GpuFaultInjector: dispatching a DELIBERATE GPU fault "
                         "({} groups x 64 threads, {} iterations, OOB element {}). "
                         "The device is expected to be lost.",
                         kThreadGroups, kIterations, kOobElement);

                FaultCB cb;
                cb.iterations = kIterations;
                cb.oobElement = kOobElement;
                cb.seed       = 0x9E3779B9u;   // any non-zero constant; keeps the chain from folding
                cb.sinkMask   = kSinkElements - 1u;

                // Outside the scope below: a copy is not the pass, and a
                // breadcrumb that covered the CB upload would blur where the
                // GPU actually stopped.
                commandList->writeBuffer(m_cb, &cb, sizeof(cb));

                // THE breadcrumb. See the header for why this lives here rather
                // than at the call site.
                GpuPassScope pass(commandList, kPassName);

                nvrhi::ComputeState state;
                state.setPipeline(m_pipeline).addBindingSet(m_bindings);
                commandList->setComputeState(state);
                commandList->dispatch(kThreadGroups, 1, 1);
            }

        private:
            nvrhi::BindingLayoutHandle   m_layout;
            nvrhi::BufferHandle          m_cb;
            nvrhi::BufferHandle          m_sink;
            nvrhi::BindingSetHandle      m_bindings;
            nvrhi::ComputePipelineHandle m_pipeline;
        };
    }

    std::unique_ptr<GpuFaultInjector> GpuFaultInjector::Create(nvrhi::IDevice* device,
                                                               ShaderLibrary& shaders)
    {
        auto injector = std::make_unique<GpuFaultInjectorImpl>();
        if (!injector->Init(device, shaders))
            return nullptr;
        return injector;
    }
}

#endif   // !ARCANE_DIST

#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Log.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <atomic>
namespace Arcane
{
    namespace { std::atomic<ProcessContext*> g_current{nullptr}; }

    ProcessContext::ProcessContext(const ProcessContextDesc& desc) : m_dedicated(desc.isDedicatedServerProcess)
    {
        if (desc.externalTypeContext) m_context = desc.externalTypeContext;
        else { m_owned = std::make_unique<Astra::TypeContext>(); m_context = m_owned.get(); }
    }

    std::unique_ptr<ProcessContext> ProcessContext::Create(ProcessContextDesc desc)
    {
        std::unique_ptr<ProcessContext> pc(new ProcessContext(desc));
        ProcessContext* expected = nullptr;
        if (!g_current.compare_exchange_strong(expected, pc.get()))
        {
            ARC_ERROR("ProcessContext: refused -- this process already has one (spec 2026-09-15 s3: exactly one per process; N Runtimes share it)");
            return nullptr;   // ~ProcessContext with m_slotHeld == false leaves the live slot alone
        }
        pc->m_slotHeld = true;
        // Install the context in THIS module's (ArcaneCore.dll's) per-module Astra slot,
        // Resident: Core never unmaps, so its binders are pinned (Runtime.cpp's residency note).
        Astra::SetTypeContext(pc->m_context, Astra::ModuleResidency::Resident);
        return pc;
    }

    ProcessContext* ProcessContext::Current() noexcept { return g_current.load(); }

    ProcessContext::~ProcessContext()
    {
        if (m_slotHeld) g_current.store(nullptr);
        // An OWNED TypeContext is LEAKED, deliberately: TypeMeta entries registered by a
        // game module hold std::function thunks compiled into that DLL, and after its
        // unload ~TypeContext would call into unmapped code (the heap-leak both hosts
        // documented at their old `new Astra::TypeContext()` sites). Adopted contexts
        // belong to their owner.
        (void)m_owned.release();
    }
}

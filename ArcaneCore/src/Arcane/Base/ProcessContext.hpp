#pragma once
// ProcessContext: the ONE per-process object (spec 2026-09-15 s3). Owns (or adopts)
// the Astra TypeContext every Runtime and every module imports, carries the
// launch flag (s4 field 1 -- systems NEVER branch on it; they branch on a
// Runtime's NetMode), and the system-factory table a game module registers into
// once per DLL load (SystemFactories() below -- Core-owned, so it outlives every
// module image, and PluginHost clears a module's entries before its image unmaps).
// Created exactly once by the host (ArcaneServer, RuntimeApp, EditorApp,
// ArcaneTests); a second Create() is a refusal.
//
// A consumer that never builds a Runtime -- arcbuild, arccook -- creates no
// ProcessContext and touches no ComponentRegistry; this class documents that
// limit rather than enforcing it (there is nothing to enforce against).
#include <Arcane/Core/Api.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>   // SystemFactoryTable (a by-value member)
#include <memory>
namespace Astra { class TypeContext; }
namespace Arcane
{
    struct ProcessContextDesc
    {
        bool                isDedicatedServerProcess = false;
        Astra::TypeContext* externalTypeContext      = nullptr;
    };
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // unique_ptr/SystemFactoryTable members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_CORE_API ProcessContext
    {
    public:
        [[nodiscard]] static std::unique_ptr<ProcessContext> Create(ProcessContextDesc desc);
        [[nodiscard]] static ProcessContext* Current() noexcept;
        ~ProcessContext();
        ProcessContext(const ProcessContext&) = delete;
        ProcessContext& operator=(const ProcessContext&) = delete;
        Astra::TypeContext& TypeContext() noexcept { return *m_context; }
        [[nodiscard]] bool  IsDedicatedServerProcess() const noexcept { return m_dedicated; }
        // The process's ONE system-factory table (spec s4). A game module registers
        // into it from OnInit (GameModule::RegisterSystem); every Runtime built on
        // this context instantiates the subset matching its own NetMode.
        [[nodiscard]] SystemFactoryTable& SystemFactories() noexcept { return m_factories; }
    private:
        explicit ProcessContext(const ProcessContextDesc& desc);
        std::unique_ptr<Astra::TypeContext> m_owned;    // null when adopting
        Astra::TypeContext*                 m_context = nullptr;
        SystemFactoryTable                  m_factories;
        bool                                m_dedicated = false;
        bool                                m_slotHeld  = false;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

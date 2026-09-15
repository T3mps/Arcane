#pragma once
// ProcessContext: the ONE per-process object (spec 2026-09-15 s3). Owns (or adopts)
// the Astra TypeContext every Runtime and every module imports, carries the
// launch flag (s4 field 1 -- systems NEVER branch on it; they branch on a
// Runtime's NetMode), and -- from Task 5 -- the system-factory table a game module
// registers into once per DLL load. Created exactly once by the host
// (ArcaneServer, RuntimeApp, EditorApp, ArcaneTests); a second Create() is a refusal.
//
// A consumer that never builds a Runtime -- arcbuild, arccook -- creates no
// ProcessContext and touches no ComponentRegistry; this class documents that
// limit rather than enforcing it (there is nothing to enforce against).
#include <Arcane/Core/Api.hpp>
#include <memory>
namespace Astra { class TypeContext; }
namespace Arcane
{
    struct ProcessContextDesc
    {
        bool                isDedicatedServerProcess = false;
        Astra::TypeContext* externalTypeContext      = nullptr;
    };
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
    private:
        explicit ProcessContext(const ProcessContextDesc& desc);
        std::unique_ptr<Astra::TypeContext> m_owned;    // null when adopting
        Astra::TypeContext*                 m_context = nullptr;
        bool                                m_dedicated = false;
        bool                                m_slotHeld  = false;
    };
}

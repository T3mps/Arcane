#pragma once

// The ONE Core->Client reach-back (plan 1 ruling P6). Defined here, implemented by
// ClientRuntime; null on a headless host, and every Core call site null-checks.
// Mosaic::IWorkScheduler is the precedent for an interface below the seam
// implemented above it.

namespace Arcane
{
    struct EngineContext;

    struct IClientHooks
    {
        virtual ~IClientHooks() = default;
        virtual void* SaveUiContext() noexcept = 0;               // ImGui::GetCurrentContext()
        virtual void  RestoreUiContext(void* saved) noexcept = 0; // ImGui::SetCurrentContext(saved)
        virtual void  OnModuleTeardown() noexcept = 0;            // drop plugin-created audio handles (ResetAudio)
        virtual void  OnSystemsCleared() noexcept = 0;            // reinstall the presentation-side engine systems
        virtual void  FillEngineContext(EngineContext& ctx) noexcept = 0;   // the four ImGui void*s
    };
}

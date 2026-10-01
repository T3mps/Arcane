#pragma once

// The PreviewStatus model (node page + editor upgrades spec s3.2 -- its ONLY
// definition). What a shader or mesh document can honestly say about its
// compile and its preview, computed from plain inputs so the precedence is
// pinned headlessly. The report's documents[] carries the ids (schema 13);
// T3 (s5.2) adds the toolbar/preview-box copy and the UI on top. Pure: no ImGui.

#include <cstdint>

namespace Arcane::Editor
{
    enum class CompileStatus : std::uint8_t { NotCompiledHere, CompilerUnavailable, Compiling, Errors, Ok };
    enum class PreviewAvailability : std::uint8_t { Ready, NoDevice, VehicleFailed, FrameFailed };

    struct PreviewStatusInputs
    {
        bool notCompiledHere = false;    // shader: mesh surface; mesh: imported
        bool compilerAvailable = true;   // services.compiler && sources && compiler->IsAvailable()
        std::uint32_t jobsInFlight = 0;
        bool hasErrors = false;
        bool deviceSeam = false;         // chromeGraph() returned non-null
        bool vehicleFailed = false;      // creation latch
        bool frameFailed = false;        // frame latch
        bool imageBound = false;         // PreviewReady() (shader) / a rendered image (mesh)
    };

    struct PreviewStatus { CompileStatus compile; PreviewAvailability preview; bool image; };

    // Compile: notCompiledHere -> NotCompiledHere; !compilerAvailable ->
    // CompilerUnavailable; hasErrors -> Errors; jobsInFlight > 0 -> Compiling;
    // else Ok. Preview: !deviceSeam -> NoDevice; vehicleFailed -> VehicleFailed;
    // frameFailed -> FrameFailed; else Ready. image = Ready && imageBound.
    [[nodiscard]] PreviewStatus ComputePreviewStatus(const PreviewStatusInputs&) noexcept;
    [[nodiscard]] const char* CompileStatusId(CompileStatus) noexcept;
    [[nodiscard]] const char* PreviewAvailabilityId(PreviewAvailability) noexcept;
}

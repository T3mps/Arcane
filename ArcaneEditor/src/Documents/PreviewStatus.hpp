#pragma once

// The PreviewStatus model (node page + editor upgrades spec s3.2 -- its ONLY
// definition). What a shader or mesh document can honestly say about its
// compile and its preview, computed from plain inputs so the precedence is
// pinned headlessly. The report's documents[] carries the ids (schema 13);
// T3 (s5.2) adds the toolbar/preview-box copy and the UI on top. Pure: no ImGui.

#include <cstdint>
#include <string>

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

    // ---- s5.2: the text. The model above is T1's (s3.2); these only word it.
    // Why there is no image: "" when s.image. The three preview faults name
    // themselves; a Ready preview with nothing bound defers to the compile.
    [[nodiscard]] std::string NoPreviewReason(const PreviewStatus& s);
    // The toolbar's one line: the compile status, plus the preview reason when
    // the compile is fine (or failed) but nothing shows.
    [[nodiscard]] std::string ToolbarStatusText(const PreviewStatus& s);
    // What a preview box draws when !s.image. NotCompiledHere reads as the
    // imported mesh: a mesh-surface MATERIAL draws no box (s5.3), so the
    // imported-mesh document is the one box that shows it.
    [[nodiscard]] std::string PreviewBoxText(const PreviewStatus& s);
}

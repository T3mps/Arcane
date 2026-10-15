#include "Documents/PreviewStatus.hpp"

namespace Arcane::Editor
{
    PreviewStatus ComputePreviewStatus(const PreviewStatusInputs& in) noexcept
    {
        PreviewStatus s{};
        if (in.notCompiledHere)         s.compile = CompileStatus::NotCompiledHere;
        else if (!in.compilerAvailable) s.compile = CompileStatus::CompilerUnavailable;
        else if (in.hasErrors)          s.compile = CompileStatus::Errors;
        else if (in.jobsInFlight > 0)   s.compile = CompileStatus::Compiling;
        else                            s.compile = CompileStatus::Ok;

        if (!in.deviceSeam)             s.preview = PreviewAvailability::NoDevice;
        else if (in.vehicleFailed)      s.preview = PreviewAvailability::VehicleFailed;
        else if (in.frameFailed)        s.preview = PreviewAvailability::FrameFailed;
        else                            s.preview = PreviewAvailability::Ready;

        s.image = s.preview == PreviewAvailability::Ready && in.imageBound;
        return s;
    }

    const char* CompileStatusId(CompileStatus c) noexcept
    {
        switch (c)
        {
            case CompileStatus::NotCompiledHere:     return "not-compiled-here";
            case CompileStatus::CompilerUnavailable: return "compiler-unavailable";
            case CompileStatus::Compiling:           return "compiling";
            case CompileStatus::Errors:              return "errors";
            case CompileStatus::Ok:                  return "ok";
        }
        return "";
    }

    const char* PreviewAvailabilityId(PreviewAvailability p) noexcept
    {
        switch (p)
        {
            case PreviewAvailability::Ready:         return "ready";
            case PreviewAvailability::NoDevice:      return "no-device";
            case PreviewAvailability::VehicleFailed: return "vehicle-failed";
            case PreviewAvailability::FrameFailed:   return "frame-failed";
        }
        return "";
    }

    std::string NoPreviewReason(const PreviewStatus& s)
    {
        if (s.image) return {};
        switch (s.preview)
        {
            case PreviewAvailability::NoDevice:      return "no GPU device";
            case PreviewAvailability::VehicleFailed: return "preview context failed -- see the log";
            case PreviewAvailability::FrameFailed:   return "preview frame failed -- see the log";
            case PreviewAvailability::Ready:         break;
        }
        switch (s.compile)
        {
            case CompileStatus::NotCompiledHere:     return "not compiled here";
            case CompileStatus::CompilerUnavailable: return "not compiled";
            case CompileStatus::Compiling:           return "still compiling";
            case CompileStatus::Errors:              return "no successful compile yet";
            case CompileStatus::Ok:                  break;
        }
        return "nothing rendered yet";   // vehicle up, the bind lands next frame
    }

    std::string ToolbarStatusText(const PreviewStatus& s)
    {
        switch (s.compile)
        {
            case CompileStatus::NotCompiledHere:     return "not compiled here";
            case CompileStatus::CompilerUnavailable: return "not compiled -- shader compiler unavailable (see the log)";
            case CompileStatus::Compiling:           return "compiling...";
            case CompileStatus::Errors:
                return s.preview == PreviewAvailability::Ready ? std::string("errors")
                                                               : "errors, no preview (" + NoPreviewReason(s) + ")";
            case CompileStatus::Ok:                  break;
        }
        return s.image ? std::string("ok") : "compiled, no preview (" + NoPreviewReason(s) + ")";
    }

    std::string PreviewBoxText(const PreviewStatus& s)
    {
        if (s.compile == CompileStatus::NotCompiledHere)     return "Imported mesh -- preview it on a mesh in the viewport";
        if (s.compile == CompileStatus::CompilerUnavailable) return "Not compiled -- shader compiler unavailable (see the log)";
        if (s.preview != PreviewAvailability::Ready)         return "No preview -- " + NoPreviewReason(s);
        if (s.compile == CompileStatus::Compiling)           return "compiling...";
        if (s.compile == CompileStatus::Errors)              return "Errors -- no successful compile yet";
        return "Preview pending -- nothing rendered yet";
    }

    PreviewFit FitPreviewImage(float availW, float availH, float extent) noexcept
    {
        // Each axis's fit; a collapsed axis does not constrain (it falls back
        // to 1:1), so a zero-sized box still reports the image's own extent.
        const float sx = availW > 0.0f ? availW / extent : 1.0f;
        const float sy = availH > 0.0f ? availH / extent : 1.0f;
        const float scale = sx < sy ? sx : sy;
        const float side = extent * (scale > 0.0f ? scale : 1.0f);
        // Centred in what is left on each axis (never a negative offset).
        const float x = availW > side ? (availW - side) * 0.5f : 0.0f;
        const float y = availH > side ? (availH - side) * 0.5f : 0.0f;
        return { x, y, side };
    }
}

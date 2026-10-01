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
}

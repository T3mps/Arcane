#include <Arcane/Render/RenderShaderSettings.hpp>

ARC_SETTINGS(Arcane::RenderShaderSettings);
ARC_SETTINGS(Arcane::DiagnosticsGpuSettings);
ARC_SETTINGS(Arcane::DiagnosticsGpuFaultSettings);

namespace Arcane
{
    std::vector<std::wstring> DxcArgumentsFor(const RenderShaderSettings& settings)
    {
        std::vector<std::wstring> args;
        if (settings.debugInfo)
        {
            args.emplace_back(L"-Zi");
            args.emplace_back(L"-Qembed_debug");
        }
        switch (settings.optimization)
        {
        case ShaderOptimization::Default: break;
        case ShaderOptimization::O0: args.emplace_back(L"-O0"); break;
        case ShaderOptimization::O1: args.emplace_back(L"-O1"); break;
        case ShaderOptimization::O2: args.emplace_back(L"-O2"); break;
        case ShaderOptimization::O3: args.emplace_back(L"-O3"); break;
        }
        return args;
    }
}

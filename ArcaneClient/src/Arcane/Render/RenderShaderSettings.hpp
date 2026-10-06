#pragma once

#include <Arcane/Base/Api.hpp>
#include <Arcane/Config/Settings.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Arcane
{
    enum class ShaderOptimization : std::uint8_t { Default, O0, O1, O2, O3 };

    ARC_REFLECT_ENUM(ShaderOptimization)
        ARC_REFLECT_ENUM_VALUE(ShaderOptimization, Default)
        ARC_REFLECT_ENUM_VALUE(ShaderOptimization, O0)
        ARC_REFLECT_ENUM_VALUE(ShaderOptimization, O1)
        ARC_REFLECT_ENUM_VALUE(ShaderOptimization, O2)
        ARC_REFLECT_ENUM_VALUE(ShaderOptimization, O3)
    ARC_END_REFLECT_ENUM()

    struct RenderShaderSettings
    {
        double compileDebounceSeconds = 0.2;
        bool debugInfo = false;
        ShaderOptimization optimization = ShaderOptimization::Default;
    };

    ARC_REFLECT_TYPE(RenderShaderSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.shader", SettingScope::PreferencesProject, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(RenderShaderSettings, compileDebounceSeconds)
            ARC_REFLECT_ATTR(Range, 0.0, 10.0)
            ARC_REFLECT_ATTR(Tooltip, "Quiet time in seconds after an edit before the shader compiler starts a job.")
        ARC_REFLECT_FIELD(RenderShaderSettings, debugInfo)
            ARC_REFLECT_ATTR(Tooltip, "Embed DXC debug information in newly compiled shaders.")
        ARC_REFLECT_FIELD(RenderShaderSettings, optimization)
            ARC_REFLECT_ATTR(Tooltip, "DXC optimization override. Default leaves DXC's current optimization unchanged.")
    ARC_END_REFLECT_TYPE()

    struct DiagnosticsGpuSettings
    {
        std::uint32_t breadcrumbSlots = 256;
    };

    ARC_REFLECT_TYPE(DiagnosticsGpuSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "diagnostics.gpu", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(DiagnosticsGpuSettings, breadcrumbSlots)
            ARC_REFLECT_ATTR(Range, 16.0, 4096.0)
            ARC_REFLECT_ATTR(Tooltip, "Recent GPU scopes retained per queue and marker slots allocated for crash reports.")
    ARC_END_REFLECT_TYPE()

    struct DiagnosticsGpuFaultSettings
    {
        std::uint32_t removalBudgetSeconds = 45;
        std::uint32_t removalPollMs = 50;
    };

    ARC_REFLECT_TYPE(DiagnosticsGpuFaultSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "diagnostics.gpuFault", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(DiagnosticsGpuFaultSettings, removalBudgetSeconds)
            ARC_REFLECT_ATTR(Range, 1.0, 300.0)
            ARC_REFLECT_ATTR(Tooltip, "Maximum seconds to wait for D3D12 device removal after an injected GPU fault.")
        ARC_REFLECT_FIELD(DiagnosticsGpuFaultSettings, removalPollMs)
            ARC_REFLECT_ATTR(Range, 1.0, 1000.0)
            ARC_REFLECT_ATTR(Tooltip, "Milliseconds between D3D12 device-removal checks after an injected GPU fault.")
    ARC_END_REFLECT_TYPE()

    ARC_API std::vector<std::wstring> DxcArgumentsFor(const RenderShaderSettings& settings);
}

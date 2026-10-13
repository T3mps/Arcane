#include <Arcane/Host/HostSettings.hpp>

#include <Arcane/Host/HostConfig.hpp>

#include <algorithm>

ARC_SETTINGS(Arcane::RenderWindowSettings);
ARC_SETTINGS(Arcane::AppWindowSettings);
ARC_SETTINGS(Arcane::BootSettings);

// render.window.width/height's Range (HostSettings.hpp) is the --window-size
// flag's validation bounds; the two stay one pair.
static_assert(Arcane::HostConfig::kMinWindowSide == 64 && Arcane::HostConfig::kMaxWindowSide == 8192,
              "render.window.* Range must match HostConfig's kMinWindowSide/kMaxWindowSide");

namespace Arcane::HostBoot
{
    bool ShouldReportScanProgress(std::size_t done, std::size_t total)
    {
        const std::size_t stride = std::max<std::uint32_t>(1u, Settings<BootSettings>().scanProgressStride);
        return done == 1 || done == total || done % stride == 0;
    }
}

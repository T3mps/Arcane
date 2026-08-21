#include <Arcane/Render/ShaderPaths.hpp>

#include <Arcane/Base/Log.hpp>

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane::ShaderPaths
{
    std::filesystem::path ResolveFlavorDir(
        GraphicsBackend backend, const std::filesystem::path& shaderDir)
    {
        std::filesystem::path dir = shaderDir;
        if (const char* overrideDir = std::getenv("ARCANE_SHADER_DIR"))
            dir = overrideDir;

        if (dir.is_relative())
        {
            // Relative dirs anchor to the executable, not the CWD: the
            // build copies artifacts next to consumer exes, and tests must
            // pass regardless of where they are launched from.
#ifdef _WIN32
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
                dir = std::filesystem::path(modulePath).parent_path() / dir;
#endif
        }
        dir /= (backend == GraphicsBackend::Vulkan) ? "spirv" : "dxil";

        if (!std::filesystem::is_directory(dir))
        {
            ARC_ERROR("Shader directory not found: {}", dir.string());
            return {};
        }
        return dir;
    }
}

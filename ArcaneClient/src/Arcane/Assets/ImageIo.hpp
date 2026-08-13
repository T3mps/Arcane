#pragma once

// Pure CPU PNG I/O: device-free, headless image codec layer. Split from
// Assets.hpp so device-free consumers (the golden-image comparator) never
// transitively include nvrhi. Implements the decode/encode half of the assets
// facade without tying to the render device or cached texture infrastructure.

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Arcane
{
    // Decode a PNG (or any stb-supported image) from disk into tight RGBA8.
    // False on missing/corrupt file (WARN-logged, never ERROR). Pure CPU.
    ARCANE_API bool LoadPngRgba(const std::filesystem::path& path,
                                std::uint32_t& width, std::uint32_t& height,
                                std::vector<unsigned char>& rgba);

    // Encode tight RGBA8 to a PNG on disk. Parent directories are created.
    // False on IO failure (WARN-logged). Pure CPU.
    ARCANE_API bool WritePngRgba(const std::filesystem::path& path,
                                 std::uint32_t width, std::uint32_t height,
                                 const unsigned char* rgba);
}

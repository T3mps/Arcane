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
    // Decoded RGBA8 pixels, tight-packed (rowPitch == width*4), retained CPU-side.
    // This is Assets::PixelsFor's payload (NRI Phase 3, Task 1): the device-free
    // supply that lets sprite geometry and a future non-NVRHI upload path read
    // decoded pixels without ever touching a live GPU texture. Default-constructed
    // == invalid (matches an AssetCache miss/failure); nvrhi-free by this header's
    // existing charter.
    struct PixelData
    {
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;

        // True once a real decode has landed: non-zero dims and an exactly
        // tight-packed buffer. Guards callers that index rgba by width*height*4.
        bool Valid() const
        {
            return width > 0 && height > 0 &&
                   rgba.size() == static_cast<std::size_t>(width) * height * 4;
        }
    };

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

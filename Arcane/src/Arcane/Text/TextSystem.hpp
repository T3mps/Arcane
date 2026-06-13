#pragma once

// Text module: MSDF glyph text on the single submission path. Fonts load
// from disk bytes (FreeType via msdfgen's bridge); glyphs are generated
// on demand into ONE 1024x1024 RGBA8 atlas packed by SkylinePacker, and
// drawn as Batcher2D::Glyph quads. Constants kAtlasSize/kPxRange mirror
// msdf.hlsl. Latin layout only (UTF-8 decode + FreeType kerning + \n);
// HarfBuzz arrives when non-Latin does (stack-spec deferral).
//
// Recording order per frame: batcher.Begin -> Draw... -> Flush(cl) ->
// batcher.End  (Flush records the atlas upload before End records draws).

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace Arcane
{
    class Batcher2D;

    using FontId = uint32_t;
    inline constexpr FontId kInvalidFontId = 0;

    class ARCANE_API TextSystem
    {
    public:
        static std::unique_ptr<TextSystem> Create(nvrhi::IDevice* device);
        virtual ~TextSystem() = default;

        // Loads a TTF/OTF (exe-relative resolution like ShaderLibrary).
        // Returns kInvalidFontId on failure (logged).
        virtual FontId LoadFont(const std::filesystem::path& path) = 0;

        // Pushes glyph quads for utf8 text at baseline pos (pixels, y
        // down). New glyphs render into the atlas CPU-side; call Flush
        // before Batcher2D::End so the upload records first.
        virtual void Draw(Batcher2D& batcher, FontId font, float sizePx,
                          glm::vec2 baselinePos, std::string_view utf8,
                          glm::vec4 color) = 0;

        // Width/height of the laid-out text (pixels) at sizePx.
        virtual glm::vec2 Measure(FontId font, float sizePx,
                                  std::string_view utf8) = 0;

        // Records the atlas upload if glyphs were added since last Flush.
        virtual void Flush(nvrhi::ICommandList* commandList) = 0;

        // The atlas texture (debug/inspection).
        virtual nvrhi::ITexture* AtlasTexture() const = 0;
    };
}

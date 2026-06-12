#include <Arcane/Text/TextSystem.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Util/SkylinePacker.hpp>

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane
{
    namespace
    {
        // MIRRORED in msdf.hlsl (kAtlasSize / kPxRange). The atlas is ONE
        // 1024x1024 RGBA8 texture; each glyph renders at a kGlyphEmPx em box
        // with its distance field spread over kPxRange pixels. The shader's
        // Chlumsky reconstruction (kPxRange / kAtlasSize unit range) assumes
        // exactly these numbers -- change BOTH files together.
        constexpr uint32_t kAtlasSize = 1024;
        constexpr double   kPxRange   = 6.0;   // MSDF distance spread (px)
        constexpr double   kGlyphEmPx = 48.0;  // 1 em rendered at this many px
        constexpr uint32_t kPadding   = 2;     // empty border pixels per glyph

        // Read a whole file into bytes. Relative paths resolve against the
        // executable directory (same anchoring as ShaderLibrary) so tests
        // pass regardless of the launch CWD.
        std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
        {
            std::filesystem::path resolved = path;
            if (resolved.is_relative())
            {
#ifdef _WIN32
                wchar_t modulePath[MAX_PATH]{};
                if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
                    resolved = std::filesystem::path(modulePath).parent_path()
                               / resolved;
#endif
            }

            std::ifstream file(resolved, std::ios::binary | std::ios::ate);
            if (!file)
                return {};
            const std::streamsize size = file.tellg();
            if (size <= 0)
                return {};
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes((size_t)size);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
                return {};
            return bytes;
        }

        // Decode one UTF-8 codepoint from [it, end). Advances it past the
        // sequence. Invalid lead/continuation bytes yield U+FFFD and skip one
        // byte (safe resync). Latin-only layout -- HarfBuzz arrives with
        // non-Latin shaping (stack-spec deferral).
        uint32_t DecodeUtf8(const char*& it, const char* end)
        {
            const auto b0 = (unsigned char)*it;
            if (b0 < 0x80)
            {
                ++it;
                return b0;
            }
            int extra = 0;
            uint32_t cp = 0;
            if ((b0 & 0xE0) == 0xC0) { extra = 1; cp = b0 & 0x1F; }
            else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0F; }
            else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07; }
            else { ++it; return 0xFFFD; }  // stray continuation/invalid lead

            if (it + 1 + extra > end)
            {
                ++it;  // truncated sequence
                return 0xFFFD;
            }
            for (int i = 1; i <= extra; ++i)
            {
                const auto bn = (unsigned char)it[i];
                if ((bn & 0xC0) != 0x80)
                {
                    ++it;  // bad continuation: resync from next byte
                    return 0xFFFD;
                }
                cp = (cp << 6) | (bn & 0x3F);
            }
            it += 1 + extra;
            return cp;
        }

        struct GlyphEntry
        {
            glm::vec2 uvMin{ 0.0f };     // atlas uv (top-left)
            glm::vec2 uvMax{ 0.0f };     // atlas uv (bottom-right)
            glm::vec2 planeMin{ 0.0f };  // em-normalized quad bounds, y-up
            glm::vec2 planeMax{ 0.0f };
            double    advance = 0.0;     // em units
            bool      hasInk  = false;
        };

        struct Font
        {
            msdfgen::FontHandle* handle = nullptr;
            // loadFontData -> FT_New_Memory_Face does NOT copy the buffer;
            // FreeType reads it for the FT_Face lifetime, so we MUST keep
            // these bytes alive until destroyFont. (Verified against
            // ThirdParty/msdfgen/ext/import-font.cpp:176-187.)
            std::shared_ptr<std::vector<uint8_t>> bytes;
            double ascender = 0.0;    // em units (unused by layout for now)
            double lineHeight = 0.0;  // em units
            std::unordered_map<uint32_t, GlyphEntry> glyphs;
        };

        class TextSystemImpl final : public TextSystem
        {
        public:
            explicit TextSystemImpl(nvrhi::IDevice* device)
                : m_device(device)
            {
            }

            ~TextSystemImpl() override
            {
                for (Font& font : m_fonts)
                    if (font.handle)
                        msdfgen::destroyFont(font.handle);
                if (m_freetype)
                    msdfgen::deinitializeFreetype(m_freetype);
            }

            bool Init()
            {
                m_freetype = msdfgen::initializeFreetype();
                if (!m_freetype)
                {
                    ARC_ERROR("TextSystem: initializeFreetype failed");
                    return false;
                }

                auto atlasDesc = nvrhi::TextureDesc()
                    .setWidth(kAtlasSize).setHeight(kAtlasSize)
                    // RGBA8_UNORM, LINEAR (NOT sRGB): the channels are signed
                    // distance DATA, not color -- the shader medians them.
                    .setFormat(nvrhi::Format::RGBA8_UNORM)
                    .setInitialState(nvrhi::ResourceStates::ShaderResource)
                    .setKeepInitialState(true)
                    .setDebugName("GlyphAtlas");
                m_atlas = m_device->createTexture(atlasDesc);
                if (!m_atlas)
                {
                    ARC_ERROR("TextSystem: glyph atlas texture creation failed");
                    return false;
                }

                m_atlasPixels.assign((size_t)kAtlasSize * kAtlasSize * 4, 0);
                m_atlasDirty = true;  // first Flush uploads the cleared atlas
                return true;
            }

            FontId LoadFont(const std::filesystem::path& path) override
            {
                auto bytes = std::make_shared<std::vector<uint8_t>>(
                    ReadFileBytes(path));
                if (bytes->empty())
                {
                    ARC_ERROR("TextSystem: could not read font: {}",
                              path.string());
                    return kInvalidFontId;
                }

                msdfgen::FontHandle* handle = msdfgen::loadFontData(
                    m_freetype, bytes->data(), (int)bytes->size());
                if (!handle)
                {
                    ARC_ERROR("TextSystem: loadFontData failed: {}",
                              path.string());
                    return kInvalidFontId;
                }

                msdfgen::FontMetrics metrics{};
                msdfgen::getFontMetrics(metrics, handle,
                                        msdfgen::FONT_SCALING_EM_NORMALIZED);

                Font font;
                font.handle = handle;
                font.bytes = std::move(bytes);
                font.ascender = metrics.ascenderY;
                // Defensive against a future msdfgen contract change; zero line
                // height would stack lines on the baseline.
                font.lineHeight = metrics.lineHeight > 0 ? metrics.lineHeight : 1.0;
                m_fonts.push_back(std::move(font));
                return (FontId)m_fonts.size();  // FontId = index + 1
            }

            void Draw(Batcher2D& batcher, FontId font, float sizePx,
                      glm::vec2 baselinePos, std::string_view utf8,
                      glm::vec4 color) override
            {
                Font* f = Resolve(font);
                if (!f)
                    return;

                const float startX = baselinePos.x;
                float x = baselinePos.x;
                float y = baselinePos.y;
                uint32_t prev = 0;

                const char* it = utf8.data();
                const char* end = it + utf8.size();
                while (it < end)
                {
                    const uint32_t cp = DecodeUtf8(it, end);
                    if (cp == '\n')
                    {
                        x = startX;
                        y += (float)(f->lineHeight) * sizePx;
                        prev = 0;
                        continue;
                    }

                    const GlyphEntry& g = EnsureGlyph(*f, cp);

                    if (prev != 0)
                    {
                        double k = 0.0;
                        msdfgen::getKerning(k, f->handle, prev, cp,
                                            msdfgen::FONT_SCALING_EM_NORMALIZED);
                        x += (float)k * sizePx;
                    }

                    if (g.hasInk)
                    {
                        // plane is y-up em; screen is y-down px. Glyph top in
                        // screen-y is baseline - planeMax.y; left is + planeMin.x.
                        const glm::vec2 dstPos(
                            x + g.planeMin.x * sizePx,
                            y - g.planeMax.y * sizePx);
                        const glm::vec2 dstSize(
                            (g.planeMax.x - g.planeMin.x) * sizePx,
                            (g.planeMax.y - g.planeMin.y) * sizePx);
                        batcher.Glyph(dstPos, dstSize, m_atlas,
                                      g.uvMin, g.uvMax, color);
                    }

                    x += (float)g.advance * sizePx;
                    prev = cp;
                }
            }

            glm::vec2 Measure(FontId font, float sizePx,
                              std::string_view utf8) override
            {
                Font* f = Resolve(font);
                if (!f || utf8.empty())
                    return glm::vec2(0.0f);

                float x = 0.0f;
                float maxWidth = 0.0f;
                uint32_t lines = 1;
                uint32_t prev = 0;

                const char* it = utf8.data();
                const char* end = it + utf8.size();
                while (it < end)
                {
                    const uint32_t cp = DecodeUtf8(it, end);
                    if (cp == '\n')
                    {
                        maxWidth = std::max(maxWidth, x);
                        x = 0.0f;
                        ++lines;
                        prev = 0;
                        continue;
                    }

                    const GlyphEntry& g = EnsureGlyph(*f, cp);
                    if (prev != 0)
                    {
                        double k = 0.0;
                        msdfgen::getKerning(k, f->handle, prev, cp,
                                            msdfgen::FONT_SCALING_EM_NORMALIZED);
                        x += (float)k * sizePx;
                    }
                    x += (float)g.advance * sizePx;
                    prev = cp;
                }
                maxWidth = std::max(maxWidth, x);

                // Returned height is the LINE-BOX height (lines * lineHeight),
                // not ink bounds -- callers centering vertically should account
                // for ascender/descender if pixel-perfect placement is required.
                return glm::vec2(maxWidth,
                                 (float)lines * (float)f->lineHeight * sizePx);
            }

            void Flush(nvrhi::ICommandList* commandList) override
            {
                if (!m_atlasDirty)
                    return;
                // writeTexture is whole-subresource (the plan's recorded
                // contract); region staging is a later optimization.
                commandList->writeTexture(m_atlas, 0, 0, m_atlasPixels.data(),
                                          (size_t)kAtlasSize * 4);
                m_atlasDirty = false;
            }

            nvrhi::ITexture* AtlasTexture() const override
            {
                return m_atlas;
            }

        private:
            Font* Resolve(FontId font)
            {
                if (font == kInvalidFontId || font > m_fonts.size())
                    return nullptr;
                return &m_fonts[(size_t)font - 1];
            }

            const GlyphEntry& EnsureGlyph(Font& font, uint32_t codepoint)
            {
                auto it = font.glyphs.find(codepoint);
                if (it != font.glyphs.end())
                    return it->second;

                GlyphEntry entry;
                msdfgen::Shape shape;
                double advance = 0.0;
                const bool loaded = msdfgen::loadGlyph(
                    shape, font.handle, (msdfgen::unicode_t)codepoint,
                    msdfgen::FONT_SCALING_EM_NORMALIZED, &advance);
                entry.advance = loaded ? advance : 0.0;

                shape.normalize();
                // Empty shape (space, control chars): advance only, no ink.
                if (!loaded || shape.contours.empty())
                {
                    entry.hasInk = false;
                    return font.glyphs.emplace(codepoint, entry).first->second;
                }

                msdfgen::edgeColoringSimple(shape, 3.0);

                const msdfgen::Shape::Bounds b = shape.getBounds();
                // Pad the bounds by the MSDF half-range (em units) so the
                // distance band has room on every side.
                const double padEm = kPxRange / kGlyphEmPx;
                const double pl = b.l - padEm;
                const double pb = b.b - padEm;
                const double pr = b.r + padEm;
                const double pt = b.t + padEm;

                // Bitmap dimensions: padded extent in px + kPadding empty
                // border pixels on each side (atlas bleed safety).
                const int w = (int)std::ceil((pr - pl) * kGlyphEmPx)
                              + 2 * (int)kPadding;
                const int h = (int)std::ceil((pt - pb) * kGlyphEmPx)
                              + 2 * (int)kPadding;
                if (w <= 0 || h <= 0)
                {
                    entry.hasInk = false;
                    return font.glyphs.emplace(codepoint, entry).first->second;
                }

                const auto packed = m_packer.Insert((uint32_t)w, (uint32_t)h);
                if (!packed.has_value())
                {
                    // Full-atlas handling is a later milestone; log once.
                    if (!m_atlasFullLogged)
                    {
                        ARC_ERROR("TextSystem: glyph atlas full "
                                  "(1024x1024); further glyphs render blank");
                        m_atlasFullLogged = true;
                    }
                    entry.hasInk = false;
                    return font.glyphs.emplace(codepoint, entry).first->second;
                }

                // project(coord) = scale * (coord + translate). Map the
                // padded bounds bottom-left to bitmap pixel (kPadding, kPadding)
                // so the kPadding border stays empty:
                //   scale*(pl + tx) = kPadding  ->  tx = kPadding/scale - pl
                const double tx = (double)kPadding / kGlyphEmPx - pl;
                const double ty = (double)kPadding / kGlyphEmPx - pb;

                msdfgen::Bitmap<float, 3> bitmap(w, h);
                msdfgen::SDFTransformation transform(
                    msdfgen::Projection(
                        msdfgen::Vector2(kGlyphEmPx, kGlyphEmPx),
                        msdfgen::Vector2(tx, ty)),
                    msdfgen::DistanceMapping(
                        msdfgen::Range(kPxRange / kGlyphEmPx)));
                msdfgen::generateMSDF(bitmap, shape, transform);

                const uint32_t px = packed->x;
                const uint32_t py = packed->y;

                // CRITICAL y-flip: msdfgen bitmap row by=0 is the glyph BOTTOM
                // (shape is y-up; generateMSDF maps rows linearly with no flip
                // -- core/msdfgen.cpp inverseYAxis=false). The atlas is
                // top-down, so blit bitmap row by into atlas row (h-1-by).
                for (int by = 0; by < h; ++by)
                {
                    const int atlasY = (int)py + (h - 1 - by);
                    uint8_t* dst = &m_atlasPixels[
                        ((size_t)atlasY * kAtlasSize + px) * 4];
                    for (int bx = 0; bx < w; ++bx)
                    {
                        const float* texel = bitmap(bx, by);
                        for (int c = 0; c < 3; ++c)
                        {
                            // NaN-safe clamp: clamp(NaN) would propagate into
                            // the uint8 cast (UB); NaN fails "> 0.0f", pins 0.
                            float v = texel[c];
                            v = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
                            dst[bx * 4 + c] = (uint8_t)(v * 255.0f + 0.5f);
                        }
                        dst[bx * 4 + 3] = 255;
                    }
                }
                m_atlasDirty = true;

                // UVs cover the full packed rect; plane bounds cover the full
                // bitmap extent (including the kPadding border) so the sampled
                // region and the screen quad are byte-aligned. After the flip
                // blit, uvMin.y (top of the packed rect) maps to the glyph TOP.
                const float inv = 1.0f / (float)kAtlasSize;
                entry.uvMin = glm::vec2((float)px * inv, (float)py * inv);
                entry.uvMax = glm::vec2((float)(px + (uint32_t)w) * inv,
                                        (float)(py + (uint32_t)h) * inv);
                // Bitmap bottom-left in shape coords = unproject(0,0) =
                // (-tx, -ty); full extent = (w, h) / kGlyphEmPx.
                const float planeL = (float)(-tx);
                const float planeB = (float)(-ty);
                entry.planeMin = glm::vec2(planeL, planeB);
                entry.planeMax = glm::vec2(
                    planeL + (float)w / (float)kGlyphEmPx,
                    planeB + (float)h / (float)kGlyphEmPx);
                entry.hasInk = true;

                return font.glyphs.emplace(codepoint, entry).first->second;
            }

            nvrhi::IDevice* m_device;
            msdfgen::FreetypeHandle* m_freetype = nullptr;
            std::vector<Font> m_fonts;
            SkylinePacker m_packer{ kAtlasSize, kAtlasSize };
            std::vector<uint8_t> m_atlasPixels;
            nvrhi::TextureHandle m_atlas;
            bool m_atlasDirty = false;
            bool m_atlasFullLogged = false;
        };
    }

    std::unique_ptr<TextSystem> TextSystem::Create(nvrhi::IDevice* device)
    {
        auto text = std::make_unique<TextSystemImpl>(device);
        if (!text->Init())
            return nullptr;
        return text;
    }
}

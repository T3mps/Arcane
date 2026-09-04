#include "Arcane/AssetPipeline/TextureImporter.hpp"

#include "Arcane/AssetPipeline/CookKey.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace Arcane::AssetPipeline
{
    namespace
    {
        // ---- sRGB <-> linear (IEC 61966-2-1) -----------------------------------------------
        // Alpha is NEVER gamma-corrected (standard convention: alpha is a linear coverage
        // value even in an sRGB-encoded color texture) -- only R/G/B ride these curves.

        float SrgbToLinear(float c) noexcept
        {
            return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        float LinearToSrgb(float c) noexcept
        {
            return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
        }

        std::byte EncodeChannel(float v) noexcept
        {
            v = std::clamp(v, 0.0f, 1.0f);
            const auto rounded = static_cast<std::uint8_t>(std::lround(v * 255.0f));
            return static_cast<std::byte>(rounded);
        }

        // ---- Linear-float image working buffer ---------------------------------------------

        struct RgbaF { float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f; };

        std::vector<RgbaF> ToLinear(std::span<const std::byte> rgba8, std::uint32_t w, std::uint32_t h, bool srgb)
        {
            std::vector<RgbaF> out(static_cast<std::size_t>(w) * h);
            for (std::size_t i = 0; i < out.size(); ++i)
            {
                const auto* px = reinterpret_cast<const std::uint8_t*>(rgba8.data()) + i * 4;
                float r = px[0] / 255.0f;
                float g = px[1] / 255.0f;
                float b = px[2] / 255.0f;
                const float a = px[3] / 255.0f;   // alpha stays linear
                if (srgb)
                {
                    r = SrgbToLinear(r);
                    g = SrgbToLinear(g);
                    b = SrgbToLinear(b);
                }
                out[i] = RgbaF{ r, g, b, a };
            }
            return out;
        }

        std::vector<std::byte> FromLinear(const std::vector<RgbaF>& img, bool srgb)
        {
            std::vector<std::byte> out(img.size() * 4);
            for (std::size_t i = 0; i < img.size(); ++i)
            {
                float r = img[i].r, g = img[i].g, b = img[i].b;
                if (srgb)
                {
                    r = LinearToSrgb(r);
                    g = LinearToSrgb(g);
                    b = LinearToSrgb(b);
                }
                out[i * 4 + 0] = EncodeChannel(r);
                out[i * 4 + 1] = EncodeChannel(g);
                out[i * 4 + 2] = EncodeChannel(b);
                out[i * 4 + 3] = EncodeChannel(img[i].a);   // alpha stays linear
            }
            return out;
        }

        // Plain 2x2 box average, FLOOR-halved output dims: max(1, dim >> 1). Sample indices are
        // CLAMPED to the input's last row/column rather than wrapped -- this is what makes the
        // same routine correct both for an odd input dimension (the box for the last output
        // texel simply samples the even-aligned pair and the final odd row/column is dropped,
        // per the spec's "keep it simple" ruling -- no polyphase filter) and for an input
        // dimension already pinned at 1 (both sample indices clamp to 0, so the "average" is
        // just that one value, correctly leaving the pinned axis untouched).
        std::vector<RgbaF> BoxDownsampleOnce(const std::vector<RgbaF>& src, std::uint32_t w, std::uint32_t h,
                                              std::uint32_t& outW, std::uint32_t& outH)
        {
            outW = std::max<std::uint32_t>(1, w >> 1);
            outH = std::max<std::uint32_t>(1, h >> 1);

            std::vector<RgbaF> dst(static_cast<std::size_t>(outW) * outH);
            for (std::uint32_t oy = 0; oy < outH; ++oy)
            {
                const std::uint32_t iy0 = std::min(2 * oy, h - 1);
                const std::uint32_t iy1 = std::min(2 * oy + 1, h - 1);
                for (std::uint32_t ox = 0; ox < outW; ++ox)
                {
                    const std::uint32_t ix0 = std::min(2 * ox, w - 1);
                    const std::uint32_t ix1 = std::min(2 * ox + 1, w - 1);

                    const RgbaF& p00 = src[static_cast<std::size_t>(iy0) * w + ix0];
                    const RgbaF& p10 = src[static_cast<std::size_t>(iy0) * w + ix1];
                    const RgbaF& p01 = src[static_cast<std::size_t>(iy1) * w + ix0];
                    const RgbaF& p11 = src[static_cast<std::size_t>(iy1) * w + ix1];

                    dst[static_cast<std::size_t>(oy) * outW + ox] = RgbaF{
                        (p00.r + p10.r + p01.r + p11.r) * 0.25f,
                        (p00.g + p10.g + p01.g + p11.g) * 0.25f,
                        (p00.b + p10.b + p01.b + p11.b) * 0.25f,
                        (p00.a + p10.a + p01.a + p11.a) * 0.25f,
                    };
                }
            }
            return dst;
        }

        // The pure-dimensions mip chain (no pixel work) from (w,h) down to (1,1), FLOOR-halved
        // at every step -- 5 -> 2 -> 1, never 3. Used both to know how many levels exist and to
        // find where a `maxSize` clamp should start.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> BuildDimsChain(std::uint32_t w, std::uint32_t h)
        {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> chain;
            chain.emplace_back(w, h);
            while (!(w == 1 && h == 1))
            {
                w = std::max<std::uint32_t>(1, w >> 1);
                h = std::max<std::uint32_t>(1, h >> 1);
                chain.emplace_back(w, h);
            }
            return chain;
        }

        // FNV-1a 64-bit over just the source bytes -- deliberately NOT the full triple cook key
        // (ComputeCookKey, which also folds in settings + importerVersion and is already
        // recoverable from the artifact's own filename per ArtifactStore's contract). This is a
        // content-only fingerprint of the source asset, independent of import settings. Same
        // algorithm as CookKey.cpp's private Fnv1a64 (duplicated rather than shared -- that
        // class lives in CookKey.cpp's own anonymous namespace, not exported), and the same "no
        // std::hash" discipline: std::hash is implementation-defined and MUST NOT be used for
        // anything that lands on disk.
        std::uint64_t HashSourceBytes(std::span<const std::byte> bytes) noexcept
        {
            std::uint64_t h = 14695981039346656037ULL;
            constexpr std::uint64_t prime = 1099511628211ULL;
            for (std::byte b : bytes)
            {
                h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b));
                h *= prime;
            }
            return h;
        }
    }

    std::optional<ImportedTexture> ImportTexture(std::span<const std::byte> pngBytes, const Guid& sourceGuid,
                                                  const TextureMetaSettings& settings)
    {
        int decodedW = 0, decodedH = 0, channelsInFile = 0;
        stbi_uc* decoded = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(pngBytes.data()),
                                                  static_cast<int>(pngBytes.size()),
                                                  &decodedW, &decodedH, &channelsInFile, 4);
        if (decoded == nullptr || decodedW <= 0 || decodedH <= 0)
            return std::nullopt;

        const auto fullW = static_cast<std::uint32_t>(decodedW);
        const auto fullH = static_cast<std::uint32_t>(decodedH);
        const std::size_t fullByteCount = static_cast<std::size_t>(fullW) * fullH * 4;

        std::vector<std::byte> originalBytes(fullByteCount);
        std::memcpy(originalBytes.data(), decoded, fullByteCount);
        stbi_image_free(decoded);

        // Full dims chain (pure integers) so we know both how many mip levels exist and, when
        // `maxSize` is set, which level the artifact's top mip should actually start at.
        const std::vector<std::pair<std::uint32_t, std::uint32_t>> chain = BuildDimsChain(fullW, fullH);

        std::size_t startLevel = 0;
        if (settings.maxSize != 0)
        {
            while (startLevel + 1 < chain.size() &&
                   std::max(chain[startLevel].first, chain[startLevel].second) > settings.maxSize)
                ++startLevel;
        }

        // Decode the full-res image to linear float once, then walk it down to `startLevel` --
        // every intermediate level is a genuine 2x2 box average in linear space, never skipped.
        std::vector<RgbaF> floatImg = ToLinear(originalBytes, fullW, fullH, settings.srgb);
        std::uint32_t curW = fullW, curH = fullH;
        for (std::size_t i = 0; i < startLevel; ++i)
        {
            std::uint32_t nw = 0, nh = 0;
            floatImg = BoxDownsampleOnce(floatImg, curW, curH, nw, nh);
            curW = nw;
            curH = nh;
        }

        struct MipLevel { std::vector<std::byte> bytes; std::uint32_t width, height; };
        std::vector<MipLevel> levels;

        // The artifact's own mip 0: if no clamp was needed (startLevel == 0), reuse the
        // pristine decoded bytes directly rather than round-tripping them through linear float
        // and back -- lossless, and avoids an unnecessary (if deterministic) precision change on
        // an untouched top level. A clamped top level has no such original to fall back to; it
        // IS a resample.
        levels.push_back({ (startLevel == 0) ? originalBytes : FromLinear(floatImg, settings.srgb), curW, curH });

        // The (possibly clamped) top level's own linear-float representation, held aside for the
        // thumbnail -- generating further mips below mutates `floatImg` in place.
        const std::vector<RgbaF> topLevelFloat = floatImg;
        const std::uint32_t topW = curW, topH = curH;

        if (settings.generateMips)
        {
            for (std::size_t lvl = startLevel + 1; lvl < chain.size(); ++lvl)
            {
                std::uint32_t nw = 0, nh = 0;
                floatImg = BoxDownsampleOnce(floatImg, curW, curH, nw, nh);
                curW = nw;
                curH = nh;
                levels.push_back({ FromLinear(floatImg, settings.srgb), curW, curH });
            }
        }

        // Thumbnail: box-downsample the top level (never the original pre-clamp size) until its
        // long edge is <=64, aspect kept by construction (both axes halve together).
        std::vector<RgbaF> thumbFloat = topLevelFloat;
        std::uint32_t thumbW = topW, thumbH = topH;
        while (std::max(thumbW, thumbH) > 64)
        {
            std::uint32_t nw = 0, nh = 0;
            thumbFloat = BoxDownsampleOnce(thumbFloat, thumbW, thumbH, nw, nh);
            thumbW = nw;
            thumbH = nh;
        }
        std::vector<std::byte> thumbBytes = FromLinear(thumbFloat, settings.srgb);

        // Assemble the payload: every mip's bytes back to back, in order, each MipDesc addressed
        // by its own offset into `payload` (never resorted -- ArtifactFormat.hpp's contract).
        std::vector<std::byte> payload;
        std::vector<MipDesc> mips;
        std::size_t payloadSize = 0;
        for (const MipLevel& lvl : levels)
            payloadSize += lvl.bytes.size();
        payload.reserve(payloadSize);
        std::uint64_t offset = 0;
        for (const MipLevel& lvl : levels)
        {
            mips.push_back(MipDesc{ offset, static_cast<std::uint64_t>(lvl.bytes.size()), lvl.width, lvl.height });
            payload.insert(payload.end(), lvl.bytes.begin(), lvl.bytes.end());
            offset += lvl.bytes.size();
        }

        TextureArtifactDesc desc{};
        desc.contentKind = ContentKind::Texture;
        desc.sourceGuid = sourceGuid;
        desc.sourceHash = HashSourceBytes(pngBytes);
        desc.importerVersion = kTextureImporterVersion;
        // BC7 encode arrives Task 4 (bc7enc_rdo isn't vendored yet) -- every Format value
        // produces the RGBA8 payload until then. Task 4 replaces this with a real branch on
        // settings.format for Bc7/Auto.
        desc.format = ArtifactPixelFormat::RGBA8;
        desc.dimension = ArtifactDimension::Tex2D;
        desc.arrayOrDepth = 1;
        desc.width = levels.front().width;
        desc.height = levels.front().height;
        desc.mipCount = static_cast<std::uint32_t>(levels.size());
        desc.srgb = settings.srgb;
        desc.mips = std::move(mips);
        desc.thumbWidth = thumbW;
        desc.thumbHeight = thumbH;

        ImportedTexture result;
        result.desc = std::move(desc);
        result.payload = std::move(payload);
        result.thumbRgba = std::move(thumbBytes);
        return result;
    }
}

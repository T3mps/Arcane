#include <Arcane/Sprite/SpriteAsset.hpp>

#include <Arcane/Base/Log.hpp>

#include <Json.hpp>

#include <fstream>

namespace Arcane
{
    bool SaveSpriteAsset(const std::filesystem::path& path, const SpriteAssetData& data)
    {
        nlohmann::json doc;
        doc["id"] = data.id.ToString();
        doc["type"] = "sprite";              // self-describing (browser/routing hint, and
                                              // the load-time discriminator -- a sprite has
                                              // no structurally-unique key of its own)
        doc["name"] = data.name;
        if (data.texture.IsValid())
            doc["texture"] = data.texture.ToString();
        doc["ppu"] = data.ppu;
        // Only write non-default rect/pivot so a plain full-texture sprite stays a
        // minimal file (absent keys resolve to the SpriteAssetData defaults on load).
        if (data.sourcePos != glm::vec2(0.0f))
            doc["sourcePos"] = { data.sourcePos.x, data.sourcePos.y };
        if (data.sourceSize != glm::vec2(0.0f))
            doc["sourceSize"] = { data.sourceSize.x, data.sourceSize.y };
        if (data.pivot != glm::vec2(0.5f))
            doc["pivot"] = { data.pivot.x, data.pivot.y };

        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            ARC_WARN("SaveSpriteAsset: cannot write '{}'", path.generic_string());
            return false;
        }
        // error_handler_t::replace: an invalid-UTF-8 name (paste path) must degrade to
        // U+FFFD, never throw out of Save -- same rule as SaveMaterialAsset.
        out << doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
        return out.good();
    }

    std::optional<SpriteAssetData> LoadSpriteAsset(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            ARC_WARN("LoadSpriteAsset: cannot read '{}'", path.generic_string());
            return std::nullopt;
        }
        auto doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
        // A sprite has no structurally-unique key (unlike a material's "snippet"/
        // "parent"/"graph"), so the "type" tag IS the discriminator -- stricter than
        // MaterialAsset's structural sniff by necessity.
        if (!doc.is_object() || !doc.contains("type") || !doc["type"].is_string() ||
            doc["type"].get<std::string>() != "sprite")
        {
            ARC_WARN("LoadSpriteAsset: '{}' is not a sprite asset", path.generic_string());
            return std::nullopt;
        }

        SpriteAssetData data;
        if (doc.contains("id") && doc["id"].is_string())
            if (auto g = Guid::FromString(doc["id"].get<std::string>()))
                data.id = *g;
        // is_string gates (not .value): a hand-edited `"name": 5` must fall back, not
        // throw type_error out of the loader -- same rule as LoadMaterialAsset.
        data.name = doc.contains("name") && doc["name"].is_string()
                        ? doc["name"].get<std::string>()
                        : path.stem().string();
        if (doc.contains("texture") && doc["texture"].is_string())
            if (auto g = Guid::FromString(doc["texture"].get<std::string>()))
                data.texture = *g;

        if (doc.contains("ppu") && doc["ppu"].is_number())
        {
            const float ppu = doc["ppu"].get<float>();
            if (ppu > 0.0f)
                data.ppu = ppu;
            else
                ARC_WARN("LoadSpriteAsset: '{}' has ppu <= 0 -- falling back to the default {}",
                         path.generic_string(), data.ppu);
        }

        // vec2 fields are 2-element number arrays; a wrong shape keeps the
        // SpriteAssetData default rather than failing the whole load.
        auto readVec2 = [&](const char* key, glm::vec2& out)
        {
            if (!doc.contains(key) || !doc[key].is_array() || doc[key].size() != 2 ||
                !doc[key][0].is_number() || !doc[key][1].is_number())
                return;
            out = glm::vec2(doc[key][0].get<float>(), doc[key][1].get<float>());
        };
        readVec2("sourcePos", data.sourcePos);
        readVec2("sourceSize", data.sourceSize);
        readVec2("pivot", data.pivot);

        return data;
    }

    ResolvedSpriteGeom ComputeSpriteGeom(const SpriteAssetData& data,
                                          std::uint32_t texWidth, std::uint32_t texHeight)
    {
        ResolvedSpriteGeom g{ {0.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 1.0f} };
        if (texWidth == 0 || texHeight == 0)
            return g;                                  // no texture info yet: 1x1 m, full UV

        const float ppu = data.ppu > 0.0f ? data.ppu : 100.0f;
        const bool fullRect = data.sourceSize.x <= 0.0f || data.sourceSize.y <= 0.0f;
        const glm::vec2 tex(static_cast<float>(texWidth), static_cast<float>(texHeight));
        const glm::vec2 pos  = fullRect ? glm::vec2(0.0f) : data.sourcePos;
        const glm::vec2 size = fullRect ? tex : data.sourceSize;
        g.uvMin = pos / tex;
        g.uvMax = (pos + size) / tex;
        g.sizeMeters = size / ppu;
        return g;
    }
}

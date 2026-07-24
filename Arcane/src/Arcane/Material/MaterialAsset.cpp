#include <Arcane/Material/MaterialAsset.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/MaterialSource.hpp>

#include <Json.hpp>

#include <fstream>

namespace Arcane
{
    namespace
    {
        nlohmann::json ValueToJson(const MatParamValue& v)
        {
            switch (v.type)
            {
                case MatParamType::Float:   return v.f[0];
                case MatParamType::Float2:  return nlohmann::json::array({ v.f[0], v.f[1] });
                case MatParamType::Float4:
                case MatParamType::Color:
                    return nlohmann::json::array({ v.f[0], v.f[1], v.f[2], v.f[3] });
                case MatParamType::Texture: return v.tex.ToString();
            }
            return nullptr;
        }

        // Interpret a JSON value AGAINST a declared type (the decl is the truth).
        std::optional<MatParamValue> ValueFromJson(const nlohmann::json& j, MatParamType type)
        {
            auto num = [](const nlohmann::json& x, float& out)
            {
                if (!x.is_number()) return false;
                out = x.get<float>();
                return true;
            };

            float f[4] = {0, 0, 0, 0};
            switch (type)
            {
                case MatParamType::Float:
                    if (!num(j, f[0])) return std::nullopt;
                    return MatParamValue::MakeFloat(f[0]);
                case MatParamType::Float2:
                    if (!j.is_array() || j.size() != 2 ||
                        !num(j[0], f[0]) || !num(j[1], f[1])) return std::nullopt;
                    return MatParamValue::MakeFloat2(f[0], f[1]);
                case MatParamType::Float4:
                case MatParamType::Color:
                {
                    if (!j.is_array() || j.size() < 3 || j.size() > 4) return std::nullopt;
                    f[3] = 1.0f;
                    for (std::size_t i = 0; i < j.size(); ++i)
                        if (!num(j[i], f[i])) return std::nullopt;
                    MatParamValue v = MatParamValue::MakeFloat4(f[0], f[1], f[2], f[3]);
                    v.type = type;
                    return v;
                }
                case MatParamType::Texture:
                {
                    if (!j.is_string()) return std::nullopt;
                    const auto g = Guid::FromString(j.get<std::string>());
                    if (!g) return std::nullopt;
                    return MatParamValue::MakeTexture(*g);
                }
            }
            return std::nullopt;
        }
    }

    bool SaveMaterialAsset(const std::filesystem::path& path, const MaterialAssetData& data)
    {
        nlohmann::json doc;
        doc["id"] = data.id.ToString();
        doc["type"] = "material";           // self-describing (browser/routing hint)
        doc["kind"] = data.kind;
        doc["name"] = data.name;
        doc["snippet"] = data.snippet;
        nlohmann::json params = nlohmann::json::object();
        for (const auto& [name, value] : data.params)
            params[name] = ValueToJson(value);
        doc["params"] = std::move(params);

        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            ARC_WARN("SaveMaterialAsset: cannot write '{}'", path.generic_string());
            return false;
        }
        out << doc.dump(2) << '\n';
        return out.good();
    }

    std::optional<MaterialAssetData> LoadMaterialAsset(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            ARC_WARN("LoadMaterialAsset: cannot read '{}'", path.generic_string());
            return std::nullopt;
        }
        auto doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (!doc.is_object() || !doc.contains("snippet") || !doc["snippet"].is_string())
        {
            ARC_WARN("LoadMaterialAsset: '{}' is not a material asset", path.generic_string());
            return std::nullopt;
        }

        MaterialAssetData data;
        if (doc.contains("id") && doc["id"].is_string())
            if (auto g = Guid::FromString(doc["id"].get<std::string>()))
                data.id = *g;
        data.name = doc.value("name", path.stem().string());
        data.kind = doc.value("kind", std::string("fullscreen"));
        data.snippet = doc["snippet"].get<std::string>();

        // Values are typed by the snippet's OWN decls -- entries for params the
        // snippet no longer declares (or whose type changed) drop with a warn.
        const MaterialSourceParse parsed = ParseMaterialSource(data.snippet);
        if (doc.contains("params") && doc["params"].is_object())
        {
            for (const auto& [name, jvalue] : doc["params"].items())
            {
                const ParamDecl* decl = nullptr;
                for (const ParamDecl& d : parsed.decls)
                    if (d.name == name) { decl = &d; break; }
                if (!decl)
                {
                    ARC_WARN("LoadMaterialAsset: '{}' saves unknown param '{}' -- dropped",
                             path.generic_string(), name);
                    continue;
                }
                if (auto v = ValueFromJson(jvalue, decl->type))
                    data.params.emplace_back(name, *v);
                else
                    ARC_WARN("LoadMaterialAsset: '{}' param '{}' does not match its "
                             "declared type -- dropped", path.generic_string(), name);
            }
        }
        return data;
    }

    std::size_t ApplyMaterialParams(const MaterialAssetData& data, MaterialInstance& instance)
    {
        std::size_t applied = 0;
        for (const auto& [name, value] : data.params)
        {
            if (instance.Set(name, value))
                ++applied;
            else
                ARC_WARN("ApplyMaterialParams: '{}' rejected (unknown name or type mismatch)",
                         name);
        }
        return applied;
    }
}

#include <Arcane/Material/MaterialAsset.hpp>

#include <Arcane/Base/Log.hpp>

#include <Json.hpp>

#include <fstream>

namespace Arcane
{
    namespace
    {
        const char* TypeName(MatParamType t)
        {
            switch (t)
            {
                case MatParamType::Float:   return "float";
                case MatParamType::Float2:  return "float2";
                case MatParamType::Float4:  return "float4";
                case MatParamType::Color:   return "color";
                case MatParamType::Texture: return "texture";
            }
            return "float4";
        }

        bool TypeFromName(std::string_view name, MatParamType& out)
        {
            if (name == "float")   { out = MatParamType::Float;   return true; }
            if (name == "float2")  { out = MatParamType::Float2;  return true; }
            if (name == "float4")  { out = MatParamType::Float4;  return true; }
            if (name == "color")   { out = MatParamType::Color;   return true; }
            if (name == "texture") { out = MatParamType::Texture; return true; }
            return false;
        }

        // Self-typed entry: {"type": "...", "value": ...}. Instances must load
        // without their parent's declarations, so the type rides in the file.
        nlohmann::json ValueToJson(const MatParamValue& v)
        {
            nlohmann::json entry;
            entry["type"] = TypeName(v.type);
            switch (v.type)
            {
                case MatParamType::Float:
                    entry["value"] = v.f[0];
                    break;
                case MatParamType::Float2:
                    entry["value"] = nlohmann::json::array({ v.f[0], v.f[1] });
                    break;
                case MatParamType::Float4:
                case MatParamType::Color:
                    entry["value"] = nlohmann::json::array({ v.f[0], v.f[1], v.f[2], v.f[3] });
                    break;
                case MatParamType::Texture:
                    entry["value"] = v.tex.ToString();
                    break;
            }
            return entry;
        }

        std::optional<MatParamValue> ValueFromJson(const nlohmann::json& entry)
        {
            if (!entry.is_object() || !entry.contains("type") || !entry.contains("value"))
                return std::nullopt;
            MatParamType type;
            if (!entry["type"].is_string() ||
                !TypeFromName(entry["type"].get<std::string>(), type))
                return std::nullopt;

            const nlohmann::json& j = entry["value"];
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
        doc["name"] = data.name;
        if (data.IsInstance())
        {
            // Instance shape: parent + sparse overrides; snippet/kind come from
            // the base at the end of the parent chain.
            doc["parent"] = data.parent.ToString();
        }
        else
        {
            doc["kind"] = data.kind;
            doc["snippet"] = data.snippet;
        }
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
        const bool hasSnippet = doc.is_object() && doc.contains("snippet") &&
                                doc["snippet"].is_string();
        const bool hasParent = doc.is_object() && doc.contains("parent") &&
                               doc["parent"].is_string();
        if (!hasSnippet && !hasParent)
        {
            ARC_WARN("LoadMaterialAsset: '{}' is not a material asset", path.generic_string());
            return std::nullopt;
        }

        MaterialAssetData data;
        if (doc.contains("id") && doc["id"].is_string())
            if (auto g = Guid::FromString(doc["id"].get<std::string>()))
                data.id = *g;
        if (hasParent)
            if (auto g = Guid::FromString(doc["parent"].get<std::string>()))
                data.parent = *g;
        data.name = doc.value("name", path.stem().string());
        data.kind = doc.value("kind", std::string("fullscreen"));
        if (hasSnippet)
            data.snippet = doc["snippet"].get<std::string>();

        // Entries are self-typed; malformed ones drop here, decl mismatches drop
        // at APPLY time (MaterialInstance::Set is the type gate).
        if (doc.contains("params") && doc["params"].is_object())
        {
            for (const auto& [name, jvalue] : doc["params"].items())
            {
                if (auto v = ValueFromJson(jvalue))
                    data.params.emplace_back(name, *v);
                else
                    ARC_WARN("LoadMaterialAsset: '{}' param '{}' is malformed -- dropped",
                             path.generic_string(), name);
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

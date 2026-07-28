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

    }

    // Self-typed entry: {"type": "...", "value": ...}. Instances must load
    // without their parent's declarations, so the type rides in the file.
    // Exported (not file-local): graph Param nodes serialize their decl
    // defaults through the same shape.
    nlohmann::json MatParamValueToJson(const MatParamValue& v)
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

    std::optional<MatParamValue> MatParamValueFromJson(const nlohmann::json& entry)
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
            if (!data.vertexSnippet.empty())
                doc["vertexSnippet"] = data.vertexSnippet;
            // Graph-owned: the graph is the authoring truth; the snippet above
            // is its generated text (kept so snippet-only consumers never care).
            if (data.graph)
                doc["graph"] = GraphToJson(*data.graph);
            if (!data.passes.empty())
            {
                if (data.kind == "sprite")
                    ARC_WARN("SaveMaterialAsset: '{}' is a sprite material with passes -- "
                             "the sprite kind refuses chains; they will drop on load",
                             path.generic_string());
                nlohmann::json passes = nlohmann::json::array();
                for (const MaterialPass& p : data.passes)
                {
                    nlohmann::json e{
                        { "name", p.name },
                        { "snippet", p.snippet },
                        { "inputs", p.inputs },
                        { "pos", nlohmann::json::array({ p.posX, p.posY }) } };
                    if (p.graph)
                        e["graph"] = GraphToJson(*p.graph);
                    passes.push_back(std::move(e));
                }
                doc["passes"] = std::move(passes);
            }
            // The BASE pass's input slots (post materials: kSceneInput
            // entries reading the external scene) -- independent of extra
            // passes, since the common post material is base-only.
            if (!data.baseInputs.empty())
                doc["baseInputs"] = data.baseInputs;
            if (!data.passes.empty() || !data.baseInputs.empty())
                doc["chainPos"] = nlohmann::json{
                    { "base", nlohmann::json::array({ data.chainBaseX, data.chainBaseY }) },
                    { "out", nlohmann::json::array({ data.chainOutX, data.chainOutY }) },
                    { "scene", nlohmann::json::array({ data.chainSceneX, data.chainSceneY }) } };
        }
        nlohmann::json params = nlohmann::json::object();
        for (const auto& [name, value] : data.params)
            params[name] = MatParamValueToJson(value);
        doc["params"] = std::move(params);

        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            ARC_WARN("SaveMaterialAsset: cannot write '{}'", path.generic_string());
            return false;
        }
        // error_handler_t::replace: an invalid-UTF-8 snippet (paste path) must
        // degrade to U+FFFD, never throw out of Save.
        out << doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
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
        const bool hasGraph = doc.is_object() && doc.contains("graph") &&
                              doc["graph"].is_object();
        if (!hasSnippet && !hasParent && !hasGraph)
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
        // is_string gates (not .value): a hand-edited `"name": 5` must fall back,
        // not throw type_error out of the loader.
        data.name = doc.contains("name") && doc["name"].is_string()
                        ? doc["name"].get<std::string>()
                        : path.stem().string();
        data.kind = doc.contains("kind") && doc["kind"].is_string()
                        ? doc["kind"].get<std::string>()
                        : std::string("fullscreen");
        if (hasSnippet)
            data.snippet = doc["snippet"].get<std::string>();
        if (doc.contains("vertexSnippet") && doc["vertexSnippet"].is_string())
            data.vertexSnippet = doc["vertexSnippet"].get<std::string>();

        if (hasGraph)
        {
            if (hasParent)
                ARC_WARN("LoadMaterialAsset: '{}' is an instance with a graph -- graphs live "
                         "on base materials only; ignored", path.generic_string());
            else if (auto g = GraphFromJson(doc["graph"]))
            {
                data.graph = std::move(*g);
                // Self-healing: a hand-authored graph-only file still yields a
                // working snippet (save always writes it, so this is rare).
                if (data.snippet.empty())
                {
                    auto gen = GenerateGraphSnippet(*data.graph,
                                                    MaterialSurfaceForKind(data.kind));
                    if (gen.Ok())
                    {
                        data.snippet = std::move(gen.snippet);
                        // Both stages travel together: GraphCodegenResult also
                        // carries displace() for a wired Vertex Output, and
                        // dropping it here would silently lose the vertex
                        // stage until the editor's next save writes both.
                        if (data.vertexSnippet.empty())
                            data.vertexSnippet = std::move(gen.vertexSnippet);
                    }
                    else
                        ARC_WARN("LoadMaterialAsset: '{}' graph does not generate "
                                 "({} error(s)); snippet left empty",
                                 path.generic_string(), gen.errors.size());
                }
            }
            else
                ARC_WARN("LoadMaterialAsset: '{}' has a malformed graph object -- ignored "
                         "(text snippet still loads)", path.generic_string());
        }

        // Pass chain: fullscreen base materials only. Sprite kind and instances
        // REFUSE passes (warn + drop -- the file stays intact on next save only
        // if the editor re-adds them, which it won't for these shapes).
        if (doc.contains("passes") && doc["passes"].is_array())
        {
            if (data.kind == "sprite" || hasParent)
                ARC_WARN("LoadMaterialAsset: '{}' carries passes on a {} -- refused",
                         path.generic_string(), hasParent ? "instance" : "sprite material");
            else
            {
                for (const nlohmann::json& e : doc["passes"])
                {
                    if (!e.is_object() || !e.contains("snippet") || !e["snippet"].is_string())
                    {
                        ARC_WARN("LoadMaterialAsset: '{}' has a malformed pass entry -- dropped",
                                 path.generic_string());
                        continue;
                    }
                    MaterialPass p;
                    p.snippet = e["snippet"].get<std::string>();
                    p.name = e.contains("name") && e["name"].is_string()
                                 ? e["name"].get<std::string>()
                                 : "pass " + std::to_string(data.passes.size() + 1);
                    if (e.contains("inputs") && e["inputs"].is_array())
                        for (const nlohmann::json& in : e["inputs"])
                            if (in.is_number_unsigned())
                            {
                                // Anything in the sentinel half-space reads as
                                // the scene input (exact round-trip + garbage
                                // clamp in one rule).
                                const std::uint64_t v = in.get<std::uint64_t>();
                                p.inputs.push_back(v >= 0x80000000ull
                                                       ? kSceneInput
                                                       : static_cast<std::uint32_t>(v));
                            }
                    if (e.contains("pos") && e["pos"].is_array() && e["pos"].size() == 2 &&
                        e["pos"][0].is_number() && e["pos"][1].is_number())
                    {
                        p.posX = e["pos"][0].get<float>();
                        p.posY = e["pos"][1].get<float>();
                    }
                    if (e.contains("graph") && e["graph"].is_object())
                    {
                        if (auto g = GraphFromJson(e["graph"]))
                        {
                            p.graph = std::move(*g);
                            // Same self-heal as the base: a graph-only pass
                            // still yields a working snippet.
                            if (p.snippet.empty())
                            {
                                auto gen = GenerateGraphSnippet(
                                    *p.graph, MaterialSurface::Fullscreen,
                                    static_cast<std::uint32_t>(p.inputs.size()),
                                    /*passGraph=*/true);
                                if (gen.Ok())
                                    p.snippet = std::move(gen.snippet);
                                else
                                    ARC_WARN("LoadMaterialAsset: '{}' pass '{}' graph "
                                             "does not generate; snippet left empty",
                                             path.generic_string(), p.name);
                            }
                        }
                        else
                            ARC_WARN("LoadMaterialAsset: '{}' pass '{}' has a malformed "
                                     "graph -- ignored (text snippet still loads)",
                                     path.generic_string(), p.name);
                    }
                    data.passes.push_back(std::move(p));
                }
            }
        }

        if (doc.contains("chainPos") && doc["chainPos"].is_object())
        {
            const auto readPos = [&](const char* key, float& x, float& y)
            {
                const nlohmann::json& cp = doc["chainPos"];
                if (cp.contains(key) && cp[key].is_array() && cp[key].size() == 2 &&
                    cp[key][0].is_number() && cp[key][1].is_number())
                {
                    x = cp[key][0].get<float>();
                    y = cp[key][1].get<float>();
                }
            };
            readPos("base", data.chainBaseX, data.chainBaseY);
            readPos("out", data.chainOutX, data.chainOutY);
            readPos("scene", data.chainSceneX, data.chainSceneY);
        }

        // The base pass's own input slots (sprite materials refuse them along
        // with passes; same sentinel clamp as the per-pass lists).
        if (!data.IsInstance() && data.kind != "sprite" &&
            doc.contains("baseInputs") && doc["baseInputs"].is_array())
            for (const nlohmann::json& in : doc["baseInputs"])
                if (in.is_number_unsigned())
                {
                    const std::uint64_t v = in.get<std::uint64_t>();
                    data.baseInputs.push_back(v >= 0x80000000ull
                                                  ? kSceneInput
                                                  : static_cast<std::uint32_t>(v));
                }

        // Entries are self-typed; malformed ones drop here, decl mismatches drop
        // at APPLY time (MaterialInstance::Set is the type gate).
        if (doc.contains("params") && doc["params"].is_object())
        {
            for (const auto& [name, jvalue] : doc["params"].items())
            {
                if (auto v = MatParamValueFromJson(jvalue))
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

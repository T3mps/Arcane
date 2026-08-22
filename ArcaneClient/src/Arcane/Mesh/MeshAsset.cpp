#include <Arcane/Mesh/MeshAsset.hpp>

#include <Arcane/Base/Log.hpp>

#include <Json.hpp>
#include <spdlog/fmt/fmt.h>

#include <fstream>

namespace Arcane
{
    namespace
    {
        // The JSON "source" string: lowercase, hand-editable, and immune to
        // enum-ordinal churn -- a project file need not know that UvSphere
        // is 2, and an added source is obvious in a diff (SaveMeshAsset's
        // comment explains the rest of the reasoning).
        const char* SourceToJsonString(MeshSource source)
        {
            switch (source)
            {
                case MeshSource::Plane:    return "plane";
                case MeshSource::Cube:     return "cube";
                case MeshSource::UvSphere: return "uvsphere";
                case MeshSource::Cylinder: return "cylinder";
                case MeshSource::Capsule:  return "capsule";
            }
            return "cube";   // unreachable: every enumerator is handled above
        }

        // Inverse of SourceToJsonString. nullopt for anything else, so the
        // caller can warn-and-fall-back exactly once at the call site rather
        // than silently defaulting here.
        std::optional<MeshSource> JsonStringToSource(const std::string& s)
        {
            if (s == "plane")    return MeshSource::Plane;
            if (s == "cube")     return MeshSource::Cube;
            if (s == "uvsphere") return MeshSource::UvSphere;
            if (s == "cylinder") return MeshSource::Cylinder;
            if (s == "capsule")  return MeshSource::Capsule;
            return std::nullopt;
        }

        // The enum tag's own spelling, used only in ValidateMeshAsset's
        // human-readable refusal reasons -- "UvSphere needs rings >= 3", not
        // the JSON form "uvsphere needs...". A Problems-pane message should
        // read like the Inspector dropdown, not like the file on disk.
        const char* SourceDisplayName(MeshSource source)
        {
            switch (source)
            {
                case MeshSource::Plane:    return "Plane";
                case MeshSource::Cube:     return "Cube";
                case MeshSource::UvSphere: return "UvSphere";
                case MeshSource::Cylinder: return "Cylinder";
                case MeshSource::Capsule:  return "Capsule";
            }
            return "Cube";   // unreachable: every enumerator is handled above
        }
    }

    bool SaveMeshAsset(const std::filesystem::path& path, const MeshAssetData& data)
    {
        nlohmann::json doc;
        doc["id"] = data.id.ToString();
        doc["type"] = "mesh";                // self-describing (browser/routing hint, and
                                              // the load-time discriminator -- a mesh has
                                              // no structurally-unique key of its own)
        doc["name"] = data.name;
        doc["source"] = SourceToJsonString(data.source);

        // EVERY field below is written, unconditionally -- see the ruling in
        // MeshAsset.hpp's SaveMeshAsset comment. A sparse write (only the
        // fields `source` currently reads) would silently drop a Plane's
        // subdivisions the moment an author switches `source` to Cube and
        // back, because the field simply never made it into the file.
        doc["rings"] = data.rings;
        doc["segments"] = data.segments;
        doc["subdivisions"] = data.subdivisions;
        doc["capsuleLengthRatio"] = data.capsuleLengthRatio;
        doc["material"] = data.material.ToString();

        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            ARC_WARN("SaveMeshAsset: cannot write '{}'", path.generic_string());
            return false;
        }
        // error_handler_t::replace: an invalid-UTF-8 name (paste path) must degrade to
        // U+FFFD, never throw out of Save -- same rule as SaveSpriteAsset/SaveMaterialAsset.
        out << doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
        return out.good();
    }

    std::optional<MeshAssetData> LoadMeshAsset(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            ARC_WARN("LoadMeshAsset: cannot read '{}'", path.generic_string());
            return std::nullopt;
        }
        auto doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
        // A mesh has no structurally-unique key (unlike a material's "snippet"/
        // "parent"/"graph"), so the "type" tag IS the discriminator -- stricter than
        // MaterialAsset's structural sniff by necessity, same as SpriteAsset.
        if (!doc.is_object() || !doc.contains("type") || !doc["type"].is_string() ||
            doc["type"].get<std::string>() != "mesh")
        {
            ARC_WARN("LoadMeshAsset: '{}' is not a mesh asset", path.generic_string());
            return std::nullopt;
        }

        MeshAssetData data;
        if (doc.contains("id") && doc["id"].is_string())
            if (auto g = Guid::FromString(doc["id"].get<std::string>()))
                data.id = *g;
        // is_string gates (not .value): a hand-edited `"name": 5` must fall back, not
        // throw type_error out of the loader -- same rule as LoadSpriteAsset.
        data.name = doc.contains("name") && doc["name"].is_string()
                        ? doc["name"].get<std::string>()
                        : path.stem().string();

        if (doc.contains("source") && doc["source"].is_string())
        {
            const std::string s = doc["source"].get<std::string>();
            if (auto src = JsonStringToSource(s))
                data.source = *src;
            else
                // One WARN, then the MeshAssetData default (Cube) -- an unknown
                // source string is most likely a file from a future engine
                // version with a source this build does not know, not a typo
                // worth failing the whole load over.
                ARC_WARN("LoadMeshAsset: '{}' has unknown source '{}' -- falling back to Cube",
                         path.generic_string(), s);
        }

        // uint32 fields: is_number_unsigned() (not is_number()) gates out both
        // wrong-shape values AND a hand-edited negative number -- get<uint32_t>()
        // on a JSON -1 would silently wrap to 4294967295 rather than falling
        // back to the MeshAssetData default the way every other malformed
        // field here does.
        auto readUint = [&](const char* key, std::uint32_t& out)
        {
            if (doc.contains(key) && doc[key].is_number_unsigned())
                out = doc[key].get<std::uint32_t>();
        };
        readUint("rings", data.rings);
        readUint("segments", data.segments);
        readUint("subdivisions", data.subdivisions);

        if (doc.contains("capsuleLengthRatio") && doc["capsuleLengthRatio"].is_number())
            data.capsuleLengthRatio = doc["capsuleLengthRatio"].get<float>();

        if (doc.contains("material") && doc["material"].is_string())
            if (auto g = Guid::FromString(doc["material"].get<std::string>()))
                data.material = *g;

        return data;
    }

    std::optional<std::string> ValidateMeshAsset(const MeshAssetData& data)
    {
        // PER SOURCE, over the fields that source actually reads -- validating
        // the whole struct regardless of tag would refuse a legal Plane for
        // having segments == 0, which means nothing to a Plane in the first
        // place. Field order within each source matches BuildMeshData's own
        // parameter order, so the FIRST field to fail is always the one named
        // in the reason (see "the refusal reason names the offending field").
        switch (data.source)
        {
            case MeshSource::Plane:
                if (data.subdivisions < 1)
                    return fmt::format("{} needs subdivisions >= 1 (got {})",
                                        SourceDisplayName(data.source), data.subdivisions);
                return std::nullopt;

            case MeshSource::Cube:
                // Reads nothing (BuildCube(1.0f) takes no field from `data`) --
                // valid under every parameter combination.
                return std::nullopt;

            case MeshSource::UvSphere:
                if (data.rings < 3)
                    return fmt::format("{} needs rings >= 3 (got {})",
                                        SourceDisplayName(data.source), data.rings);
                if (data.segments < 3)
                    return fmt::format("{} needs segments >= 3 (got {})",
                                        SourceDisplayName(data.source), data.segments);
                return std::nullopt;

            case MeshSource::Cylinder:
                if (data.segments < 3)
                    return fmt::format("{} needs segments >= 3 (got {})",
                                        SourceDisplayName(data.source), data.segments);
                return std::nullopt;

            case MeshSource::Capsule:
                // Cap-ring floor is 2, not 3: an arc (a hemispherical cap) needs
                // fewer steps than a closed loop (UE's NumHemisphereArcSteps,
                // CapsuleGenerator.h:265-267).
                if (data.rings < 2)
                    return fmt::format("{} needs rings >= 2 (got {})",
                                        SourceDisplayName(data.source), data.rings);
                if (data.segments < 3)
                    return fmt::format("{} needs segments >= 3 (got {})",
                                        SourceDisplayName(data.source), data.segments);
                if (data.capsuleLengthRatio < 1.0f)
                    return fmt::format("{} needs capsuleLengthRatio >= 1.0 (got {})",
                                        SourceDisplayName(data.source), data.capsuleLengthRatio);
                return std::nullopt;
        }
        return std::nullopt;   // unreachable: every enumerator is handled above
    }

    std::optional<MeshData> BuildMeshData(const MeshAssetData& data)
    {
        if (ValidateMeshAsset(data).has_value())
            return std::nullopt;   // invalid mesh is an error: emit nothing

        // Two unit constants below (1.0f, 0.5f) are not magic numbers -- they
        // are THE unit rule, spelled out at the one place BuildCube/
        // BuildUvSphere still take a size parameter (both predate F2a's unit
        // generators). Every other source below is already unit by
        // construction (MeshBuilder.hpp's own contract).
        switch (data.source)
        {
            case MeshSource::Plane:    return BuildPlane(data.subdivisions);
            case MeshSource::Cube:     return BuildCube(1.0f);
            case MeshSource::UvSphere: return BuildUvSphere(0.5f, data.rings, data.segments);
            case MeshSource::Cylinder: return BuildCylinder(data.segments);
            case MeshSource::Capsule:  return BuildCapsule(data.rings, data.segments, data.capsuleLengthRatio);
        }
        return std::nullopt;   // unreachable: every enumerator is handled above
    }
}

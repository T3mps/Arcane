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
                case MeshSource::Imported: return "imported";
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
            if (s == "imported") return MeshSource::Imported;
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
                case MeshSource::Imported: return "Imported";
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
        doc["importedSource"] = data.importedSource.ToString();

        // F2c s4.4: `slots`, not the F2a scalar "material" -- the legacy key is
        // READ-ONLY from here (LoadMeshAsset still maps it for a file that has not
        // been re-saved yet), and SaveMeshAsset never writes it again. Every field
        // above is written unconditionally (this struct's own SaveMeshAsset rule),
        // and `slots` is no exception: an empty array is a legal, explicit "no
        // material assigned", not an omission.
        nlohmann::json slotsJson = nlohmann::json::array();
        for (const MeshSlot& slot : data.slots)
        {
            nlohmann::json s;
            s["name"] = slot.name;
            s["material"] = slot.material.ToString();
            slotsJson.push_back(std::move(s));
        }
        doc["slots"] = std::move(slotsJson);

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

        if (doc.contains("importedSource") && doc["importedSource"].is_string())
            if (auto g = Guid::FromString(doc["importedSource"].get<std::string>()))
                data.importedSource = *g;

        // F2c s4.4: `slots` when present and well-shaped; OTHERWISE the legacy
        // scalar "material" key, tolerantly mapped -- a VALID guid becomes one
        // unnamed slot, a nil/absent one becomes no slot at all (never a
        // fabricated slot with no material, the same never-fabricate discipline
        // s7.1 applies to geometry). No ARC_WARN on the legacy path, deliberately:
        // unlike an unknown `source` string (a genuine anomaly), a legacy
        // "material" key is the ENTIRE existing corpus -- warning on every F2a
        // file in every project would be noise for a mapping that is exact and
        // lossless, not a sign anything is wrong.
        if (doc.contains("slots") && doc["slots"].is_array())
        {
            for (const auto& s : doc["slots"])
            {
                if (!s.is_object())
                    continue;
                MeshSlot slot;
                if (s.contains("name") && s["name"].is_string())
                    slot.name = s["name"].get<std::string>();
                if (s.contains("material") && s["material"].is_string())
                    if (auto g = Guid::FromString(s["material"].get<std::string>()))
                        slot.material = *g;
                data.slots.push_back(std::move(slot));
            }
        }
        else if (doc.contains("material") && doc["material"].is_string())
        {
            if (auto g = Guid::FromString(doc["material"].get<std::string>()); g && g->IsValid())
                data.slots.push_back(MeshSlot{ std::string(), *g });
        }

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

            case MeshSource::Imported:
                // Reads importedSource, not topology -- rings/segments/subdivisions/
                // capsuleLengthRatio mean nothing to a cooked-artifact source, the
                // same "reads nothing" shape Cube's own arm above documents.
                if (!data.importedSource.IsValid())
                    return fmt::format("{} needs importedSource to be set",
                                        SourceDisplayName(data.source));
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

            case MeshSource::Imported:
                // Task 11 territory, not this function's: an imported mesh's
                // geometry comes from the cooked .arcmesh artifact (ResolveMeshData),
                // never from a procedural generator. Keeps this switch exhaustive
                // and the behaviour honest in the one commit between the two tasks
                // -- nullopt here is NOT a validation failure (ValidateMeshAsset
                // above already passed), just "wrong function, ask ResolveMeshData".
                ARC_WARN("BuildMeshData: '{}' is an Imported mesh -- resolve it through "
                         "ResolveMeshData (Task 11), not BuildMeshData",
                         data.name);
                return std::nullopt;
        }
        return std::nullopt;   // unreachable: every enumerator is handled above
    }

    MeshResolveResult ResolveMeshData(const MeshAssetData& data,
                                       const MeshArtifactSupplyFn& supply,
                                       const CookPendingFn& cookPending)
    {
        MeshResolveResult result;

        if (data.source != MeshSource::Imported)
        {
            // PRIMITIVE DELEGATION, unchanged: BuildMeshData + ComputeMeshBounds is still
            // the pure, device-free, supply-free path every builder test drives.
            // BuildMeshData validates internally (ValidateMeshAsset) and returns nullopt
            // exactly when that validation refuses `data` -- re-derive the human-readable
            // reason on that path only, the same one-extra-call-on-failure-only trade
            // MeshCache::Request already makes for the identical reason.
            auto mesh = BuildMeshData(data);
            if (!mesh)
            {
                const auto reason = ValidateMeshAsset(data);
                result.state  = MeshResolveState::Failed;
                result.reason = reason ? *reason : std::string("mesh failed to build");
                return result;
            }
            result.bounds = ComputeMeshBounds(*mesh);
            result.mesh   = std::move(*mesh);
            result.state  = MeshResolveState::Ready;
            return result;
        }

        // THE NIL GUARD: ValidateMeshAsset already refuses an Imported mesh with a nil
        // importedSource (Task 10) -- proven here too, because THIS is the function a host
        // actually calls, and a supply consulted with a nil guid is a directory scan for
        // nothing. Checked BEFORE the supply call, never after.
        if (!data.importedSource.IsValid())
        {
            result.state  = MeshResolveState::Failed;
            result.reason = fmt::format("mesh '{}' cannot resolve: importedSource is nil",
                                         data.name);
            return result;
        }

        // THE SUPPLY CALL.
        const LoadedClientMesh* artifact = supply ? supply(data.importedSource) : nullptr;
        if (!artifact)
        {
            // THE PENDING/MISSING SPLIT (s7.1): PENDING is QUIET -- empty reason, nothing
            // is wrong, it is still cooking, retry next frame. MISSING/REFUSED is LOUD --
            // the reason NAMES the guid, the same "actionable Problems-pane message" rule
            // ValidateMeshAsset's own reasons follow.
            if (cookPending && cookPending(data.importedSource))
            {
                result.state = MeshResolveState::PendingCook;
                return result;
            }
            result.state  = MeshResolveState::Failed;
            result.reason = fmt::format(
                "mesh '{}' has no cooked artifact for importedSource {}",
                data.name, data.importedSource.ToString());
            return result;
        }

        // THE ARTIFACT -> MeshData DECODE: eight floats per vertex (px,py,pz, nx,ny,nz,
        // u,v) into MeshVertex; MeshSectionView -> MeshSection field for field; the
        // artifact's OWN stored AABB into MeshBounds -- NEVER recomputed (s7.1: it was
        // cooked from these exact vertices, and recomputing would be work that can only
        // reproduce the same answer or produce a different, WRONG one).
        MeshData mesh;
        mesh.vertices.reserve(artifact->vertices.size() / 8);
        for (std::size_t i = 0; i + 7 < artifact->vertices.size(); i += 8)
        {
            MeshVertex v;
            v.position = { artifact->vertices[i + 0], artifact->vertices[i + 1], artifact->vertices[i + 2] };
            v.normal   = { artifact->vertices[i + 3], artifact->vertices[i + 4], artifact->vertices[i + 5] };
            v.uv       = { artifact->vertices[i + 6], artifact->vertices[i + 7] };
            mesh.vertices.push_back(v);
        }
        mesh.indices = artifact->indices;
        mesh.sections.reserve(artifact->sections.size());
        for (const MeshSectionView& s : artifact->sections)
            mesh.sections.push_back(MeshSection{ s.name, s.indexOffset, s.indexCount, s.slotIndex });

        result.bounds.min = { artifact->aabbMin[0], artifact->aabbMin[1], artifact->aabbMin[2] };
        result.bounds.max = { artifact->aabbMax[0], artifact->aabbMax[1], artifact->aabbMax[2] };
        result.mesh  = std::move(mesh);
        result.state = MeshResolveState::Ready;
        return result;
    }
}

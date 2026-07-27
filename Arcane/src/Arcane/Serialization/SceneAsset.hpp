#pragma once

// SceneAsset: the .arcscene FILE layer over SceneSerializer's in-memory
// SaveJson/LoadJson. On disk a scene is a native JSON asset -- a top-level "id"
// (the Guid AssetRegistry mints and resolves by) wrapped around the same
// {version, entities} document SaveJson already produces.
//
// The READ and the APPLY are deliberately separate calls. Every caller must
// validate a file BEFORE destroying the scene it already has: a failed Open
// Scene must leave the editor exactly as it was, not drop the user into an
// empty registry with an error. ReadSceneFile checks envelope, version, id,
// and entity-list STRUCTURE without touching any registry, so a rejection
// there costs the caller nothing -- but it cannot see every way
// ApplySceneDocument can still fail (see the note on ApplySceneDocument
// below). The guarantee this split actually buys is narrower than "a bad
// file can't hurt you": it is "ApplySceneDocument must only ever run against
// an already-emptied registry, so the worst case is an empty registry, never
// a half-overwritten previous scene."
//
// Exception-free: every failure returns nullopt/false and writes a reason to
// the caller's `error` string.

#include <Arcane/Guid.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace Arcane::Scene
{
    // Consistent with .arcmat. NOT ".ascene" -- the example in
    // ProjectManifest.hpp predates this and is stale.
    inline constexpr const char* kSceneExt = ".arcscene";

    // A validated scene file, parsed but NOT yet applied to any registry.
    struct SceneDocument
    {
        Arcane::Guid   id;
        nlohmann::json doc;   // { "version", "entities" } -- LoadJson's input
    };

    namespace Detail
    {
        inline void SetError(std::string* error, std::string msg)
        {
            if (error) *error = std::move(msg);
        }
    }

    // Read + validate a .arcscene. Touches no registry, so a rejection costs the
    // caller nothing. nullopt on IO failure, parse failure, a missing/malformed
    // id, a schema version this build does not speak, or an entity list whose
    // per-entry SHAPE LoadJson would reject (non-object entry, non-object
    // components, non-integer parent, non-integer-array links). This is a
    // structural check only -- see the loop below for the one class of
    // LoadJson failure it deliberately cannot catch.
    inline std::optional<SceneDocument> ReadSceneFile(const std::filesystem::path& file,
                                                      std::string* error)
    {
        std::error_code ec;
        if (!std::filesystem::exists(file, ec))
        {
            Detail::SetError(error, "no such file: " + file.generic_string());
            return std::nullopt;
        }

        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            Detail::SetError(error, "could not open " + file.generic_string());
            return std::nullopt;
        }

        try
        {
            nlohmann::json doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/true,
                                                       /*ignore_comments*/false);
            if (!doc.is_object())
            {
                Detail::SetError(error, file.generic_string() + " is not a JSON object");
                return std::nullopt;
            }

            // Version FIRST: a v1 file parsed as v2 is the failure mode worth
            // naming precisely, and LoadJson would only tell us "false".
            const auto vit = doc.find("version");
            if (vit == doc.end() || !vit->is_number_integer())
            {
                Detail::SetError(error, file.generic_string() + " has no schema version");
                return std::nullopt;
            }
            if (vit->get<int>() != kSceneJsonVersion)
            {
                Detail::SetError(error, file.generic_string() + " is scene schema version " +
                                        std::to_string(vit->get<int>()) + "; this engine reads " +
                                        std::to_string(kSceneJsonVersion));
                return std::nullopt;
            }

            const auto iit = doc.find("id");
            if (iit == doc.end() || !iit->is_string())
            {
                Detail::SetError(error, file.generic_string() + " has no asset id");
                return std::nullopt;
            }
            const std::optional<Arcane::Guid> id = Arcane::Guid::FromString(iit->get<std::string>());
            if (!id || !id->IsValid())
            {
                Detail::SetError(error, file.generic_string() + " has a malformed asset id");
                return std::nullopt;
            }

            if (!doc.contains("entities") || !doc["entities"].is_array())
            {
                Detail::SetError(error, file.generic_string() + " has no entity list");
                return std::nullopt;
            }

            // LoadJson (SceneSerializer.hpp) creates entities into the live
            // registry AS it walks this array, and can bail partway through:
            // a non-object element is only caught after earlier elements were
            // already added, and a malformed parent/links shape is likewise
            // only found mid-walk. Catch every one of those SHAPES here, up
            // front, so a file LoadJson would reject on structure is rejected
            // here instead -- before anything is touched.
            //
            // This cannot catch everything LoadJson can reject. A component
            // whose reflected field type is unsupported (the E02-3 latch
            // inside AddComponentByTypeName) only shows up once the field
            // reader actually walks the component's data -- there is no way
            // to see it without applying. That failure mode is why
            // ApplySceneDocument's contract (below) requires an
            // already-emptied registry: if E02-3 fires past this gate, the
            // worst case is an empty registry, never a half-overwritten
            // previous scene.
            const nlohmann::json& entities = doc["entities"];
            for (std::size_t i = 0; i < entities.size(); ++i)
            {
                const nlohmann::json& entry = entities[i];
                if (!entry.is_object())
                {
                    Detail::SetError(error, file.generic_string() + " entity " +
                                            std::to_string(i) + " is not an object");
                    return std::nullopt;
                }
                if (entry.contains("components") && !entry["components"].is_object())
                {
                    Detail::SetError(error, file.generic_string() + " entity " +
                                            std::to_string(i) + " has a non-object components field");
                    return std::nullopt;
                }
                if (entry.contains("parent") && !entry["parent"].is_number_integer())
                {
                    Detail::SetError(error, file.generic_string() + " entity " +
                                            std::to_string(i) + " has a non-integer parent field");
                    return std::nullopt;
                }
                if (entry.contains("links"))
                {
                    const nlohmann::json& links = entry["links"];
                    if (!links.is_array())
                    {
                        Detail::SetError(error, file.generic_string() + " entity " +
                                                std::to_string(i) + " has a non-array links field");
                        return std::nullopt;
                    }
                    for (std::size_t j = 0; j < links.size(); ++j)
                    {
                        if (!links[j].is_number_integer())
                        {
                            Detail::SetError(error, file.generic_string() + " entity " +
                                                    std::to_string(i) + " has a non-integer links entry at index " +
                                                    std::to_string(j));
                            return std::nullopt;
                        }
                    }
                }
            }

            return SceneDocument{*id, std::move(doc)};
        }
        catch (const nlohmann::json::exception& e)
        {
            Detail::SetError(error, std::string("could not parse ") + file.generic_string() +
                                    ": " + e.what());
            return std::nullopt;
        }
    }

    // Populate `reg` from an already-validated document. The caller is expected
    // to have emptied the registry first (Runtime::ResetRegistry) -- this does
    // not clear, because clearing is the caller's decision and its timing is the
    // whole point of the read/apply split.
    //
    // That empty-first contract is load-bearing, not defensive style:
    // ReadSceneFile's structural gate cannot detect an unsupported reflected
    // field type (E02-3), so this call can still fail on a document
    // ReadSceneFile accepted. If it does, the registry it partially populated
    // must already have been the empty one, never the caller's previous
    // scene -- that is the only failure mode this split does not eliminate,
    // only contain.
    inline bool ApplySceneDocument(const SceneDocument& scene, Astra::Registry& reg)
    {
        return LoadJson(reg, scene.doc);
    }

    // Serialize `reg`'s SceneRoot subtree to `file`, stamping `id` as the
    // top-level asset id. The parent directory must already exist.
    inline bool SaveSceneFile(const std::filesystem::path& file, const Astra::Registry& reg,
                              const Arcane::Guid& id, std::string* error)
    {
        if (!id.IsValid())
        {
            Detail::SetError(error, "refusing to save a scene with no asset id");
            return false;
        }
        try
        {
            nlohmann::json doc = SaveJson(reg);
            // Inserted after SaveJson so the id survives even though SaveJson
            // owns the rest of the document's shape.
            doc["id"] = id.ToString();

            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                Detail::SetError(error, "could not open " + file.generic_string() + " for writing");
                return false;
            }
            out << doc.dump(2);
            if (!out)
            {
                Detail::SetError(error, "could not write " + file.generic_string());
                return false;
            }
            return true;
        }
        catch (const nlohmann::json::exception& e)
        {
            Detail::SetError(error, std::string("could not serialize the scene: ") + e.what());
            return false;
        }
    }

    // The New Scene registry shape: one root entity carrying Transform +
    // EntityInfo, published as the SceneRoot resource.
    //
    // Not optional. SaveJson walks the SceneRoot subtree and returns an EMPTY
    // document when the resource is absent, so a New Scene without this would
    // save nothing at all and report success.
    inline Astra::Entity CreateEmpty(Astra::Registry& reg)
    {
        const Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        reg.AddComponent<Arcane::EntityInfo>(root,
                                             Arcane::EntityInfo{Arcane::Guid::Generate(), "Scene"});
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        return root;
    }
}

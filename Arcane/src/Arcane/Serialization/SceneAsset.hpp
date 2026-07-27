#pragma once

// SceneAsset: the .arcscene FILE layer over SceneSerializer's in-memory
// SaveJson/LoadJson. On disk a scene is a native JSON asset -- a top-level "id"
// (the Guid AssetRegistry mints and resolves by) wrapped around the same
// {version, entities} document SaveJson already produces.
//
// The READ and the APPLY are deliberately separate calls. Every caller must
// validate a file BEFORE destroying the scene it already has: a failed Open
// Scene must leave the editor exactly as it was, not drop the user into an
// empty registry with an error. Splitting the API makes that structural rather
// than a rule someone has to remember.
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
    // id, or a schema version this build does not speak.
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

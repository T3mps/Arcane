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

#include <Arcane/Base/Api.hpp>
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
    // id, a schema version this build does not speak, or an entity that is not
    // a JSON object -- the one per-entry SHAPE LoadJson genuinely rejects
    // mid-walk (see the loop below). A malformed parent, a malformed links
    // list, or non-object components are NOT rejected here: LoadJson tolerates
    // all three (forward/back compatibility, ReflectionJson.hpp:3-19), so a
    // gate stricter than the loader it guards would make a scene the running
    // game loads fine unopenable in the editor. This is a structural check
    // only -- it still cannot catch everything LoadJson can reject; see the
    // loop below.
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
            // registry AS it walks this array, and bails (returns false) the
            // moment an element is not an object -- but only after every
            // earlier element was already turned into a live entity. Catch
            // that one SHAPE here, up front, so it is rejected before
            // anything is touched instead of after a partial create.
            //
            // A malformed parent, a malformed links entry, or non-object
            // components are deliberately NOT checked here: LoadJson (:212,
            // :246, :254, :258) skips or continues past each of those instead
            // of failing -- forward/back compatibility for the reflection/
            // JSON bridge (ReflectionJson.hpp:3-19). Rejecting them here would
            // make this gate stricter than the loader it guards, turning a
            // scene file the running game loads fine into one the editor
            // refuses to open.
            //
            // This still cannot catch everything LoadJson can reject. A
            // component whose reflected field type is unsupported (the E02-3
            // latch inside AddComponentByTypeName) only shows up once the
            // field reader actually walks the component's data -- there is no
            // way to see it without applying. That failure mode is why
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
    //
    // Writes a temp sibling and atomically replaces `file` (SceneAsset.cpp),
    // so every failure path leaves the previously-saved scene byte-for-byte
    // intact -- the same rule Project::SetBootScene follows for the .arcproj,
    // and it matters more here: this file holds the level. Out of line because
    // the Windows replace needs <windows.h>, which does not belong in a header
    // this widely included.
    ARCANE_API bool SaveSceneFile(const std::filesystem::path& file, const Astra::Registry& reg,
                                  const Arcane::Guid& id, std::string* error);

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

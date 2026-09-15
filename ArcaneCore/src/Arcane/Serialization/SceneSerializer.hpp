#pragma once

// Scene JSON: a versioned, reflection-driven, inspectable/editable peer of the
// binary (SceneModule) runtime persistence. It round-trips an ARBITRARY reflected
// component roster -- every reflected + serializable component on each entity is
// emitted keyed by its reflected type name, and loaded back through an add-by-
// descriptor factory -- instead of a hardcoded Transform+SpriteRenderer pair.
// Both hierarchy (parent) and non-hierarchical links are persisted.
//
// Schema:
//   {
//     "version": <int>,
//     "assets":  [ "<guid>", ... ],             // v4+; always present, may be empty
//     "entities": [
//       { "components": { "<TypeName>": { ...fields... }, ... },
//         "parent": <index|-1>,
//         "links":  [ <index>, ... ] }          // optional; non-hierarchical
//     ]
//   }
//
// "assets" is the scene's REFERENCE MANIFEST: the distinct, sorted asset guids
// the entity roster names. It is an INDEX, not scene data -- the loader never
// reads it, so a stale or hand-edited manifest can mislead a reader (the asset
// panel's reference graph, or a preload set derived without loading the scene)
// but can never break a scene.
//
// Entities are ordered root-first (BFS) so parent/link indices refer to entries in
// the same array. A version mismatch is detected and reported (LoadJson returns
// false) rather than mis-parsed; the loader never throws (exception-free engine).

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/ReflectionJson.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <Json.hpp>

#include <algorithm>
#include <new>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace Arcane::Scene
{
    // Bumped when the on-disk schema layout changes. v1 was the implicit,
    // hardcoded local+sprite+parent format; v2 is the reflection-driven roster;
    // v3 is the 3D transform spine (F1, engine ABI 16).
    //
    // v3 is the bump for a BREAKING on-disk change, which is the only class of
    // change this constant exists for -- Arcane::Transform's serialized shape
    // changed incompatibly: "position"/"scale" went from 2-element to
    // 3-element arrays and "rotation" from a scalar (radians) to a 4-element
    // [x,y,z,w] quaternion. Leaving it at 2 through F1 was the final review's
    // finding 3, and the cost was concrete: a v2 file fell straight PAST the
    // version gate into the field-level reader, which refuses it one malformed
    // array at a time, when a clean "scene schema version 2; this engine reads
    // 3" was available at the envelope (SceneAsset.hpp's ReadSceneFile already
    // produces exactly that sentence). It is also the ONLY cross-version guard
    // on the entity clipboard (Edit::InstantiateSubtrees), where an old payload
    // otherwise reaches the component reader and pastes nothing.
    //
    // There is no upgrade path, deliberately: the corpus is two authored files
    // and both were re-authored with the spine (F1 decisions ledger).
    //
    // v4 (2026-09-07, asset-manager Plan 2, engine ABI 23, spec s3.3) is the
    // OTHER class of change, and the first of its kind here: purely ADDITIVE.
    // It adds the top-level "assets" reference manifest documented above and
    // changes nothing a v3 file already said, so unlike every bump before it,
    // v4 does NOT invalidate its predecessor -- see kSceneJsonVersionMin.
    //
    // v5 (2026-09-11, 2D physics wiring Plan 1, engine ABI 28, spec s7.2) is
    // ADDITIVE like v4: Collider2D::fixtures now writes as a JSON array a v4
    // engine would refuse on read (its bridge had no container branch), so
    // the number says so; nothing a v4 file already said changed, and v4 (and
    // v3) keep loading -- kSceneJsonVersionMin stays 3.
    inline constexpr int kSceneJsonVersion = 5;

    // The OLDEST schema this build still loads. v4's addition is additive, so a
    // v3 file is read exactly as it always was (the loader simply never looks
    // for a manifest, and its absence is not an error). v1/v2 stay REFUSED:
    // their Transform has a genuinely incompatible on-disk shape, which is the
    // whole reason the v3 bump was a bump.
    //
    // The two gates that enforce this range are LoadJson below and
    // ReadSceneFile (SceneAsset.hpp). The entity-clipboard gate
    // (Edit::InstantiateSubtrees, EntityOps.cpp) deliberately stays an EQUALITY
    // test against kSceneJsonVersion: a clipboard payload is written by the
    // same running build that reads it, never persisted, so it has no old
    // corpus to stay compatible with.
    inline constexpr int kSceneJsonVersionMin = 3;

    // Walks a type's serializable reflected fields, driving the given visitor.
    // (The seam's per-type walk, reused outside ComponentRegistry for known types.)
    template<typename T>
    inline void ForEachReflectedField(void* instance, Astra::IFieldVisitor& visitor)
    {
        const Astra::TypeMeta* meta = Astra::GetMeta<T>();
        if (!meta) return;
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.IsSerializable())
                visitor.Visit(f, instance);
    }

    // Serializes every reflected + serializable component on the scene subtree,
    // keyed by reflected type name, plus parent + non-hierarchical links, plus
    // the v4 asset reference manifest. Returns
    // { "version", "assets": [...], "entities": [...] }.
    //
    // "assets" is emitted UNCONDITIONALLY -- including for the no-SceneRoot
    // early return below, and as an empty array for a scene that references
    // nothing. That is what lets a consumer read "v4 and no assets key" as a
    // malformed file rather than having to guess between "references nothing"
    // and "written by something that did not emit manifests".
    inline nlohmann::json SaveJson(const Astra::Registry& reg)
    {
        nlohmann::json doc;
        doc["version"] = kSceneJsonVersion;
        doc["assets"] = nlohmann::json::array();
        doc["entities"] = nlohmann::json::array();

        const SceneRoot* sceneRoot = reg.GetResource<SceneRoot>();
        if (!sceneRoot) return doc;

        // Every asset-reference guid the component walk below passes over, in
        // mention order and with repeats -- the writer's sink is deliberately
        // raw (ReflectionJson.hpp). Distinctness and ordering are decided once,
        // after the loop.
        std::vector<Arcane::Guid> sceneAssets;

        std::vector<Astra::Entity> order;
        order.push_back(sceneRoot->entity);
        reg.GetRelations(sceneRoot->entity).ForEachDescendant(
            [&](Astra::Entity e, size_t) { order.push_back(e); });

        auto indexOf = [&](Astra::Entity e) -> int
        {
            for (size_t i = 0; i < order.size(); ++i)
                if (order[i] == e) return static_cast<int>(i);
            return -1;
        };

        for (Astra::Entity e : order)
        {
            nlohmann::json entry;

            // Arbitrary component roster: every reflected component on the entity.
            nlohmann::json components = nlohmann::json::object();
            for (const Astra::ComponentDescriptor* desc : reg.GetEntityComponents(e))
            {
                if (!desc || !desc->meta || !desc->visitFields)
                    continue;   // non-reflected component -> outside the JSON contract
                void* instance = const_cast<void*>(reg.GetComponentByHash(e, desc->hash));
                if (!instance)
                {
                    // Zero-size ("empty"/tag) components -- e.g. Hidden -- have NO
                    // storage array (Astra's is_empty optimization: desc->size == 0),
                    // so GetComponentByHash always returns nullptr for them even though
                    // the entity carries the component (present in the archetype mask).
                    // That is "nothing to read", not "absent" -- write the same null
                    // wire shape SaveJson already uses for an all-Serializable(false)
                    // component, so LoadJson's existing null-body handling (roster
                    // faithfulness: key present => component present) reconstructs it.
                    if (desc->is_empty)
                        components[std::string(desc->meta->typeName)] = nullptr;
                    continue;
                }
                nlohmann::json cj;
                ReflectionJsonWriter writer(cj, &sceneAssets);
                desc->visitFields(instance, writer);   // writer READS; const_cast is safe
                // writer.HasError() is intentionally not checked here: SAVE is
                // best-effort (there is no error channel back to the scene-save
                // caller today, and this path is exercised by tests, not yet by
                // a shipping editor save button). The only field shape that used
                // to cause silent DATA LOSS on the round trip -- an all-
                // Serializable(false) component writing as JSON null -- is fixed
                // at LOAD time (roster faithfulness, see LoadJson below), so an
                // unchecked writer error here cannot resurrect that bug.
                components[std::string(desc->meta->typeName)] = std::move(cj);
            }
            entry["components"] = std::move(components);

            entry["parent"] = indexOf(reg.GetParent(e));

            // Non-hierarchical links: emit each undirected edge once (forward
            // edge, target index > self index) so load adds it exactly once.
            const int self = indexOf(e);
            nlohmann::json links = nlohmann::json::array();
            for (Astra::Entity linked : reg.GetRelationshipGraph().GetLinks(e))
            {
                const int j = indexOf(linked);
                if (j > self) links.push_back(j);
            }
            if (!links.empty())
                entry["links"] = std::move(links);

            doc["entities"].push_back(std::move(entry));
        }

        // The manifest: distinct targets, one entry per referenced asset rather
        // than one per mention (a scene routinely names the same material from
        // several components), sorted by canonical guid string so re-saving an
        // unchanged scene produces an unchanged block -- the manifest must not
        // add noise to a diff just because the entity walk visited components in
        // a different order.
        std::unordered_set<Arcane::Guid> seen;
        std::vector<std::string> manifest;
        manifest.reserve(sceneAssets.size());
        for (const Arcane::Guid& g : sceneAssets)
            if (seen.insert(g).second)
                manifest.push_back(g.ToString());
        std::sort(manifest.begin(), manifest.end());
        doc["assets"] = std::move(manifest);

        return doc;
    }

    namespace Detail
    {
        // Outcome of AddComponentByTypeName. The two Skipped* values are both
        // "the type could not be instantiated at all" -- forward-compat, the
        // caller tolerates this and the scene still loads -- but split by
        // cause because the fix differs:
        //   SkippedUnknownType   the name has NO reflection at all (typo, a
        //                        renamed type whose old key is still on disk
        //                        -- e.g. the 2026-07-27 EntityInfo->Identity
        //                        rename, which is the incident that motivated
        //                        LoadJson warning on skips at all).
        //   SkippedUnregistered  the type IS reflected (TypeMeta exists) but
        //                        was never registered on THIS registry's
        //                        ComponentRegistry -- usually means the
        //                        plugin/module that owns the type is not
        //                        loaded in this process.
        // Added: instantiated and populated cleanly. Error: instantiated, but the
        // reflection reader latched while populating it -- either an unsupported
        // field TYPE (E02-3) or, since Task 3 (F1), a field key that is present
        // but unreadable. This must fail the whole load, not silently install a
        // partially-populated component with no signal anything went wrong.
        enum class AddComponentResult { SkippedUnknownType, SkippedUnregistered, Added, Error };

        // Add-by-descriptor factory: instantiate a component by its reflected type
        // name and populate it from JSON via the reflection reader. Never throws.
        //
        // `error`, when non-null, receives the reader's own diagnostic on the
        // Error result -- the ONE string that names the offending FIELD
        // ("malformed JSON value for field 'position' ...", ReflectionJson.hpp).
        // It is an out-parameter rather than part of the enum because every
        // caller needs the enum and only the failing branch needs the string.
        //
        // Final-review finding 1 (F1): this message used to be built, latched,
        // and then dropped on the floor -- callers read HasError() and returned
        // a bare false, so the editor's "parsed but could not be loaded (see
        // Console)" modal pointed at a Console that said nothing at all. Before
        // Task 3 the path was reachable only by an unsupported field type, a
        // code defect a developer finds anyway; Task 3 made it reachable BY
        // DATA, on exactly the hand-edited-file route the reader documents
        // itself as existing to survive.
        inline AddComponentResult AddComponentByTypeName(Astra::Registry& reg, Astra::ComponentRegistry* creg,
                                           Astra::Entity e, const std::string& typeName,
                                           const nlohmann::json& fields,
                                           std::string* error = nullptr)
        {
            const Astra::TypeMeta* meta = Astra::GetMetaByName(typeName);
            if (!meta) return AddComponentResult::SkippedUnknownType;
            const Astra::ComponentDescriptor* desc = creg->GetComponentDescriptorByHash(meta->typeHash);
            if (!desc) return AddComponentResult::SkippedUnregistered;   // reflected but not registered as a component

            const std::size_t bytes = desc->size ? desc->size : 1;
            const std::align_val_t align{ desc->alignment ? desc->alignment : 1 };
            void* buf = ::operator new(bytes, align);
            desc->DefaultConstruct(buf);
            bool fieldError = false;
            if (desc->visitFields)
            {
                ReflectionJsonReader reader(fields);
                desc->visitFields(buf, reader);   // tolerant of a MISSING key (keeps the default);
                                                   // latches HasError() on an unsupported field TYPE
                                                   // and on a key that IS present but unreadable
                                                   // (wrong JSON type / arity) -- see ReflectionJson.hpp
                fieldError = reader.HasError();
                if (fieldError && error)
                    *error = reader.Error();
            }
            reg.AddComponentByID(e, desc->id, buf, desc->size);
            desc->Destruct(buf);
            ::operator delete(buf, align);
            return fieldError ? AddComponentResult::Error : AddComponentResult::Added;
        }
    }

    // Returns false (never throws) on a malformed document, a missing/mismatched
    // schema version, a structurally invalid entity list, or a component whose
    // reflection reader latched -- either an unsupported field TYPE (E02-3) or,
    // since Task 3 (F1), a field key that is PRESENT but unreadable (wrong JSON
    // type, wrong array arity, a non-unit quaternion). A MISSING key is still
    // tolerated and leaves the field at its default; that is the forward/back-
    // compatibility path and it is unchanged. See ReflectionJson.hpp's header
    // for why the two cases had to stop being the same answer.
    //
    // Which of the two is COMMON matters for anyone reading a failure: since
    // Task 3 it is malformed DATA. An unsupported field type is a code defect a
    // developer trips in dev; a wrong-shaped value is a hand-edited or stale
    // file, and it is the case a user actually meets. Both now leave an
    // ARC_WARN and a published "scene.component.malformed" Diagnostic naming
    // the component and, through the reader's own message, the field.
    inline bool LoadJson(Astra::Registry& reg, const nlohmann::json& doc)
    {
        try
        {
            if (!doc.is_object() || !doc.contains("entities")) return false;

            // Version gate: a missing/wrong-typed/out-of-range version is
            // reported (clean false), never silently mis-parsed as the current
            // schema. A RANGE since v4, because v4 is purely additive over v3
            // (see kSceneJsonVersionMin) -- a NEWER-than-this-build version is
            // still refused, since this loader cannot know what it would be
            // mis-reading.
            //
            // Nothing below reads "assets": the manifest is a save-side index
            // and the loader is deliberately blind to it, so a stale or
            // hand-corrupted one cannot cost a scene a single entity.
            const auto vit = doc.find("version");
            if (vit == doc.end() || !vit->is_number_integer()) return false;
            const int version = vit->get<int>();
            if (version < kSceneJsonVersionMin || version > kSceneJsonVersion) return false;

            const auto& entities = doc["entities"];
            if (!entities.is_array()) return false;

            Astra::ComponentRegistry* creg = reg.GetComponentRegistry();

            std::vector<Astra::Entity> created;
            created.reserve(entities.size());

            // Accumulated across the WHOLE load and published ONCE after the
            // entity loop below -- Diagnostics::Publish is a publication-group
            // replace, so publishing per-entity would leave only the last
            // entity's rows visible. See the unconditional Publish() after the
            // loop for the empty-vector/clean-reload retraction case.
            std::vector<Arcane::Diagnostic> diagnostics;

            // File-order position of the entity currently being populated. This
            // is the only entity identification available at skip time that is
            // never in doubt: the entity's own components (e.g. Identity, which
            // carries the human-readable name) may not have been added yet --
            // walk order inside "components" follows nlohmann::json's key sort,
            // not JSON-file order, and the skipped key can itself BE the
            // identity component (exactly what happened in the incident that
            // motivated this warning: a scene's "Arcane::EntityInfo" key,
            // orphaned by the EntityInfo->Identity rename, was silently
            // dropped). Do not attempt to read a name off `e` here -- there may
            // not be one yet.
            std::size_t entityIndex = 0;

            for (const auto& entry : entities)
            {
                if (!entry.is_object()) return false;

                Astra::Entity e = reg.CreateEntity();
                const auto cit = entry.find("components");
                if (cit != entry.end() && cit->is_object())
                {
                    // A component whose fields are ALL Serializable(false) (e.g.
                    // WorldTransform) writes as JSON null in SaveJson -- the
                    // writer visits zero fields. The roster must stay faithful
                    // (key present => component present), so a null body still
                    // gets a default-constructed component added (there is
                    // nothing to populate either way). Any other non-object
                    // shape matches neither the populated-object nor the
                    // all-non-serializable-null case, so it is left skipped.
                    static const nlohmann::json kEmptyFields = nlohmann::json::object();
                    for (auto it = cit->begin(); it != cit->end(); ++it)
                    {
                        const nlohmann::json* fields;
                        if (it.value().is_object())      fields = &it.value();
                        else if (it.value().is_null())   fields = &kEmptyFields;
                        else                              continue;

                        std::string fieldError;
                        const Detail::AddComponentResult r =
                            Detail::AddComponentByTypeName(reg, creg, e, it.key(), *fields,
                                                           &fieldError);
                        if (r == Detail::AddComponentResult::Error)
                        {
                            // Fail loud -- and SAY WHY. Final-review finding 1:
                            // this branch used to be a bare `return false` while
                            // the two far less serious Skipped* cases below each
                            // got an ARC_WARN and a published Diagnostic, so the
                            // one failure that refuses the WHOLE scene was the
                            // only one with nothing behind it. `fieldError` is
                            // the reader's message and names the field; without
                            // it the user is told a file "could not be loaded"
                            // and given no way to find out which value did it.
                            ARC_WARN("scene load: component \"{}\" on entity #{} "
                                     "(id {}, v{}) could not be read -- {}; the scene "
                                     "was NOT loaded",
                                     it.key(), entityIndex, e.GetID(),
                                     static_cast<unsigned>(e.GetVersion()), fieldError);
                            Arcane::Diagnostic d;
                            d.severity = Arcane::DiagSeverity::Error;
                            d.scope    = Arcane::DiagScope::Scene;
                            d.code     = "scene.component.malformed";
                            d.message  = "Component \"" + std::string(it.key()) +
                                         "\" on entity #" + std::to_string(entityIndex) +
                                         " could not be read";
                            d.detail   = fieldError + " -- the scene was not loaded.";
                            // NO locator, deliberately, unlike the Skipped*
                            // branches below. Those run on a load that SUCCEEDS,
                            // so their entity handle stays live and clickable;
                            // this one abandons the partially-created scene, so
                            // an Entity locator would send the user chasing a
                            // handle into a registry the caller is about to
                            // replace (EditorApp::DoOpenScene -> CreateEmpty).
                            //
                            // Published HERE, because the post-loop Publish is
                            // never reached from this return -- and publication
                            // is a group REPLACE, so skipping it entirely would
                            // leave the PREVIOUS scene's rows standing as though
                            // they still described the editor's state.
                            //
                            // Published as a ONE-ROW group, NOT as
                            // `diagnostics`: the Skipped* rows accumulated above
                            // carry Entity locators into the partial scene this
                            // load is abandoning, and they describe skips inside
                            // a load that did not happen. The one row that is
                            // still true afterwards is this one.
                            Arcane::Diagnostics::Publish(
                                "scene", std::span<const Arcane::Diagnostic>(&d, 1));
                            return false;
                        }

                        // Skipped (either cause): unknown/unregistered types are
                        // tolerated -- a structurally valid scene still loads --
                        // but this WARN is the whole point of this change: before
                        // it, a skip left no trace anywhere, and re-saving the
                        // scene afterward (SaveJson only ever walks the LIVE
                        // roster) writes the file back out WITHOUT the dropped
                        // component, permanently. The two causes get different
                        // wording because they imply different next actions for
                        // the user: a rename/typo to go fix in the scene file,
                        // versus a plugin that needs to be loaded.
                        // GetVersion() is VersionType (uint8_t by default) --
                        // widened to unsigned so spdlog/fmt formats it as a
                        // number, not as a raw character.
                        if (r == Detail::AddComponentResult::SkippedUnknownType)
                        {
                            ARC_WARN("scene load: unknown component \"{}\" skipped on "
                                     "entity #{} (id {}, v{}) -- re-saving this scene "
                                     "will drop it permanently",
                                     it.key(), entityIndex, e.GetID(),
                                     static_cast<unsigned>(e.GetVersion()));
                            Arcane::Diagnostic d;
                            d.severity = Arcane::DiagSeverity::Error;
                            d.scope    = Arcane::DiagScope::Scene;
                            d.code     = "scene.component.unknown";
                            d.message  = "Unknown component \"" + std::string(it.key()) +
                                         "\" on entity #" + std::to_string(entityIndex);
                            d.detail   = "Re-saving this scene will drop it permanently.";
                            // GetValue() -- the FULL packed id+version, not GetID()
                            // (which strips the version bits, see Entity.hpp) -- so
                            // the consumer (EditorApp::RouteLocator) can reconstruct
                            // via Astra::Entity(StorageType), which expects the
                            // packed value. No live entity has version 0, so a
                            // version-stripped id can never round-trip to a real
                            // selection.
                            d.locator  = Arcane::DiagLocator::Entity(
                                             static_cast<std::uint64_t>(e.GetValue()));
                            diagnostics.push_back(std::move(d));
                        }
                        else if (r == Detail::AddComponentResult::SkippedUnregistered)
                        {
                            ARC_WARN("scene load: component \"{}\" is reflected but not "
                                     "registered (plugin not loaded?) -- skipped on "
                                     "entity #{} (id {}, v{}); re-saving this scene "
                                     "will drop it permanently",
                                     it.key(), entityIndex, e.GetID(),
                                     static_cast<unsigned>(e.GetVersion()));
                            Arcane::Diagnostic d;
                            d.severity = Arcane::DiagSeverity::Error;
                            d.scope    = Arcane::DiagScope::Scene;
                            d.code     = "scene.component.unregistered";
                            d.message  = "Component \"" + std::string(it.key()) +
                                         "\" is reflected but not registered (plugin not "
                                         "loaded?) on entity #" + std::to_string(entityIndex);
                            d.detail   = "Re-saving this scene will drop it permanently.";
                            // GetValue() -- see the SkippedUnknownType branch above
                            // for why this must be the packed value, not GetID().
                            d.locator  = Arcane::DiagLocator::Entity(
                                             static_cast<std::uint64_t>(e.GetValue()));
                            diagnostics.push_back(std::move(d));
                        }
                    }
                }
                created.push_back(e);
                ++entityIndex;
            }

            // Unconditional: an empty vector RETRACTS the previous load's rows,
            // which is exactly the clean-reload case. LoadJson takes no path/
            // asset id -- callers hand it an already-parsed nlohmann::json --
            // so there is no per-scene identifier in scope here to key on; a
            // stable constant key is the documented fallback (see Task 7 brief).
            Arcane::Diagnostics::Publish("scene", diagnostics);

            for (size_t i = 0; i < entities.size(); ++i)
            {
                const auto& entry = entities[i];

                const auto pit = entry.find("parent");
                if (pit != entry.end() && pit->is_number_integer())
                {
                    const int parent = pit->get<int>();
                    if (parent >= 0 && parent < static_cast<int>(created.size()))
                        reg.SetParent(created[i], created[static_cast<size_t>(parent)]);
                }

                const auto lit = entry.find("links");
                if (lit != entry.end() && lit->is_array())
                {
                    for (const auto& lj : *lit)
                    {
                        if (!lj.is_number_integer()) continue;
                        const int j = lj.get<int>();
                        if (j >= 0 && j < static_cast<int>(created.size()))
                            reg.AddLink(created[i], created[static_cast<size_t>(j)]);
                    }
                }
            }

            if (!created.empty())
                reg.SetResource<SceneRoot>(SceneRoot{created.front()});
            return true;
        }
        catch (const nlohmann::json::exception&)
        {
            return false;
        }
    }
}

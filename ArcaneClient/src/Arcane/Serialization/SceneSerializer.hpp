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
//     "entities": [
//       { "components": { "<TypeName>": { ...fields... }, ... },
//         "parent": <index|-1>,
//         "links":  [ <index>, ... ] }          // optional; non-hierarchical
//     ]
//   }
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

#include <new>
#include <string>
#include <vector>

namespace Arcane::Scene
{
    // Bumped when the on-disk schema layout changes. v1 was the implicit,
    // hardcoded local+sprite+parent format; v2 is the reflection-driven roster.
    inline constexpr int kSceneJsonVersion = 2;

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
    // keyed by reflected type name, plus parent + non-hierarchical links. Returns
    // { "version", "entities": [...] }.
    inline nlohmann::json SaveJson(const Astra::Registry& reg)
    {
        nlohmann::json doc;
        doc["version"] = kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array();

        const SceneRoot* sceneRoot = reg.GetResource<SceneRoot>();
        if (!sceneRoot) return doc;

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
                ReflectionJsonWriter writer(cj);
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
        // reflection reader latched an unsupported-field-type diagnostic (E02-3)
        // while populating it -- this must fail the whole load, not silently
        // install a partially-populated component with no signal anything went
        // wrong.
        enum class AddComponentResult { SkippedUnknownType, SkippedUnregistered, Added, Error };

        // Add-by-descriptor factory: instantiate a component by its reflected type
        // name and populate it from JSON via the reflection reader. Never throws.
        inline AddComponentResult AddComponentByTypeName(Astra::Registry& reg, Astra::ComponentRegistry* creg,
                                           Astra::Entity e, const std::string& typeName,
                                           const nlohmann::json& fields)
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
    inline bool LoadJson(Astra::Registry& reg, const nlohmann::json& doc)
    {
        try
        {
            if (!doc.is_object() || !doc.contains("entities")) return false;

            // Version gate: a missing/wrong-typed/mismatched version is reported
            // (clean false), never silently mis-parsed as the current schema.
            const auto vit = doc.find("version");
            if (vit == doc.end() || !vit->is_number_integer()) return false;
            if (vit->get<int>() != kSceneJsonVersion) return false;

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

                        const Detail::AddComponentResult r =
                            Detail::AddComponentByTypeName(reg, creg, e, it.key(), *fields);
                        if (r == Detail::AddComponentResult::Error)
                            return false;   // E02-3: unsupported field type latched -- fail loud

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

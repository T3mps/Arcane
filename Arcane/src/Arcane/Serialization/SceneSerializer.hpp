#pragma once

// Scene JSON: a versioned, reflection-driven, inspectable/editable peer of the
// binary (SceneModule) runtime persistence. It round-trips an ARBITRARY reflected
// component roster -- every reflected + serializable component on each entity is
// emitted keyed by its reflected type name, and loaded back through an add-by-
// descriptor factory -- instead of a hardcoded LocalTransform+SpriteRenderer pair.
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
                    continue;
                nlohmann::json cj;
                ReflectionJsonWriter writer(cj);
                desc->visitFields(instance, writer);   // writer READS; const_cast is safe
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
        // Add-by-descriptor factory: instantiate a component by its reflected type
        // name and populate it from JSON via the reflection reader. Returns false
        // if the type is not reflected or not registered as a component (caller
        // decides whether to skip). Never throws.
        inline bool AddComponentByTypeName(Astra::Registry& reg, Astra::ComponentRegistry* creg,
                                           Astra::Entity e, const std::string& typeName,
                                           const nlohmann::json& fields)
        {
            const Astra::TypeMeta* meta = Astra::GetMetaByName(typeName);
            if (!meta) return false;
            const Astra::ComponentDescriptor* desc = creg->GetComponentDescriptorByHash(meta->typeHash);
            if (!desc) return false;   // reflected but not registered as a component

            const std::size_t bytes = desc->size ? desc->size : 1;
            const std::align_val_t align{ desc->alignment ? desc->alignment : 1 };
            void* buf = ::operator new(bytes, align);
            desc->DefaultConstruct(buf);
            if (desc->visitFields)
            {
                ReflectionJsonReader reader(fields);
                desc->visitFields(buf, reader);   // tolerant of missing/wrong-typed leaf data
            }
            reg.AddComponentByID(e, desc->id, buf, desc->size);
            desc->Destruct(buf);
            ::operator delete(buf, align);
            return true;
        }
    }

    // Returns false (never throws) on a malformed document, a missing/mismatched
    // schema version, or a structurally invalid entity list. Wrong-typed leaf
    // fields are tolerated by the guarded ReflectionJsonReader (left at defaults).
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

            for (const auto& entry : entities)
            {
                if (!entry.is_object()) return false;

                Astra::Entity e = reg.CreateEntity();
                const auto cit = entry.find("components");
                if (cit != entry.end() && cit->is_object())
                {
                    for (auto it = cit->begin(); it != cit->end(); ++it)
                    {
                        if (!it.value().is_object()) continue;
                        Detail::AddComponentByTypeName(reg, creg, e, it.key(), it.value());
                        // Unknown/unregistered types are skipped (cannot instantiate);
                        // a structurally valid scene still loads.
                    }
                }
                created.push_back(e);
            }

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

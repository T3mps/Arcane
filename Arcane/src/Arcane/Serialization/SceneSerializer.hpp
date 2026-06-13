#pragma once

// Scene JSON (M4 scope: the slice's known component roster -- LocalTransform +
// SpriteRenderer -- plus parent links). General schema-driven scene JSON for
// arbitrary component rosters is a Grimoire-era concern. Binary (SceneModule)
// remains the primary runtime persistence; JSON is the inspectable/editable peer.

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/ReflectionJson.hpp>

#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <Json.hpp>

#include <vector>

namespace Arcane::Scene
{
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

    // Emits { "entities": [ { "local": {...}, "sprite": {...}, "parent": <index|-1> } ] }
    // ordered root-first (BFS), so parent indices always refer to an earlier entry.
    inline nlohmann::json SaveJson(const Astra::Registry& reg)
    {
        nlohmann::json doc;
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

        auto& mutableReg = const_cast<Astra::Registry&>(reg);  // GetComponent is non-const
        for (Astra::Entity e : order)
        {
            nlohmann::json entry;
            if (auto* lt = mutableReg.GetComponent<LocalTransform>(e))
            {
                nlohmann::json j;
                ReflectionJsonWriter w(j);
                ForEachReflectedField<LocalTransform>(lt, w);
                entry["local"] = std::move(j);
            }
            if (auto* sr = mutableReg.GetComponent<SpriteRenderer>(e))
            {
                nlohmann::json j;
                ReflectionJsonWriter w(j);
                ForEachReflectedField<SpriteRenderer>(sr, w);
                entry["sprite"] = std::move(j);
            }
            entry["parent"] = indexOf(mutableReg.GetParent(e));
            doc["entities"].push_back(std::move(entry));
        }
        return doc;
    }

    inline bool LoadJson(Astra::Registry& reg, const nlohmann::json& doc)
    {
        if (!doc.contains("entities")) return false;
        const auto& entities = doc["entities"];

        std::vector<Astra::Entity> created;
        created.reserve(entities.size());

        for (const auto& entry : entities)
        {
            Astra::Entity e = reg.CreateEntity();
            if (entry.contains("local"))
            {
                LocalTransform lt;
                ReflectionJsonReader r(entry["local"]);
                ForEachReflectedField<LocalTransform>(&lt, r);
                reg.AddComponent<LocalTransform>(e, lt);
                reg.AddComponent<WorldTransform>(e, WorldTransform{});
            }
            if (entry.contains("sprite"))
            {
                SpriteRenderer sr;
                ReflectionJsonReader r(entry["sprite"]);
                ForEachReflectedField<SpriteRenderer>(&sr, r);
                reg.AddComponent<SpriteRenderer>(e, sr);
            }
            created.push_back(e);
        }

        for (size_t i = 0; i < entities.size(); ++i)
        {
            const int parent = entities[i].value("parent", -1);
            if (parent >= 0 && parent < static_cast<int>(created.size()))
                reg.SetParent(created[i], created[static_cast<size_t>(parent)]);
        }

        if (!created.empty())
            reg.SetResource<SceneRoot>(SceneRoot{created.front()});
        return true;
    }
}

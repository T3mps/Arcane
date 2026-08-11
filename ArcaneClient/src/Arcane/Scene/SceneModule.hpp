#pragma once

// Scene registration + binary persistence. Binary save/load is the scene's
// primary runtime persistence (and the future hot-reload path); it is one call
// over Astra's integrated, CRC'd, versioned Registry::Save/Load.

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/SerializationError.hpp>

#include <filesystem>
#include <memory>

namespace Arcane
{
    inline void RegisterSceneComponents(Astra::ComponentRegistry& creg)
    {
        creg.RegisterComponent<Transform>();
        creg.RegisterComponent<WorldTransform>();
        creg.RegisterComponent<PreviousTransform>();
        creg.RegisterComponent<SpriteRenderer>();
        creg.RegisterComponent<PostProcess>();
        creg.RegisterComponent<Identity>();
        creg.RegisterComponent<Hidden>();
        // APPEND new engine components here rather than inserting above. Ids are a
        // monotonic first-touch counter (Runtime.cpp explains the numbering), so
        // inserting shifts every id after it. Nothing in-process is hurt by a shift
        // today, but appending costs nothing and keeps the ids of everything already
        // registered stable.
        creg.RegisterComponent<Camera>();
    }

    inline void RegisterSceneComponents(Astra::Registry& reg)
    {
        RegisterSceneComponents(*reg.GetComponentRegistry());
    }

    namespace Scene
    {
        inline Astra::Result<void, Astra::SerializationError>
            SaveBinary(const Astra::Registry& reg, const std::filesystem::path& path)
        {
            return reg.Save(path);
        }

        inline Astra::Result<std::unique_ptr<Astra::Registry>, Astra::SerializationError>
            LoadBinary(const std::filesystem::path& path,
                       std::shared_ptr<Astra::ComponentRegistry> components)
        {
            return Astra::Registry::Load(path, std::move(components));
        }
    }
}

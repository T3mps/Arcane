#pragma once

// Registry resources (singletons) for the scene slice. RenderContext2D and
// TextureTable are set by the host each frame; SceneRoot marks the subtree that
// IS the scene.

#include <Astra/Entity/Entity.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>

namespace Arcane
{
    struct SceneRoot { Astra::Entity entity; };

    struct RenderContext2D
    {
        class Batcher2D* batcher = nullptr;   // set by the host between Begin/End
        glm::vec2        cameraOffset{0.0f, 0.0f};  // world->screen; world unit == canvas px
    };

    struct TextureTable
    {
        // textureId 0 is reserved for "untextured". Full Assets integration deferred.
        std::unordered_map<uint32_t, nvrhi::ITexture*> textures;

        nvrhi::ITexture* Resolve(uint32_t id) const
        {
            if (id == 0) return nullptr;
            auto it = textures.find(id);
            return it != textures.end() ? it->second : nullptr;
        }
    };
}

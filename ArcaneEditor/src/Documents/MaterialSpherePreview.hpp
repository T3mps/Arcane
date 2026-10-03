#pragma once

// The MESH-surface material preview: a lit UV sphere in the material's
// resolved baseColor (+ its albedo's bindless slot), framed by a fixed camera
// and one directional light. ONE definition, two consumers (T3-D6):
//   * MaterialPreviewHarvester -- the Asset Browser's 64px thumbnail, from
//     the SAVED params (what MeshMaterialCache resolves off disk);
//   * ShaderEditorDocument     -- the document's live 512px preview, from the
//     bound instance's CURRENT params (so an edit shows before the save).
// Sharing the constants is what keeps the thumbnail and the preview one
// picture (spec s5.3 amendment, 2026-10-02). Header-only and device-free: the
// caller owns the vehicle, its mesh supply (which must answer
// kMaterialPreviewSphereId with BuildMaterialPreviewSphere()'s geometry) and
// the instance storage the scene's span borrows.

#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Nri/nodes/MeshNode.hpp>   // MeshInstance, MeshSceneDesc
#include <Arcane/Scene/SceneCamera.hpp>           // PerspectiveProjection

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <span>

namespace Arcane::Editor
{
    // The sphere's mesh-supply key ('SPHR'): never a project asset Guid.
    inline constexpr Arcane::Guid kMaterialPreviewSphereId{ 0x53504852ull, 1ull };

    // A 0.5 m sphere: 2.0 m back at 35 degrees it fills ~78% of the square.
    [[nodiscard]] inline Arcane::MeshData BuildMaterialPreviewSphere()
    {
        return Arcane::BuildUvSphere(0.5f, 24, 32);
    }

    // The one instance: the sphere at the origin in the material's colour.
    [[nodiscard]] inline Arcane::MeshInstance MaterialPreviewSphereInstance(const glm::vec4& baseColor,
                                                                            std::uint32_t materialSlot)
    {
        Arcane::MeshInstance mi;
        mi.mesh = kMaterialPreviewSphereId;
        mi.model = glm::mat4(1.0f);
        mi.baseColor = baseColor;
        mi.materialSlot = materialSlot;
        return mi;
    }

    // The camera + light around `instances` (borrowed: it must outlive the
    // RenderFrame call). Right-handed, [0,1] depth -- SceneCamera's own
    // convention, and the only one MeshNode's shaders are built for.
    [[nodiscard]] inline Arcane::MeshSceneDesc MaterialPreviewSphereScene(std::span<const Arcane::MeshInstance> instances)
    {
        Arcane::MeshSceneDesc scene;
        scene.instances = instances;
        scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        scene.projection = Arcane::PerspectiveProjection(35.0f, 1.0f, 0.05f, 10.0f);
        scene.lightDirection = glm::vec3(0.45f, 0.7f, 0.8f);   // TOWARD the light
        scene.lightColor = glm::vec3(1.0f);
        scene.ambient = glm::vec3(0.12f);
        return scene;
    }
}

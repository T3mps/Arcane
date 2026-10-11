#pragma once

// The asset documents' tuning as cvars (settings sweep S6-35, inventory Part 3
// "Graph/Node editor", "Inspector" and "Documents"): editor.shader.* (the
// shader editor and the preview checkerboard), editor.shader.chainLayout.*,
// editor.mesh.* and editor.mesh.primitiveRanges.* (the mesh document),
// editor.sprite.* (the sprite document), editor.preview.* (the material
// sphere) and editor.preview.light.* -- the ONE preview light (R1) the
// material sphere, the Asset Browser thumbnails and the mesh document share.
// Per-machine editor preferences.
//
// Plain structs, no settings machinery: ARC_SETTINGS and the reflection live in
// DocumentSettings.cpp. Every default is the pre-sweep literal it replaced,
// except the mesh document's light, which moved from normalize(0.4, 1, 0.3) to
// the shared direction (R1; no golden captures the mesh document).

#include <Arcane/Config/CVarTypes.hpp>

#include <cstdint>

namespace Arcane::Editor
{
    // editor.preview.light.* -- the direction is TOWARD the light, in view-
    // independent world space; the sphere and the thumbnails use it as given
    // (un-normalized, as before), the mesh document normalizes it.
    struct EditorPreviewLightSettings
    {
        CVarVec3  direction{ 0.45f, 0.7f, 0.8f };
        float     ambient = 0.12f;
        CVarColor color{ 1.0f, 1.0f, 1.0f, 1.0f };
    };

    // editor.preview.* -- the material sphere (MaterialSpherePreview.hpp).
    struct EditorPreviewSettings
    {
        std::int32_t sphereRings    = 24;
        std::int32_t sphereSegments = 32;
        float        sphereFov      = 35.0f;   // vertical, degrees
    };

    // editor.shader.*
    struct ShaderEditorSettings
    {
        std::int32_t navHistoryMax     = 32;
        std::int32_t previewResolution = 512;   // square, px; a document reads it when it opens
        std::int32_t renameListMax     = 8;
        std::int32_t bodyPreviewLines  = 8;
        std::int32_t bodyPreviewChars  = 48;
        // The preview checkerboard's square side in render-target texels (32 at
        // the default 512 preview = 16 cells), not UI chrome (S6-44).
        float        previewCheckerCell = 32.0f;
        // The checkerboard behind the shader editor's preview and the material
        // thumbnails: its light squares, and a sprite material's quad as a
        // fraction of the preview's side (S6-45: frozen names, moved here from
        // editor.preview.*).
        CVarColor    previewCheckerLight{ 0.16f, 0.16f, 0.19f, 1.0f };
        float        previewCheckerSpriteScale = 0.8f;
        // A pass node's live thumbnail, canvas units: inside a canvas node, so
        // it scales with the graph zoom, not UI chrome (S6-45).
        float        passThumbPx = 72.0f;
    };

    // editor.shader.chainLayout.* -- where a material's pass chain is first laid
    // out on its canvas (canvas coordinates written into the .arcmat, S6-45):
    // the base at (originX, originY), pass k at originX + pitchX * k, the Output
    // after the last pass, and the Scene source at the base + sceneOffset.
    struct ShaderChainLayoutSettings
    {
        float originX      = 40.0f;
        float originY      = 40.0f;
        float pitchX       = 190.0f;
        float sceneOffsetX = -170.0f;   // left of the base
        float sceneOffsetY = 90.0f;     // below it
    };

    // editor.mesh.* -- the preview, and the capsule length ratio's drag speed.
    struct MeshDocSettings
    {
        std::int32_t previewResolution     = 512;   // square, px; a document reads it when it opens
        float        previewFov            = 45.0f; // vertical, degrees
        float        previewMargin         = 1.5f;  // camera distance over the bounding-sphere fit
        float        capsuleRatioDragSpeed = 0.02f;
    };

    // editor.mesh.primitiveRanges.* -- the bounds the mesh document's rows offer
    // for a procedural source (inventory "MeshDocument.cpp:699-716"; S6-45).
    // Each minimum's cvar range starts at ValidateMeshAsset's floor for that
    // field (1 / 3 / 3 / 2 / 1), so no setting can offer a value the asset
    // would refuse; a maximum below its minimum collapses to the minimum.
    struct EditorMeshPrimitiveRangesSettings
    {
        std::int32_t planeSubdivisionsMin  = 1;
        std::int32_t planeSubdivisionsMax  = 64;
        std::int32_t sphereRingsMin        = 3;
        std::int32_t sphereRingsMax        = 128;
        std::int32_t segmentsMin           = 3;
        std::int32_t segmentsMax           = 128;
        std::int32_t capsuleRingsMin       = 2;   // a two-step arc still closes a hemisphere
        std::int32_t capsuleRingsMax       = 64;
        float        capsuleLengthRatioMin = 1.0f;
        float        capsuleLengthRatioMax = 20.0f;
    };

    // editor.sprite.*
    struct SpriteDocSettings
    {
        float ppuDragSpeed   = 0.5f;
        float ppuMin         = 1.0f;
        float ppuMax         = 4096.0f;
        float pivotDragSpeed = 0.005f;
    };
}

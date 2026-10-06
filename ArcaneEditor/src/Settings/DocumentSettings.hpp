#pragma once

// The asset documents' tuning as cvars (settings sweep S6-35, inventory Part 3
// "Graph/Node editor", "Inspector" and "Documents"): editor.shader.* (the
// shader editor), editor.mesh.* (the mesh document), editor.sprite.* (the
// sprite document), editor.preview.* (the material sphere and the preview
// checkerboard) and editor.preview.light.* -- the ONE preview light (R1) the
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

    // editor.preview.* -- the material sphere (MaterialSpherePreview.hpp) and
    // the checkerboard behind the shader editor's preview and the thumbnails.
    struct EditorPreviewSettings
    {
        std::int32_t sphereRings    = 24;
        std::int32_t sphereSegments = 32;
        float        sphereFov      = 35.0f;   // vertical, degrees
        CVarColor    checkerLight{ 0.16f, 0.16f, 0.19f, 1.0f };
        float        checkerExtent  = 0.8f;    // a sprite material's quad, as a fraction of the preview
    };

    // editor.shader.*
    struct ShaderEditorSettings
    {
        std::int32_t navHistoryMax     = 32;
        std::int32_t previewResolution = 512;   // square, px; a document reads it when it opens
        std::int32_t renameListMax     = 8;
        std::int32_t bodyPreviewLines  = 8;
        std::int32_t bodyPreviewChars  = 48;
        float        nodePreviewMinPx  = 96.0f;
        float        dragSpeed         = 0.01f;
        float        rangeDragSpeed    = 0.05f;
    };

    // editor.mesh.* -- the preview and the authoring maxima of the procedural
    // sources (the minima 1/2/3/3/1 are geometric, not preferences).
    struct MeshDocSettings
    {
        std::int32_t previewResolution     = 512;   // square, px; a document reads it when it opens
        float        previewFov            = 45.0f; // vertical, degrees
        float        previewMargin         = 1.5f;  // camera distance over the bounding-sphere fit
        std::int32_t subdivMax             = 64;
        std::int32_t ringsMax              = 128;
        std::int32_t capsuleRingsMax       = 64;
        std::int32_t segmentsMax           = 128;
        float        capsuleRatioMax       = 20.0f;
        float        capsuleRatioDragSpeed = 0.02f;
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

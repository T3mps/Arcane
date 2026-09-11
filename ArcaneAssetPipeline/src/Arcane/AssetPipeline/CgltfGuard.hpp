#pragma once

// Arcane::AssetPipeline::CgltfDataGuard -- the cgltf_data* RAII guard, shared by
// MeshImporter.cpp and GltfSurvey.cpp (F2c Task 13). Both TUs run the SAME
// cgltf_parse -> (extensionsRequired check) -> cgltf_load_buffers -> cgltf_validate
// front half (MeshImporter.hpp's own comment names the refusal ladder rungs 1-4;
// SurveyGltf reuses them verbatim, minus rung 5's drawability gate, which is
// mesh-geometry-specific and has no bearing on images/materials). Extracted here
// rather than left as MeshImporter.cpp's own private copy, so the guard has exactly
// ONE definition instead of two near-copies drifting apart -- the failure mode this
// task's own brief calls out by name.
//
// FILE-LOCAL: not part of ArcaneAssetPipeline's public surface (no header outside
// this directory's two importer TUs should reach for it). Written FIRST, before any
// early-return refusal in a caller: a function with several early-return refusals and
// a hand-written cgltf_free on each is exactly where a leak hides. Never copied or
// moved (each caller owns exactly one, as a local) -- copy is deleted so an accidental
// copy can't double-free; the destructor is the only cgltf_free call site either TU
// needs.

#include <cgltf.h>

namespace Arcane::AssetPipeline
{
    struct CgltfDataGuard
    {
        cgltf_data* data = nullptr;
        ~CgltfDataGuard() { if (data) cgltf_free(data); }
        CgltfDataGuard() = default;
        CgltfDataGuard(const CgltfDataGuard&) = delete;
        CgltfDataGuard& operator=(const CgltfDataGuard&) = delete;
    };
}

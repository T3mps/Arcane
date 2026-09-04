#pragma once

// arccook::DdsDump -- writes a standard, spec-compliant DDS file from an already-cooked
// .arcart artifact, for EYEBALL-ONLY debugging (arccook's --dump-dds). This is NEVER a
// load path: nothing in the engine reads .dds anywhere -- a real DDS viewer (RenderDoc,
// PIX, Visual Studio's own image viewer, ...) is the intended consumer. Lives in
// arccook/, not ArcaneAssetPipeline: it is a CLI debug convenience, not part of the
// cook/staleness contract CookSession shares with the editor (Task 12).
//
// BC7 mips get a DX10 extension header (fourCC 'DX10', DXGI_FORMAT_BC7_UNORM or
// _UNORM_SRGB per the artifact's own `srgb` flag) -- BC7 has no legacy DDS FourCC of its
// own. RGBA8 mips get the classic (non-DX10) uncompressed pixel format: every legacy DDS
// reader already understands R8G8B8A8 masks, so no DX10 header is needed there.

#include <filesystem>

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>

namespace arccook
{
    // Writes `artifact`'s full mip chain (mip 0 first, exactly as already stored in its
    // own payload/MipDesc table) to `path` as a standard DDS file. Returns false on any
    // IO failure; `path` is not guaranteed to exist on false.
    [[nodiscard]] bool WriteDds(const std::filesystem::path& path,
                                 const Arcane::AssetPipeline::LoadedArtifact& artifact);
}

// libFuzzer harness: .arcsprite loading (ArcaneCore/src/Arcane/Sprite) and the
// Guid text parser every asset JSON id goes through -- the .arcsprite "id" and
// "texture" fields, native-JSON "id"s and imported-asset .meta sidecars'
// "guid" (AssetRegistry.cpp reads those with the same Guid::FromString).
//
// The input is the .arcsprite file. LoadSpriteAsset must refuse bad input by
// returning nullopt (nlohmann runs with exceptions off there); UBSan catches an
// out-of-range number narrowed into a float field. An accepted sprite then
// goes through ComputeSpriteGeom, the consumer that turns it into UVs.
// The input is also fed to Guid::FromString as a string; an accepted guid must
// round-trip through ToString.

#include <Arcane/Guid.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{
    [[noreturn]] void Fail(const char* property, int line)
    {
        std::fprintf(stderr, "sprite_fuzz: property violated (line %d): %s\n", line, property);
        std::abort();
    }
#define FUZZ_CHECK(cond) do { if (!(cond)) Fail(#cond, __LINE__); } while (0)
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const std::filesystem::path file = std::filesystem::temp_directory_path() /
        ("arcane-sprite-fuzz-" + std::to_string(::getpid()) + ".arcsprite");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    if (const std::optional<Arcane::SpriteAssetData> sprite = Arcane::LoadSpriteAsset(file))
    {
        FUZZ_CHECK(sprite->ppu > 0.0f);
        (void)Arcane::ComputeSpriteGeom(*sprite, 64, 32);
        (void)Arcane::ComputeSpriteGeom(*sprite, 0, 0);
    }

    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (const std::optional<Arcane::Guid> g = Arcane::Guid::FromString(text))
    {
        const std::optional<Arcane::Guid> back = Arcane::Guid::FromString(g->ToString());
        FUZZ_CHECK(back && *back == *g);
    }
    return 0;
}

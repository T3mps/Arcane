#pragma once

#include <Arcane/Core/Constant.hpp>
#include <Arcane/Core/Api.hpp>
#include <cstdint>

namespace Arcane
{
    ARC_CONSTANT("ABI: the game-module ABI version; the host refuses a mismatched module")
    inline constexpr std::uint32_t kGamePluginABIVersion = 61;
    ARC_CORE_API std::uint32_t PluginABIVersion();

    // The compiler-derived layout digest (Core/AbiManifest.hpp, AbiManifestMix() at
    // the default Astra defines). It must equal the integer in build/abi-digest.txt;
    // `arcane-abi-digest --check` and the [abi] tests enforce that. Regenerate with
    // `arcane-abi-digest --write` after rebuilding the tool, then copy the value here.
    ARC_CONSTANT("ABI: compiler-derived layout digest; regenerate with arcane-abi-digest --write")
    inline constexpr std::uint64_t kGamePluginABIDigest = 0x18adaa4dbd9da25eull;
}

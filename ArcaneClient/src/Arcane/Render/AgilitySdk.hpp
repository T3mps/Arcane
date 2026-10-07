#pragma once

#include <Arcane/Core/Constant.hpp>

// The D3D12 Agility SDK handshake (settings inventory R3: a must-match pair
// that every EXE spelled out). The D3D12 loader reads two EXPORTED data
// symbols from the EXE and redirects device creation into the vendored
// D3D12Core.dll under .\D3D12\. The version MUST equal the vendored package
// (ThirdParty/AgilitySDK/README.md pins 1.619.x -> 619); NRI logging "Using
// ID3D12Device10+" is the proof the redirect took. CONSTANT: changing it
// without re-vendoring the DLLs breaks device creation.
//
// Use: ARC_AGILITY_SDK_EXPORTS(); once, at global scope, in an EXE's main
// translation unit. The loader ignores exports from a DLL.

namespace Arcane::AgilitySdk
{
    ARC_CONSTANT("must match the vendored Agility SDK package (ThirdParty D3D12Core.dll)")
    inline constexpr unsigned kVersion = 619;
    inline constexpr const char* kPath = ".\\D3D12\\";
}

#if defined(_WIN32)
#define ARC_AGILITY_SDK_EXPORTS()                                                                   \
    extern "C" __declspec(dllexport) extern const unsigned D3D12SDKVersion = ::Arcane::AgilitySdk::kVersion; \
    extern "C" __declspec(dllexport) extern const char* D3D12SDKPath = ::Arcane::AgilitySdk::kPath
#else
#define ARC_AGILITY_SDK_EXPORTS() static_assert(true, "no Agility SDK off Windows")
#endif

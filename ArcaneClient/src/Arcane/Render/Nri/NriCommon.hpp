#pragma once

// NRI substrate (Phase 1, Task 4): result-check discipline, callback-to-
// latch wiring, and one-line device identity logging. Every later NRI task
// in ArcaneClient/src/Arcane/Render/Nri/ builds on these three symbols, so
// the signatures here are load-bearing -- see
// .superpowers/sdd/2026-08-13-nri-phase1-substrate/task-4-brief.md.
//
// Include order: NRIDeviceCreation.h declares nri::Message with an
// enumerator literally named ERROR, and <windows.h> (dragged in
// transitively by Arcane/Base/Log.hpp -> spdlog) #defines ERROR via
// wingdi.h. Once that macro is live, every later textual "ERROR" in the
// including translation unit -- even a qualified nri::Message::ERROR --
// gets corrupted by preprocessor substitution. Keep the NRI includes first
// in this header and in NriCommon.cpp.
#include <NRI.h>                          // vendored NRI include root: nri::Result, nri::Device
#include <Extensions/NRIDeviceCreation.h> // nri::CallbackInterface, nri::Message

#include <Arcane/Base/Api.hpp>

namespace Arcane
{
    // Every nri call that returns nri::Result goes through this. Logs the
    // failing expression + result name at ERROR and bumps RenderErrorCount
    // (the 0/0 gate latch), returns false on failure. Never throws.
    ARCANE_API bool NriCheckImpl(nri::Result result, const char* expr, const char* file, int line);
    #define ARC_NRI_CHECK(expr) ::Arcane::NriCheckImpl((expr), #expr, __FILE__, __LINE__)

    // Install into DeviceCreationDesc/wrapper descs: routes NRI's
    // MessageCallback into ARC_INFO/WARN/ERROR by severity, ERRORs bump
    // RenderErrorCount. Mirrors the NVRHI message-callback latch
    // (DeviceD3D12.cpp's NvrhiMessageCallback) -- same gate, new producer.
    ARCANE_API nri::CallbackInterface MakeNriCallbacks() noexcept;

    // One INFO line: nriVersion + which backend + validation on/off.
    ARCANE_API void LogNriIdentity(nri::Device& device);
}

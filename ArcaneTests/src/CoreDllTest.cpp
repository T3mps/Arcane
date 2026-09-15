// Core-DLL split (spec docs/specs/2026-09-15-core-dll-split-design.md s1, s8):
// which DLL DEFINES a symbol is the whole point of the export-macro audit, so pin
// it directly -- the address the exe resolves for a Core symbol lies inside
// ArcaneCore.dll's image, and a Client symbol's inside ArcaneClient.dll's.
// GetModuleHandleEx(FROM_ADDRESS) is the OS's own answer to "which module owns
// this address"; no psapi needed.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputActions.hpp>        // Client-resident, and it logs (ARC_WARN)
#include <Arcane/Render/RenderErrorLatch.hpp>   // RenderErrorCount -- a Client export

#include <Json.hpp>
#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <memory>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{
    HMODULE OwnerOf(const void* addr)
    {
        HMODULE h = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(addr), &h);
        return h;
    }
}

TEST_CASE("ArcaneCore.dll is a loaded module and defines the Core surface", "[core-dll]")
{
    const HMODULE core   = ::GetModuleHandleW(L"ArcaneCore.dll");
    const HMODULE client = ::GetModuleHandleW(L"ArcaneClient.dll");
    REQUIRE(core != nullptr);
    REQUIRE(client != nullptr);
    CHECK(core != client);

    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::Log::Engine))       == core);
    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::Diagnostics::SetSink)) == core);
    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::BuildInfo))          == core);
    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::RenderErrorCount))   == client);
}

TEST_CASE("one engine logger: the exe and ArcaneClient.dll see the same spdlog instance", "[core-dll]")
{
    // PROVES: a log line emitted from INSIDE ArcaneClient.dll reaches a sink this
    // EXE pushed, through a logger that lives in a THIRD module (ArcaneCore.dll).
    //
    // This is the half of the split most likely to rot silently. spdlog is
    // header-only here, so each module owns its own registry (Log.hpp's opening
    // comment): if ArcaneClient.dll ever resolved Log::Engine() to a local copy
    // instead of Core's export, everything would still LOG -- to a second,
    // sinkless instance -- and only the absence of lines would betray it. So
    // assert the crossing directly rather than any property of Engine() alone.
    //
    // Vehicle: Arcane::InputActions, a Client-resident subsystem for the whole
    // arc (ArcaneClient/src/Arcane/Input/), whose path compiler emits
    // ARC_WARN("input: unknown control path '{}' in {}/{}") for a binding path
    // that does not start with '<' (InputActions.cpp, CompileSinglePath). One
    // map, one action, one bogus binding is the whole fixture -- no device, no
    // window, no Runtime. Sink capture follows PluginHostTest.cpp's pattern
    // (push, provoke, erase).
    const HMODULE core   = ::GetModuleHandleW(L"ArcaneCore.dll");
    const HMODULE client = ::GetModuleHandleW(L"ArcaneClient.dll");
    REQUIRE(core != nullptr);
    REQUIRE(client != nullptr);

    std::string captured;
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [&](const spdlog::details::log_msg& m) { captured.append(m.payload.data(), m.payload.size()).push_back('\n'); });
    Arcane::Log::Engine()->sinks().push_back(sink);

    {
        auto actions = Arcane::InputActions::Create();   // the impl lives in ArcaneClient.dll
        REQUIRE(actions != nullptr);
        const nlohmann::json doc = nlohmann::json::parse(R"({
            "actionMaps": [{
                "name": "CoreDllProbe",
                "actions": [{
                    "name": "Bogus",
                    "bindings": [{ "path": "bogus/whatever" }]
                }]
            }]
        })");
        CHECK(actions->LoadJson(doc));   // the map itself is well-formed; only the PATH is not
    }

    {
        auto& sinks = Arcane::Log::Engine()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
    }

    INFO(captured);
    // The line crossed ArcaneClient.dll -> Core's logger -> this exe's sink.
    CHECK(captured.find("input: unknown control path") != std::string::npos);
    CHECK(captured.find("CoreDllProbe/Bogus") != std::string::npos);
    // ...and the logger it travelled through is genuinely Core's, not a Client
    // or exe copy -- without this the line above would pass just as happily on a
    // per-module logger, which is the bug being excluded.
    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::Log::Engine)) == core);
}

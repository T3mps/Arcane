#pragma once

// The ONLY unmangled C surface in the engine. The game DLL exports the seven
// GamePlugin_* functions; the host resolves them by name into a PluginVTable.
// EngineContext is the C++ facade handed to the plugin (Arcane::Runtime).

#include <Arcane/Base/Api.hpp>

#include <cstdint>

namespace Astra { class TypeContext; class IWorkScheduler; class BinaryWriter; class BinaryReader; }

namespace Arcane
{
    class Runtime;  // defined in Arcane.dll; the plugin holds it opaquely via EngineContext

    // Bump on ANY change to EngineContext layout or the entry-point set/signatures.
    inline constexpr uint32_t kGamePluginABIVersion = 1;

    struct EngineContext
    {
        uint32_t               abiVersion;     // == kGamePluginABIVersion at the host
        Astra::TypeContext*    typeContext;    // plugin calls Astra::SetTypeContext(this) FIRST
        Astra::IWorkScheduler* workScheduler;  // the one engine enkiTS adapter (shared instance)
        Arcane::Runtime*       engine;         // registry, schedulers, snapshot/restore, render ctx
    };

    namespace PluginEntry
    {
        inline constexpr const char* kABIVersion  = "GamePlugin_ABIVersion";
        inline constexpr const char* kInit        = "GamePlugin_Init";
        inline constexpr const char* kShutdown    = "GamePlugin_Shutdown";
        inline constexpr const char* kFixedUpdate = "GamePlugin_FixedUpdate";
        inline constexpr const char* kUpdate      = "GamePlugin_Update";
        inline constexpr const char* kSaveState   = "GamePlugin_SaveState";
        inline constexpr const char* kLoadState   = "GamePlugin_LoadState";
    }

    // Host-side resolved function-pointer table for one loaded plugin image.
    struct PluginVTable
    {
        uint32_t (*ABIVersion)()                         = nullptr;
        bool     (*Init)(EngineContext*)                 = nullptr;
        void     (*Shutdown)()                           = nullptr;
        void     (*FixedUpdate)(double dt)               = nullptr;
        void     (*Update)(double dt, double alpha)      = nullptr;
        void     (*SaveState)(Astra::BinaryWriter&)      = nullptr;
        bool     (*LoadState)(Astra::BinaryReader&)      = nullptr;
    };
}

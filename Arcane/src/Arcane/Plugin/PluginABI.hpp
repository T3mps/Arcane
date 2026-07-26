#pragma once

// The ONLY unmangled C surface in the engine. The game DLL exports the seven
// GamePlugin_* functions; the host resolves them by name into a PluginVTable.
// EngineContext is the C++ facade handed to the plugin (Arcane::Runtime).

#include <Arcane/Base/Api.hpp>

#include <cstdint>

namespace Astra { class TypeContext; class BinaryWriter; class BinaryReader; }
namespace Mosaic { struct IWorkScheduler; }   // the shared data-parallel seam (Astra aliases this)

namespace Arcane
{
    class Runtime;  // defined in Arcane.dll; the plugin holds it opaquely via EngineContext
    struct ITaskExecutor;  // <Arcane/Jobs/TaskExecutor.hpp>; same enki pool, worker-index face

    // Bump on ANY change to EngineContext layout or the entry-point set/signatures.
    // v2 (2026-06-20): added the ImGui cross-DLL handoff fields below + GamePlugin_DrawUI.
    // v3 (2026-06-26): added taskExecutor (the ITaskExecutor face of the engine scheduler).
    // v4 (2026-07-10): foundations branch changed the cross-DLL C++ surface plugins
    //     consume through EngineContext: Runtime::SnapshotRegistry now returns
    //     Astra::Result (E02-4), Batcher2D gained the pure virtual RemoveTexture
    //     mid-vtable (E01-2), and Assets gained SetDevice before GetTexture. Any of
    //     these mismatched across the boundary is a vtable/stack smash; the version
    //     gate must reject a cross-build pairing.
    // v5 (2026-07-19): the re-vendored Astra migrated its threading seam to
    //     Mosaic, so workScheduler is now Mosaic::IWorkScheduler (per-lane worker id
    //     + FunctionRef callback) -- a DIFFERENT vtable than the prior Astra-native
    //     IWorkScheduler. A stale plugin would vtable-mismatch; reject the pairing.
    // v6 (2026-07-24): shader-editor Slice 8 changed the cross-DLL surface twice:
    //     Batcher2D gained RegisterMaterial/UpdateMaterial/SetGlobals/QuadMaterial
    //     mid-vtable, and SpriteRenderer grew a Guid `material` field (+16 bytes;
    //     plugins compile the header-only RenderSubmissionSystem + components).
    //     A stale plugin's SetLayer call lands on UpdateMaterial and its Quad on
    //     SetGlobals -- observed as an AV on project open; reject the pairing.
    // v7 (2026-07-24): LocalTransform renamed to Transform. The reflected type
    //     name IS the cross-module component identity (TypeID name hash +
    //     name-keyed scene JSON), so a stale plugin would register/query the
    //     old name and silently diverge from engine systems; reject the pairing.
    //     NOTE (2026-07-25, post arc slices 2+3, deliberately NO bump):
    //     OffscreenCanvas gained SetPostChain/SetPostGlobals APPENDED at the
    //     END of its vtable (every pre-existing slot index unchanged; only
    //     hosts -- in-tree, rebuilt with the engine -- call the new slots),
    //     and PostProcess is an ADDED component type (name-keyed identity; a
    //     stale plugin's roster simply lacks it -- typed views come back
    //     empty, nothing smashes). Neither changes EngineContext, the entry
    //     points, or any existing layout, so a v7<->v7 cross-build pairing
    //     stays memory-safe in both directions.
    //     Outliner slice 1 rides the same argument: EntityInfo + Hidden are
    //     two more appended name-keyed component types, and the Not<Hidden>
    //     filter in the plugin-compiled RenderSubmissionSystem is behavioral
    //     (a stale plugin just doesn't honor hiding). Still NO bump.
    inline constexpr uint32_t kGamePluginABIVersion = 7;

    // The ABI version compiled into the LOADED Arcane.dll -- i.e. the one the
    // plugin gate actually enforces at runtime.
    //
    // kGamePluginABIVersion above is a header constant, so every module bakes in
    // its OWN copy at compile time. A host exe reporting that constant reports
    // what IT was built against, which is not necessarily what the DLL beside it
    // enforces: a partially-updated install (fresh Arcane.dll, stale
    // ArcaneEditor.exe, or the reverse) stamps a number the runtime will reject.
    // Anything PUBLISHING the ABI -- the `--print-engine-info` probe the Arcane
    // Hub stamps into new .arcproj files -- must ask the DLL, not itself.
    ARCANE_API uint32_t PluginABIVersion();

    struct EngineContext
    {
        uint32_t               abiVersion;     // == kGamePluginABIVersion at the host
        Astra::TypeContext*    typeContext;    // plugin calls Astra::SetTypeContext(this) FIRST
        Mosaic::IWorkScheduler* workScheduler; // the one engine enkiTS adapter (shared instance)
        Arcane::ITaskExecutor* taskExecutor;   // SAME enki pool, worker-index ParallelFor (physics/general)
        Arcane::Runtime*       engine;         // registry, schedulers, snapshot/restore, render ctx

        // ImGui cross-DLL handoff (v2). ImGui's globals (GImGui) and heap do not
        // cross the DLL boundary; a plugin that wants to draw ImGui must adopt the
        // host's context + allocators via ImGui::SetCurrentContext / SetAllocatorFunctions
        // in Init. Kept as void* so this ABI header stays imgui-include-free; the
        // plugin reinterpret_casts them to ImGuiContext* / ImGuiMemAllocFunc /
        // ImGuiMemFreeFunc. Null in headless hosts (no ImGuiLayer) -> plugin skips.
        void* imguiContext  = nullptr;   // ImGuiContext*
        void* imguiAlloc    = nullptr;   // ImGuiMemAllocFunc
        void* imguiFree     = nullptr;   // ImGuiMemFreeFunc
        void* imguiUserData = nullptr;
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
        // v2: the host calls DrawUI between ImGuiLayer::BeginFrame and ::Render --
        // the only window where ImGui draw calls are valid. The plugin's Update runs
        // in the sim phase (before BeginFrame), so it is too early for ImGui.
        inline constexpr const char* kDrawUI      = "GamePlugin_DrawUI";
    }

    // Host-side resolved function-pointer table for one loaded plugin image.
    struct PluginVTable
    {
        uint32_t (*ABIVersion)()                         = nullptr;
        bool     (*Init)(EngineContext*)                 = nullptr;
        void     (*Shutdown)()                           = nullptr;
        void     (*FixedUpdate)(double dt)               = nullptr;
        void     (*Update)(double dt, double alpha)      = nullptr;
        // SaveState is void because BinaryWriter is error-latching; callers check writer.HasError()
        // after the call, mirroring how LoadState signals failure via its bool return.
        void     (*SaveState)(Astra::BinaryWriter&)      = nullptr;
        bool     (*LoadState)(Astra::BinaryReader&)      = nullptr;
        // v2: host calls this between ImGuiLayer BeginFrame and Render (the only valid
        // ImGui draw window). Update is sim-phase -- too early. May be null if a plugin
        // does not export it (resolution is lenient); the host null-checks before calling.
        void     (*DrawUI)()                             = nullptr;
    };
}

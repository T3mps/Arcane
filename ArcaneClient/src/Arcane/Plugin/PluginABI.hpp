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
    //     Outliner slice 1 rides the same argument: Identity + Hidden are
    //     two more appended name-keyed component types, and the Not<Hidden>
    //     filter in the plugin-compiled RenderSubmissionSystem is behavioral
    //     (a stale plugin just doesn't honor hiding). Still NO bump.
    // v8 (2026-07-28): the sprite-asset arc re-shaped SpriteRenderer's LAYOUT --
    //     textureId (uint32) + size (vec2) removed, a Guid `sprite` added first --
    //     so tint/sortingLayer/orderInLayer/shape/material all sit at new
    //     offsets, and TextureTable was deleted from SceneResources.hpp. Plugins
    //     compile both headers themselves (components + the header-only
    //     RenderSubmissionSystem), which is the same change class the v6 entry
    //     above (:34-35) bumped for. Unlike v6 the failure is not a vtable slide
    //     but the plugin's own copy of submission reading the component at stale
    //     offsets: tint moved 12 -> 16 and sortingLayer 28 -> 32, so a stale tint
    //     read at 12 comes out of the tail of the new `sprite` Guid plus the
    //     first three tint floats, and a stale sortingLayer read at 28 comes out
    //     of tint.w. It also resolves a TextureTable resource the engine no
    //     longer registers. Corruption rather than a guaranteed AV, and silent
    //     either way; reject the pairing.
    // v9 (2026-07-29): Astra re-vendored to dev f8c9998 -- 40 of 60 shared
    //     headers changed, 8 added, 1 removed (Archetype/ArchetypeGraph.hpp).
    //     Same reasoning as the v5 entry above (:28-31), which bumped for a
    //     previous Astra re-vendor: plugins compile Astra's headers THEMSELVES
    //     (Registry, ComponentRegistry, Archetype storage, EntityTable,
    //     Serialization's Binary{Reader,Writer} -- the very types crossing the
    //     boundary through EngineContext::typeContext and the SaveState/
    //     LoadState entry points), so a plugin built against the old headers
    //     and a host built against the new ones disagree on layout and on
    //     TypeContext identity rules. Concretely observable: the new
    //     TypeContext REFUSES a type whose unqualified name-hash collides with
    //     an already-registered one (it used to alias them silently), handing
    //     back an INVALID ComponentID that then overflows the component bitmap
    //     -- an assert in a checked build, a wrong-component read in a
    //     shipping one. Reject the pairing.
    // v10 (2026-08-09): Astra re-vendored to dev f8a75a9 -- the ComponentModule
    //     program. ReRegisterComponent NO LONGER EXISTS (plugins own their types
    //     via RAII ComponentModule handles, heap-held + reset in Shutdown), and
    //     ComponentRegistry grew owner/shadow/meta-thunk state, so a v9 plugin's
    //     inlined template code manipulates a registry whose layout changed.
    //     Layout mismatch plus a removed API: reject the pairing.
    // v11 (2026-08-14): Batcher2D grew a virtual -- Batch2DDrained Drain(), the
    //     read interface the NRI graph path's Batch2DNode consumes instead of
    //     End(). Plugins compile their own copy of Batcher2D.hpp (through
    //     SceneResources.hpp's RenderContext2D, which the header-only
    //     RenderSubmissionSystem calls) and dispatch through this vtable, so a
    //     module built against a Batcher2D with a different virtual set is the
    //     same failure class as the v6 entry above (:34-35). Drain() is declared
    //     LAST specifically so no EXISTING slot moves -- a v10 module would in
    //     fact still dispatch correctly -- but "correct by luck" is not what the
    //     gate is for: the header a v10 module baked in has no Drain() at all,
    //     so anything that calls it through a plugin-provided Batcher2D
    //     implementation (a test double, a future editor batcher) hits a slot
    //     that module never emitted. Reject the pairing; the failure mode of
    //     NOT rejecting it is silent.
    inline constexpr uint32_t kGamePluginABIVersion = 11;

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

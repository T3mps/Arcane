#pragma once

// GameModule: the SDK's game-module boilerplate, written ONCE (spec docs/specs/
// 2026-09-13-game-module-boilerplate-design.md) -- Unreal's
// IMPLEMENT_PRIMARY_GAME_MODULE shaped for Arcane's plugin ABI. A module is a
// class deriving GameModule plus one line:
//
//   struct Module final : Arcane::GameModule
//   {
//       void OnDrawUI() override { /* HUD */ }
//   };
//   ARCANE_GAME_MODULE(MyGame::Module)
//
// The macro emits the eight exports the host resolves (PluginEntry::k*,
// PluginABI.hpp) and everything a module used to copy: the shared TypeContext
// pin, the Mosaic log-sink + assert-handler installs, the ImGui context/
// allocator adoption, this module's Astra::ComponentModule with the
// ARCANE_COMPONENT drain (GameComponents.hpp), and the registry Save/LoadState
// round-trip for hot reload. Every hook has a default; override what the
// module needs. THE ENGINE OWNS ITS STANDARD SYSTEMS (Runtime::
// InstallEngineSystems: PhysicsSystem -> TransformPropagationSystem in
// fixedUpdate, RenderSubmissionSystem in render) -- a module registers ONLY its
// own systems, in OnInit through RegisterSystem<T>(mask, phase) (ABI 30), and
// places them with Astra::Before<...> / Astra::After<...> against the engine's
// types (Astra keys systems by a hash of the type NAME, so that works across the
// DLL boundary). RegisterSystem declares a FACTORY, not an instance: each of the
// N Runtimes the host attached instantiates the subset its NetMode matches
// (Arcane/Plugin/SystemFactory.hpp), which is what lets one module image serve a
// server world and a client world in one process.
//
// EVERYTHING HERE INSTANTIATES IN THE MODULE IMAGE, on purpose: SetTypeContext,
// the Mosaic installs and RegisterComponents each act on THIS module's own
// per-module state, and the ComponentModule handle owns descriptors that point
// into this image. That is why the bodies are inline in a header rather than
// exported from ArcaneClient.dll.

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ProcessContext.hpp>   // Process()/RegisterSystem reach SystemFactories()
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/GameComponents.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>   // RoleMask / SystemPhase / SystemFactoryEntry
#include <Arcane/Scene/SceneResources.hpp>   // SceneRoot: SceneRootEntity() + the Save/LoadState root id

#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <imgui.h>   // ABI v2: adopt the host's ImGui context/allocators (imported from ArcaneClient.dll)

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>       // std::ignore (RegisterSystem's AddSystem result)
#include <type_traits>
#include <vector>

#if defined(_WIN32)
  #define ARCANE_GAME_MODULE_EXPORT __declspec(dllexport)
#else
  #define ARCANE_GAME_MODULE_EXPORT __attribute__((visibility("default")))
#endif

namespace Arcane
{
    class GameModule
    {
    public:
        virtual ~GameModule() = default;

        // ---- hooks (every one defaulted; override what the module needs) ----

        // After the prologue (TypeContext, Mosaic, ImGui, this module's
        // ComponentModule drained). Register the module's OWN systems here.
        // false aborts the load (the host reports "initial load failed").
        virtual bool OnInit(EngineContext& ctx) { (void)ctx; return true; }
        // Before the ComponentModule handle closes and before the image unmaps;
        // Context()/Registry()/Components() are still valid here.
        virtual void OnShutdown() {}
        virtual void OnFixedUpdate(double dt) { (void)dt; }
        virtual void OnUpdate(double dt, double alpha) { (void)dt; (void)alpha; }
        // Between the host's ImGui BeginFrame and Render. Not called in a headless
        // host (no ImGui context) -- no guard needed in the override.
        virtual void OnDrawUI() {}
        // Module extras, written AFTER the registry blob / read AFTER the registry
        // restore. The registry round-trip itself is the macro's (never overridden).
        virtual void OnSaveState(Astra::BinaryWriter& w) { (void)w; }
        virtual bool OnLoadState(Astra::BinaryReader& r) { (void)r; return true; }

        // ---- what the prologue established (valid from OnInit to OnShutdown) ----

        [[nodiscard]] EngineContext& Context() const noexcept
        {
            ARC_ASSERT(m_ctx != nullptr, "GameModule::Context() outside the OnInit..OnShutdown window");
            return *m_ctx;
        }
        [[nodiscard]] Runtime&         Engine()   const noexcept { return *Context().engine; }
        [[nodiscard]] Astra::Registry& Registry() const noexcept { return Engine().Registry(); }
        // The process object (ABI 30): the shared TypeContext and the system-factory
        // table. Engine() is the PRIMARY world; Process() is what every world shares.
        [[nodiscard]] ProcessContext&  Process()  const noexcept { return *Context().process; }
        // The presentation extension, or NULL on a headless host (ArcaneServer, an
        // embedded server world, a headless test). Always null-check it.
        [[nodiscard]] ClientRuntime*   Client()   const noexcept { return Context().client; }
        [[nodiscard]] Astra::ComponentModule& Components() const noexcept
        {
            ARC_ASSERT(m_components != nullptr, "GameModule::Components() outside the OnInit..OnShutdown window");
            return *m_components;
        }
        // The scene's root entity, read from the registry ON DEMAND -- never
        // cached: Init runs before the host loads the boot scene, and File > Open
        // Scene swaps the whole registry. Invalid when no scene is loaded.
        [[nodiscard]] Astra::Entity SceneRootEntity() const
        {
            const SceneRoot* sr = Registry().GetResource<SceneRoot>();
            return sr ? sr->entity : Astra::Entity::Invalid();
        }

        // Register one of this module's systems ONCE per DLL load (spec s4: "systems
        // stay explicit, their order is a design act" -- an explicit line in OnInit,
        // with an explicit mask; Astra's Before/After traits still place it). Every
        // Runtime whose NetMode matches `mask` instantiates it: the primary right
        // after OnInit, any other attached Runtime at attach, and all of them again
        // after a hot reload. The std::function lives in THIS module and PluginHost
        // clears it before the image unmaps.
        template <class System, class... Args>
        void RegisterSystem(RoleMask mask, SystemPhase phase, Args... args)
        {
            Process().SystemFactories().Add(SystemFactoryEntry{
                std::string(Astra::TypeID<System>::Name()), mask, phase,
                [args...](Astra::SystemScheduler& s) { std::ignore = s.AddSystem<System>(args...); }, nullptr });
        }

        // Bound by ARCANE_GAME_MODULE's Init before OnInit runs. Not for modules.
        void BindForMacro_(EngineContext* ctx, Astra::ComponentModule* components) noexcept
        {
            m_ctx        = ctx;
            m_components = components;
        }

    private:
        EngineContext*          m_ctx        = nullptr;
        Astra::ComponentModule* m_components = nullptr;
    };

    namespace GameModuleDetail
    {
        // The per-module state behind the exports: three RAW pointers, so the
        // object is trivially destructible. The ComponentModule contract's
        // "NEVER a plugin-side static/global object" rule: a static whose
        // destructor does live cleanup (a ComponentModule by value, an optional,
        // a unique_ptr) runs it during FreeLibrary at DLL_PROCESS_DETACH, under
        // the loader lock. Raw pointers have no destructor; a skipped Shutdown
        // degrades to the contract's forget semantics (the handle leaks and the
        // host's UnregisterModuleRange net catches the descriptor half).
        struct State
        {
            EngineContext*          ctx        = nullptr;
            Astra::ComponentModule* components = nullptr;
            GameModule*             instance   = nullptr;

            template <typename Type>
            bool Init(EngineContext* c, const char* name)
            {
                static_assert(std::is_base_of_v<GameModule, Type>,
                              "ARCANE_GAME_MODULE(Type): Type must derive from Arcane::GameModule");

                // 1. The shared reflection context in THIS module, and this
                // module's Mosaic log/assert routing into the engine's.
                Astra::SetTypeContext(c->typeContext);
                Log::InstallMosaicSink();
                Assert::InstallMosaicHandler();
                ctx = c;

                // ABI v2: adopt the host's ImGui context + allocators so OnDrawUI
                // draws into the host's single GImGui. Null in a headless host.
                if (c->imguiContext)
                {
                    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(c->imguiContext));
                    ImGui::SetAllocatorFunctions(
                        reinterpret_cast<ImGuiMemAllocFunc>(c->imguiAlloc),
                        reinterpret_cast<ImGuiMemFreeFunc>(c->imguiFree),
                        c->imguiUserData);
                }

                // 2. This module's own component types: open the handle, drain the
                // ARCANE_COMPONENT registrar into it. Every component added under
                // Source/ (Assets -> Create -> C++ Class, or one ARCANE_COMPONENT
                // line by hand) registers here with no edit to the module.
                components = new Astra::ComponentModule(
                    Astra::ComponentModule::Open(c->engine->Components(), name));
                if (!*components)
                {
                    delete components;   // empty handle: safe to destroy here (still mapped)
                    components = nullptr;
                    ctx        = nullptr;
                    return false;        // SetTypeContext above makes this unreachable; fail loudly if not
                }
                const std::size_t count = Game::RegisterComponents(*components);
                ARC_INFO("{}: registered {} module component type(s)", name, count);

                // 3. The module itself. Systems are the module's to register in
                // OnInit -- the engine's standard ones are already installed.
                instance = new Type();
                instance->BindForMacro_(c, components);
                if (!instance->OnInit(*c))
                {
                    delete instance;   instance   = nullptr;
                    delete components; components = nullptr;
                    ctx = nullptr;
                    return false;
                }
                return true;
            }

            void Shutdown()
            {
                // Instance first (its destructor may still touch its own
                // components), then the handle -- which releases this module's
                // descriptors + meta BEFORE the image unmaps.
                if (instance)
                {
                    instance->OnShutdown();
                    delete instance;
                    instance = nullptr;
                }
                delete components;
                components = nullptr;
                ctx        = nullptr;
            }

            void SaveState(Astra::BinaryWriter& w)
            {
                // Persist the scene-root entity id explicitly -- resources are not
                // part of the registry snapshot, so LoadState must re-set SceneRoot
                // after the restore. Read live: whatever scene is loaded NOW is the
                // one a hot reload has to bring back.
                const Astra::Entity root = instance ? instance->SceneRootEntity() : Astra::Entity::Invalid();
                w(static_cast<uint64_t>(root));

                auto snap = ctx->engine->SnapshotRegistry();
                if (snap.IsErr())
                {
                    // Snapshot failed: write a zero-length blob so LoadState fails
                    // cleanly (RestoreRegistry rejects an empty frame) instead of
                    // masking the loss. No extras follow a failed blob.
                    w(static_cast<uint64_t>(0));
                    return;
                }
                const std::vector<std::byte>& blob = *snap.GetValue();
                w(static_cast<uint64_t>(blob.size()));
                w.WriteBytes(blob.data(), blob.size());

                if (instance)
                    instance->OnSaveState(w);
            }

            bool LoadState(Astra::BinaryReader& r)
            {
                uint64_t rootRaw = 0; r(rootRaw);
                const Astra::Entity savedRoot(static_cast<Astra::Entity::StorageType>(rootRaw));

                uint64_t n = 0; r(n);
                std::vector<std::byte> blob(static_cast<std::size_t>(n));
                r.ReadBytes(blob.data(), static_cast<std::size_t>(n));
                if (r.HasError()) return false;
                if (!ctx->engine->RestoreRegistry(blob)) return false;

                // SceneRoot is a resource; the snapshot does not carry it. A zero id
                // means the reload happened with no scene loaded -- a legitimate
                // state; leave the resource unset rather than publish an invalid root.
                if (savedRoot.IsValid())
                    ctx->engine->Registry().SetResource<SceneRoot>(SceneRoot{savedRoot});

                return instance ? instance->OnLoadState(r) : true;
            }
        };
    }
}

// The one-argument face: the module reports the SDK's own ABI version.
#define ARCANE_GAME_MODULE(Type) ARCANE_GAME_MODULE_ABI(Type, ::Arcane::kGamePluginABIVersion)

// The two-argument form exists for ONE caller: the HotReloadPluginBad test
// build, which must report a version the host's gate refuses. A real module
// never passes anything but the SDK's constant.
#define ARCANE_GAME_MODULE_ABI(Type, Abi)                                                          \
    namespace { ::Arcane::GameModuleDetail::State arcane_game_module_state_; }                     \
    extern "C"                                                                                     \
    {                                                                                              \
        ARCANE_GAME_MODULE_EXPORT uint32_t GamePlugin_ABIVersion()                                 \
        { return static_cast<uint32_t>(Abi); }                                                     \
        ARCANE_GAME_MODULE_EXPORT bool GamePlugin_Init(::Arcane::EngineContext* ctx)               \
        { return arcane_game_module_state_.Init<Type>(ctx, #Type); }                               \
        ARCANE_GAME_MODULE_EXPORT void GamePlugin_Shutdown()                                       \
        { arcane_game_module_state_.Shutdown(); }                                                  \
        ARCANE_GAME_MODULE_EXPORT void GamePlugin_FixedUpdate(double dt)                           \
        { if (auto* m = arcane_game_module_state_.instance) m->OnFixedUpdate(dt); }                \
        ARCANE_GAME_MODULE_EXPORT void GamePlugin_Update(double dt, double alpha)                  \
        { if (auto* m = arcane_game_module_state_.instance) m->OnUpdate(dt, alpha); }              \
        ARCANE_GAME_MODULE_EXPORT void GamePlugin_DrawUI()                                         \
        {                                                                                          \
            auto& s = arcane_game_module_state_;                                                   \
            if (s.instance && s.ctx && s.ctx->imguiContext) s.instance->OnDrawUI();                \
        }                                                                                          \
        ARCANE_GAME_MODULE_EXPORT void GamePlugin_SaveState(::Astra::BinaryWriter& w)              \
        { arcane_game_module_state_.SaveState(w); }                                                \
        ARCANE_GAME_MODULE_EXPORT bool GamePlugin_LoadState(::Astra::BinaryReader& r)              \
        { return arcane_game_module_state_.LoadState(r); }                                         \
    }

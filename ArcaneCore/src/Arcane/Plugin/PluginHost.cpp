#include <Arcane/Plugin/PluginHost.hpp>

#include <Arcane/Plugin/Plugin.hpp>

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/ClientHooks.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>
#include <Arcane/Sim/NetDriver.hpp>

#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <tuple>       // std::ignore (InitPluginsLive's discarded Init result)
#include <utility>
#include <vector>

// Only for the process id that namespaces this host's plugin images (see
// HostProcessTag). Every other platform detail lives behind Module.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Arcane
{
    namespace
    {
        // Copy-and-load stages the plugin into a temp image so the SOURCE dll stays
        // rebuildable while the host has one mapped. That image directory must be
        // per-PROCESS: two hosts on the same project (the editor with the game module
        // loaded, plus a separate-window ArcaneRuntime spawned from its Play button)
        // otherwise both stage "<stem>_1.dll" into one shared directory -- and the
        // second one cannot write it, because Windows locks a mapped DLL against
        // overwrite. That failed the copy, failed Init, and closed the new window a
        // moment after it opened. The generation counter cannot fix this on its own:
        // it restarts at 1 in every host, so the collision is on the FIRST load.
        std::string HostProcessTag()
        {
#ifdef _WIN32
            return std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
#else
            return std::to_string(static_cast<long>(::getpid()));
#endif
        }

        struct PluginImage
        {
            std::optional<Plugin> plugin;
            std::filesystem::path dll;
            std::filesystem::path pdb;
            std::uint32_t gen = 0;

            [[nodiscard]] bool IsLoaded() const noexcept
            {
                return plugin.has_value() && plugin->IsLoaded();
            }
        };

        // Restores whatever ImGui context was current before a call into
        // PluginHost, regardless of what the plugin's entry point does to
        // GImGui while it runs (2026-07-30 review, Fix 3). There is exactly
        // one GImGui in the process -- imgui is exported from ArcaneClient.dll and
        // imported everywhere else -- and a plugin's Init is free to call
        // ImGui::SetCurrentContext(...) to adopt the host's allocator/context
        // for its OWN offscreen "game" ImGui layer (Sandbox.cpp:102 does
        // exactly this) without ever restoring it. PluginHost did not used to
        // bracket that, so the switch leaked to WHATEVER HOST CODE RAN NEXT --
        // which is precisely how the 2026-07-30 boot-ordering incident
        // happened: EditorApp's font/theme/docking-flag/settings-handler boot
        // stages, running after StagePluginLoad, silently configured the
        // GAME context instead of the editor's. That specific symptom is
        // fixed by REORDERING those stages ahead of plugin_load (see
        // ProjectBoot.cpp's EditorStages), but the landmine itself lives
        // here: any FUTURE host code that touches ImGui state after calling
        // into PluginHost inherits this same hazard unless PluginHost itself
        // guarantees it never leaks a context change to its caller. Applied
        // at the PUBLIC API boundary (Load/Unload/Reload/FixedUpdateAll/
        // UpdateAll/DrawUIAll) rather than at every individual vt-> call
        // site: the property this exists for is "PluginHost never leaks a
        // context change to ITS CALLER", which holds regardless of how many
        // Init/Shutdown/FixedUpdate/etc. calls happen internally in between,
        // and DrawUIAll's plugins are still free to leave the game context
        // set FOR THE DURATION of their own draw call -- only the state AFTER
        // this class returns control is what gets restored. Cheap (two
        // pointer reads/writes) and safe even with no ImGui context at all
        // (headless [hotreload] tests): GetCurrentContext()/SetCurrentContext
        // both tolerate null.
        // Same contract as the old ImGuiContextGuard (comment above kept): restore
        // whatever UI context was current before a call into PluginHost. Core knows
        // no ImGui; the ClientRuntime's hooks do. Null hooks = headless = nothing to save.
        struct UiContextGuard
        {
            explicit UiContextGuard(IClientHooks* h) noexcept : hooks(h), saved(h ? h->SaveUiContext() : nullptr) {}
            ~UiContextGuard() noexcept { if (hooks) hooks->RestoreUiContext(saved); }
            UiContextGuard(const UiContextGuard&) = delete;
            UiContextGuard& operator=(const UiContextGuard&) = delete;
            IClientHooks* hooks;
            void*         saved;
        };

        // Publishes the ONE diagnostic row naming WHICH load cause (OS-level load
        // failure, missing export, ABI mismatch, CRT-flavor mismatch) sank a plugin load,
        // keyed "plugin:<name>" so the matching success site's Diagnostics::Clear
        // retracts it once the plugin is fixed. Deliberately NOT called for an
        // Init()-returned-false failure (the module loaded fine; that is a
        // plugin-authored failure, a different cause than this task covers) --
        // callers only reach here when the Plugin itself failed to resolve.
        void PublishPluginLoadFailure(const std::string& name, const std::filesystem::path& dllPath,
                                       const PluginResolveError& resolveError)
        {
            Diagnostic d;
            d.severity = DiagSeverity::Error;
            d.scope    = DiagScope::Plugin;
            switch (resolveError.kind)
            {
                case PluginResolveError::Kind::MissingExport:
                    d.code    = "plugin.export.missing";
                    d.message = "Plugin '" + name + "' is missing the required export '" +
                                resolveError.symbol + "'.";
                    d.detail  = "The DLL loaded, but it does not export the full plugin entry "
                                "point set -- it may not be an Arcane game module.";
                    break;
                case PluginResolveError::Kind::AbiMismatch:
                    d.code    = "plugin.abi.mismatch";
                    d.message = "Plugin '" + name + "' targets engine ABI " +
                                std::to_string(resolveError.pluginAbi) + ", but this engine is ABI " +
                                std::to_string(resolveError.engineAbi) + ".";
                    d.detail  = "Rebuild the game DLL against this engine and update its manifest.";
                    break;
                case PluginResolveError::Kind::CrtFlavorMismatch:
                    d.code    = "plugin.crt.mismatch";
                    d.message = "Plugin '" + name + "' is a " +
                                std::string(resolveError.pluginDebugCrt ? "Debug" : "Release") +
                                "-CRT build (imports " + resolveError.symbol + "), but this host is a " +
                                std::string(resolveError.hostDebugCrt ? "Debug" : "Release") + " build.";
                    d.detail  = "Debug and Release modules cannot be mixed -- the first cross-boundary "
                                "call is undefined behavior, and the DLL was refused before LoadLibrary "
                                "could run its initializers. Rebuild the game module in the host's "
                                "configuration (Binaries/ holds ONE configuration at a time).";
                    break;
                case PluginResolveError::Kind::None:
                    d.code    = "plugin.module.load-failed";
                    d.message = "Plugin '" + name + "' could not be loaded: " + Module::LastLoadError();
                    d.detail  = "The file may be missing, corrupt, or built for a different "
                                "architecture, or one of its own dependencies may be missing.";
                    break;
            }
            d.locator = DiagLocator::File(dllPath.string());
            const std::vector<Diagnostic> diags{std::move(d)};
            Diagnostics::Publish("plugin:" + name, diags);
        }
    }

    struct PluginHost::Impl
    {
        // The process's one ProcessContext (TypeContext + the system-factory table)
        // and the worlds this module serves. runtimes.front() is the PRIMARY: the
        // Runtime the module is handed as EngineContext::engine.
        ProcessContext&        process;
        std::vector<Runtime*>  runtimes;
        std::filesystem::path  source;
        std::filesystem::path  tempDir;
        EngineContext          ctx{};
        std::optional<PluginImage> current;
        std::uint32_t gen = 0;

        std::filesystem::file_time_type lastWrite{};
        bool                            pending = false;
        std::filesystem::file_time_type pendingWrite{};
        std::chrono::steady_clock::time_point pendingSince{};

        // Secondary plugins (project Plugins/): loaded ONCE (no independent hot-reload),
        // sharing the Runtime, torn down alongside the primary, and re-established across a
        // primary hot-reload. pluginSources is the AddPlugin() order; `plugins` holds the
        // loaded+Init'd images, kept MAPPED (so their component descriptors stay valid
        // while the registry is reset during a primary reload/teardown).
        std::vector<std::filesystem::path> pluginSources;
        std::vector<PluginImage>           plugins;

        Impl(ProcessContext& proc, std::filesystem::path src)
            : process(proc), source(std::move(src))
        {
            tempDir = std::filesystem::temp_directory_path() / "arcane_plugins" / HostProcessTag();
            RefreshContext();
        }

        // The primary world -- the one the module itself lives in. Asserted non-empty
        // at Load(); every internal caller runs after that gate or guards itself.
        [[nodiscard]] Runtime& Primary() const noexcept { return *runtimes.front(); }
        // The primary's client hooks, or null with no world attached yet / a headless
        // host. Every UiContextGuard site goes through here.
        [[nodiscard]] IClientHooks* PrimaryHooks() const noexcept
        {
            return runtimes.empty() ? nullptr : runtimes.front()->ClientHooks();
        }

        void RefreshContext()
        {
            ctx.abiVersion    = kGamePluginABIVersion;
            // ABI 30: the process object, the primary's presentation extension (null on
            // a headless host) and the primary's net mode.
            ctx.process       = &process;
            ctx.typeContext   = &process.TypeContext();
            // The four ImGui void*s are presentation: null unless a client is
            // attached AND the host installed its context (an ImGui-less host --
            // ArcaneServer, a headless test -- hands the module null, as before).
            ctx.imguiContext  = nullptr;
            ctx.imguiAlloc    = nullptr;
            ctx.imguiFree     = nullptr;
            ctx.imguiUserData = nullptr;
            if (runtimes.empty())
            {
                // No world attached yet (a host that constructed the PluginHost before
                // AttachRuntime). Nothing to describe; Load() refuses this state.
                ctx.workScheduler = nullptr;
                ctx.taskExecutor  = nullptr;
                ctx.engine        = nullptr;
                ctx.client        = nullptr;
                ctx.netMode       = NetMode::Standalone;
                return;
            }
            Runtime& primary  = Primary();
            ctx.workScheduler = primary.WorkScheduler();
            ctx.taskExecutor  = primary.TaskExecutor();
            ctx.engine        = &primary;
            ctx.client        = primary.Client();
            ctx.netMode       = primary.Mode();
            if (IClientHooks* h = primary.ClientHooks())
                h->FillEngineContext(ctx);
        }

        // Run one image's Init with the factory table's owner bracket open on that
        // image (spec s4): GameModule::RegisterSystem passes a null owner and the
        // table stamps the open one, so a module never names its own image base --
        // and TeardownImage can drop exactly its entries before the unmap.
        //
        // NO IMAGE RANGE, NO REGISTRATION. Module::Image() reports {} where the
        // platform has no implementation (Module.cpp: non-Windows, "no host ships
        // here yet") and when the PE header read fails. `base` is the whole owner
        // key, so in that state nothing could ever clear a factory before the image
        // unmaps -- and a second such module would collide on the same null key.
        // Init still runs (a module that registers no systems is unaffected);
        // SystemFactoryTable::Add then asserts and drops anything it does try to
        // register, with the cause already named here.
        bool InitImage(Plugin& p)
        {
            const Module::ImageSpan image = p.LoadedModule().Image();
            if (!image.base)
            {
                ARC_ERROR("plugin: '{}' reports no image range -- system-factory registration is "
                          "refused for it (the owner key is the image base, Arcane/Plugin/"
                          "SystemFactory.hpp); its components are still registered normally",
                          p.LoadedModule().Path().generic_string());
                return p.VTable().Init(&ctx);
            }
            process.SystemFactories().BeginOwner(image.base);
            const bool ok = p.VTable().Init(&ctx);
            process.SystemFactories().EndOwner();
            return ok;
        }

        // Give every attached world the module's matching systems. Idempotent (Astra
        // answers AlreadyRegistered, which InstantiateInto ignores), so the load,
        // attach and reload paths can all call it without coordinating.
        void InstantiateAll()
        {
            for (Runtime* rt : runtimes)
                rt->InstantiateModuleSystems();
        }

        bool CopyVersioned(std::uint32_t g, PluginImage& out)
        {
            std::error_code ec;
            std::filesystem::create_directories(tempDir, ec);
            const std::string stem = source.stem().string();
            out.dll = tempDir / (stem + "_" + std::to_string(g) + ".dll");
            if (!std::filesystem::copy_file(source, out.dll,
                    std::filesystem::copy_options::overwrite_existing, ec) || ec)
            {
                return false;
            }

            std::filesystem::path srcPdb = source;
            srcPdb.replace_extension(".pdb");
            if (std::filesystem::exists(srcPdb))
            {
                out.pdb = tempDir / (stem + "_" + std::to_string(g) + ".pdb");
                std::filesystem::copy_file(srcPdb, out.pdb,
                    std::filesystem::copy_options::overwrite_existing, ec);
            }
            out.gen = g;
            lastWrite = std::filesystem::last_write_time(source, ec);
            return true;
        }

        void DeleteFiles(const PluginImage& img)
        {
            std::error_code ec;
            if (!img.dll.empty()) std::filesystem::remove(img.dll, ec);
            if (!img.pdb.empty()) std::filesystem::remove(img.pdb, ec);
        }

        void TeardownImage(PluginImage& img, bool callShutdown)
        {
            if (!img.plugin)
                return;

            const PluginVTable& vt = img.plugin->VTable();
            if (callShutdown && vt.Shutdown)
                vt.Shutdown();
            // Drop plugin-created audio handles before clearing systems/registry, the
            // same window in which we tear down plugin-owned ECS state. The 2026-06-26
            // host refactor unified all teardown paths (unload, init-failure, reload-of-
            // previous, reload-failure) through TeardownImage, so this single call
            // covers what the audio PR originally hooked at three separate sites.
            // Every ATTACHED world, not just the primary: each has its own schedulers
            // holding this module's system objects and its own registry holding this
            // module's component instances.
            for (Runtime* rt : runtimes)
            {
                if (IClientHooks* h = rt->ClientHooks())
                    h->OnModuleTeardown();
                rt->ClearSystems();
                // Reset while the module is still loaded: registered component destructors
                // may point into plugin code.
                rt->ResetRegistry();
            }

            // ...and the DESCRIPTORS themselves die with the image too, which the
            // line above does NOT cover. The hot-reload contract is now HANDLE-
            // BASED: a well-behaved plugin owns only the component types it
            // implements via its own Astra::ComponentModule, and that handle's
            // Shutdown-time Reset() already cleaned them up (restoring whatever
            // it shadowed) before we get here. The engine's own roster is
            // module-owned too (Runtime's ctor holds an Astra::ComponentModule
            // "Arcane" for it, under a Resident declaration -- Runtime.cpp), so
            // a plugin overriding one of those types shadows the engine's entry
            // and its own unload restores it via the same owner-stack
            // mechanism -- so there is nothing to rebind on this path anymore.
            //
            // The purge below is the FALLBACK NET for a plugin that forgot (or
            // never adopted) that cleanup: it catches any descriptor still
            // pointing into this image while the image is STILL MAPPED, turning
            // what used to be a dangling call into freed code (Diagnosed
            // 2026-07-31 from a hang/crash on project switch: Aphelyon ->
            // ReferenceProject -> Aphelyon, faulting in AddComponentByTypeName ->
            // ComponentDescriptor::DefaultConstruct) into a clean
            // SkippedUnregistered miss in SceneSerializer instead.
            const Module::ImageSpan image = img.plugin->LoadedModule().Image();
            // The SAME window, and the same reason, as the descriptor purge: this
            // module's system-factory std::functions are compiled INTO the image, so
            // they must die before it unmaps (spec s4 / plan 1 P8). OUTSIDE the
            // `size` guard below on purpose: `base` ALONE is the owner key, and it is
            // exactly the key InitImage opened the bracket with -- hiding the clear
            // behind a range the purge needs would leave a dangling callable whenever
            // Image() reports a base but no usable size. (A null base opened no
            // bracket, so this is then a no-op on an empty owner.)
            process.SystemFactories().ClearOwner(image.base);
            // Component descriptors need the RANGE, not just the base -- hence the
            // separate guard. Every attached world's registry is swept: they SHARE the
            // primary's ComponentRegistry (Runtime's three-argument ctor), so this is
            // one purge repeated harmlessly rather than N distinct ones, and it stays
            // a loop so a future non-sharing arrangement is still covered.
            if (image.size != 0)
            {
                for (Runtime* rt : runtimes)
                {
                    if (const auto& creg = rt->Components())
                    {
                        const std::size_t dropped = creg->UnregisterModuleRange(image.base, image.size);
                        if (dropped != 0)
                            ARC_TRACE("PluginHost: disowned {} component descriptor(s) owned by the unloading module", dropped);
                    }
                }
            }

            img.plugin.reset();   // FreeLibrary / dlclose
        }

        // Fallback net for secondaries, mirroring TeardownImage's purge for the
        // primary: a well-behaved plugin's ComponentModule already cleaned up in
        // its Shutdown; this catches one that forgot, BEFORE FreeLibrary.
        void DisownPluginImages()
        {
            for (auto& img : plugins)
            {
                if (!img.plugin)
                    continue;
                const Module::ImageSpan image = img.plugin->LoadedModule().Image();
                // Secondaries register system factories through the same
                // GameModule::RegisterSystem path, so they get the same
                // before-the-unmap clear the primary does in TeardownImage -- and,
                // for the same reason, keyed on `base` alone, outside the range guard.
                process.SystemFactories().ClearOwner(image.base);
                if (image.size != 0)
                {
                    for (Runtime* rt : runtimes)
                    {
                        const auto& creg = rt->Components();
                        if (!creg)
                            continue;
                        const std::size_t dropped = creg->UnregisterModuleRange(image.base, image.size);
                        if (dropped != 0)
                            ARC_TRACE("PluginHost: disowned {} descriptor(s) from a secondary that skipped handle cleanup", dropped);
                    }
                }
            }
        }

        void TeardownLive()
        {
            if (current)
                TeardownImage(*current, true);
        }

        // --- secondary plugins ---------------------------------------------------------

        // Load + Init each pending plugin (direct load, no versioned copy -- secondaries
        // do not hot-reload). Init in AddPlugin() order. On any failure, quiesce + drop the
        // ones already brought up and return false (the caller unwinds the whole set).
        bool LoadInitPlugins()
        {
            for (const auto& src : pluginSources)
            {
                const std::string name = src.stem().string();
                PluginResolveError resolveError;
                std::optional<Plugin> p = Plugin::Load(src, &resolveError);
                RefreshContext();
                if (!p || !InitImage(*p))
                {
                    if (p)
                    {
                        // THE IMAGE IS MAPPED AND ITS FACTORY BRACKET ALREADY RAN
                        // (final-review fix wave, C1). `p` is a local optional: letting
                        // it die at the `return false` below FreeLibrary's the image
                        // while everything this module's OnInit registered before
                        // failing is still in the PROCESS-LIFETIME SystemFactoryTable
                        // -- it was never pushed into `plugins`, so DisownPluginImages
                        // never sees it and nothing ever calls ClearOwner(image.base).
                        // The next Runtime construction would then instantiate a
                        // std::function compiled into freed code. Adopt the image into
                        // a PluginImage and run the SAME full teardown the primary's
                        // own init-failure path runs (Load() above, ReloadPrimary's
                        // rollback branch): ClearOwner, the component-descriptor purge
                        // and the registry reset, all while the image is still mapped.
                        // callShutdown stays TRUE -- the Shutdown this replaces was
                        // unconditional, and the macro's Shutdown() after a failed
                        // OnInit is a no-op (GameModule.hpp nulls instance/components
                        // on that path).
                        PluginImage failed;
                        failed.plugin = std::move(*p);
                        TeardownImage(failed, /*callShutdown*/ true);
                    }
                    else   // a real load failure, not Init()-returned-false -- name the cause
                    {
                        PublishPluginLoadFailure(name, src, resolveError);
                    }
                    ShutdownPluginsLive();
                    DisownPluginImages();
                    plugins.clear();
                    ARC_ERROR("plugin: failed to load secondary '{}'", src.generic_string());
                    return false;
                }
                PluginImage img;
                img.plugin = std::move(*p);
                plugins.push_back(std::move(img));
                Diagnostics::Clear("plugin:" + name);
                ARC_INFO("plugin: secondary '{}' loaded", src.generic_string());
            }
            return true;
        }

        // Call each loaded plugin's Shutdown (reverse of load order) but KEEP the modules
        // mapped -- so a following registry reset can safely destroy plugin-typed component
        // instances (their descriptors still point into loaded code).
        void ShutdownPluginsLive()
        {
            for (auto it = plugins.rbegin(); it != plugins.rend(); ++it)
                if (it->plugin && it->plugin->VTable().Shutdown)
                    it->plugin->VTable().Shutdown();
        }

        // Re-Init each loaded plugin (load order) on the SAME mapped modules -- used to
        // re-establish secondaries after a primary hot-reload reset the shared registry.
        void InitPluginsLive()
        {
            for (auto& img : plugins)
            {
                RefreshContext();
                // BeginOwner clears this image's previous entries first, so a
                // re-established secondary re-registers rather than double-registers.
                if (img.plugin) std::ignore = InitImage(*img.plugin);
            }
        }

        // The hot-reload refusal (spec s5): swapping the module under a live net
        // driver would rebuild the world both ends of a connection agreed on. Names
        // WHICH attached world is holding it open -- with N worlds, "a driver is
        // active" is not actionable on its own.
        bool RefuseReloadForActiveNetDriver()
        {
            for (std::size_t i = 0; i < runtimes.size(); ++i)
            {
                const INetDriver* d = runtimes[i]->NetDriver();
                if (!d || !d->IsActive())
                    continue;
                const std::string name = source.stem().string();
                ARC_ERROR("plugin: reload refused -- Runtime #{} ({}) has an active net driver "
                          "(spec 2026-09-15 s5); stop it first", i + 1, ToString(runtimes[i]->Mode()));
                Diagnostic diag;
                diag.severity = DiagSeverity::Warning;
                diag.scope    = DiagScope::Project;
                diag.code     = "plugin.reload.refused-net-active";
                diag.message  = "Hot reload of '" + name + "' was refused: Runtime #" +
                                std::to_string(i + 1) + " (" + ToString(runtimes[i]->Mode()) +
                                ") has an active net driver.";
                diag.detail   = "Reloading the game module rebuilds the world; stop the "
                                "networked session first, then rebuild.";
                const std::vector<Diagnostic> diags{std::move(diag)};
                Diagnostics::Publish("plugin:" + name, diags);
                return true;
            }
            return false;
        }

        // The primary module's full reload sequence (copy + ABI + SaveState/LoadState +
        // last-good rollback). Delicate + heavily tested; kept byte-for-byte as the
        // single-module host -- PluginHost::Reload only wraps plugin re-establishment
        // around it, and with no plugins the wrapper is a no-op.
        bool ReloadPrimary(bool restoreState);
    };

    bool PluginHost::Impl::ReloadPrimary(bool restoreState)
    {
        const std::string name = source.stem().string();

        // The refusal comes FIRST, before anything is copied, torn down or counted:
        // a refused reload must leave the live module, the generation and every
        // attached registry exactly as they were (spec s5).
        if (RefuseReloadForActiveNetDriver())
            return false;

        const std::uint32_t nextGen = gen + 1;
        PluginImage next;
        if (!CopyVersioned(nextGen, next))
            return false;

        std::vector<std::byte> snapshot;
        if (restoreState && current && current->plugin)
        {
            const PluginVTable& vt = current->plugin->VTable();
            if (vt.SaveState)
            {
                Astra::BinaryWriter w(snapshot);
                vt.SaveState(w);
                if (w.HasError())
                {
                    ARC_ERROR("plugin: SaveState failed; aborting reload, keeping live plugin");
                    DeleteFiles(next);
                    return false;
                }
            }
        }

        // SNAPSHOT ALL (spec s5). The PRIMARY's state travels through the module's own
        // SaveState/LoadState above -- the module decides what its world means. Every
        // OTHER attached world has no module code of its own, so its registry IS its
        // state: snapshot it here and restore it after the new image's Init. A failed
        // snapshot aborts the reload with the live module untouched, exactly as a
        // failed SaveState does.
        std::vector<std::pair<Runtime*, std::vector<std::byte>>> secondaryWorlds;
        if (restoreState)
        {
            for (std::size_t i = 1; i < runtimes.size(); ++i)
            {
                auto snap = runtimes[i]->SnapshotRegistry();
                if (snap.IsErr())
                {
                    ARC_ERROR("plugin: snapshot of Runtime #{} ({}) failed; aborting reload, "
                              "keeping live plugin", i + 1, ToString(runtimes[i]->Mode()));
                    DeleteFiles(next);
                    return false;
                }
                secondaryWorlds.emplace_back(runtimes[i], std::move(*snap.GetValue()));
            }
        }

        std::optional<PluginImage> previous = std::move(current);
        current.reset();
        if (previous)
            TeardownImage(*previous, true);

        if (!restoreState)
            for (Runtime* rt : runtimes)
                rt->ResetRegistry();

        PluginResolveError resolveError;
        std::optional<Plugin> loadedNext = Plugin::Load(next.dll, &resolveError);
        RefreshContext();
        const bool initRan = loadedNext && InitImage(*loadedNext);
        bool ok = initRan;
        if (ok && restoreState)
        {
            Astra::BinaryReader r(snapshot);
            ok = loadedNext->VTable().LoadState(r);
            // RESTORE ALL: the other worlds come back from their own registry bytes.
            for (auto& [rt, bytes] : secondaryWorlds)
            {
                if (!ok)
                    break;
                if (!rt->RestoreRegistry(bytes))
                {
                    ARC_ERROR("plugin: restore of the {} world failed after reload", ToString(rt->Mode()));
                    ok = false;
                }
            }
        }

        if (ok)
        {
            next.plugin = std::move(*loadedNext);
            current = std::move(next);
            gen = nextGen;
            if (previous)
                DeleteFiles(*previous);
            // The new image re-registered its factories in Init; nothing has
            // instantiated them yet -- for the primary either, since TeardownImage
            // cleared its systems. Every attached world, one call.
            InstantiateAll();
            Diagnostics::Clear("plugin:" + name);
            ARC_INFO("plugin reloaded (gen {}, snapshot {} bytes)", nextGen, snapshot.size());
            return true;
        }

        if (loadedNext)
        {
            next.plugin = std::move(*loadedNext);
            TeardownImage(next, initRan);
        }
        else   // a real load failure, not Init()/SaveState/LoadState -- name the cause
        {
            PublishPluginLoadFailure(name, source, resolveError);
        }
        DeleteFiles(next);
        for (Runtime* rt : runtimes)
            rt->ClearSystems();

        bool rolledBack = false;
        if (previous && !previous->dll.empty())
        {
            std::optional<Plugin> rollback = Plugin::Load(previous->dll);
            RefreshContext();
            if (rollback)
            {
                // Adopt the image into `previous` BEFORE judging Init: whichever way
                // Init goes, this image is now mapped and InitImage has already opened
                // (and closed) its factory bracket, so the failure path must run the
                // full TeardownImage rather than let the optional quietly FreeLibrary.
                // Otherwise anything the module registered before failing -- factories
                // and component descriptors alike -- outlives its own code, and the
                // factory entries live in a PROCESS-LIFETIME table.
                const bool rollbackInit = InitImage(*rollback);
                previous->plugin = std::move(*rollback);
                if (rollbackInit)
                {
                    if (restoreState && !snapshot.empty())
                    {
                        Astra::BinaryReader r(snapshot);
                        if (!previous->plugin->VTable().LoadState(r))
                            ARC_ERROR("plugin: rollback LoadState failed; last-good running but state may be lost");
                    }
                    // Same restore-all the success path performs: the other worlds are
                    // rolled back to the bytes taken before the failed swap.
                    for (auto& [rt, bytes] : secondaryWorlds)
                        if (!rt->RestoreRegistry(bytes))
                            ARC_ERROR("plugin: rollback restore of the {} world failed; last-good "
                                      "running but that world's state may be lost", ToString(rt->Mode()));
                    rolledBack = true;
                }
                else
                {
                    // Init failed, so no Shutdown is owed (the Load() init-failure path
                    // makes the same call with the same argument).
                    TeardownImage(*previous, /*callShutdown*/ false);
                }
            }
        }

        if (rolledBack)
        {
            current = std::move(previous);
            InstantiateAll();
            ARC_ERROR("plugin reload failed (gen {}); rolled back to last-good", nextGen);
            return false;
        }

        // Double failure: the new image failed to load AND the last-good rollback
        // also failed (or there was no last-good to roll back to). Do NOT install a
        // half-assigned 'current' whose plugin optional is empty -- that reads as
        // IsLoaded()==false / Vtable()==null while the host still believes a plugin
        // is present, so the main loop's `if (vt)` guards silently freeze the sim
        // (no FixedUpdate/Update/DrawUI) while still rendering, with only the generic
        // reload-failed line logged. Surface an honest dead state instead: leave
        // 'current' empty so IsLoaded()==false truthfully means "no plugin", clean up
        // the abandoned last-good copies, and log the double failure explicitly.
        if (previous)
            DeleteFiles(*previous);
        current.reset();
        ARC_ERROR("plugin reload failed (gen {}); rollback to last-good ALSO failed -- no plugin loaded", nextGen);
        return false;
    }

    PluginHost::PluginHost(ProcessContext& process, std::filesystem::path src)
        : m_impl(std::make_unique<Impl>(process, std::move(src))) {}

    PluginHost::~PluginHost()
    {
        Unload();
        // Unload's DeleteFiles removed the images; take the now-empty per-process
        // directory with them so TEMP does not collect one dead folder per run.
        // Best effort by design: remove() on a non-empty directory just reports an
        // error into `ec`, which is the right outcome if something is still mapped.
        std::error_code ec;
        std::filesystem::remove(m_impl->tempDir, ec);
    }

    void PluginHost::AddPlugin(std::filesystem::path dll)
    {
        m_impl->pluginSources.push_back(std::move(dll));
    }

    bool PluginHost::AttachRuntime(Runtime& rt)
    {
        auto& rts = m_impl->runtimes;
        if (std::find(rts.begin(), rts.end(), &rt) != rts.end())
            return true;   // idempotent: attaching the same world twice is a no-op, not two worlds
        // THE SHARED-COMPONENTREGISTRY INVARIANT (spec s4). A module opens its
        // Astra::ComponentModule on the PRIMARY's registry and nowhere else
        // (GameModule.hpp), so a secondary with a registry of its own would resolve
        // none of the module's component types -- and a registry snapshot moved
        // between the two worlds would fail with UnknownComponent. Refuse here,
        // where the mistake is still cheap and nameable, rather than letting it
        // surface as an empty view or a failed scene load much later.
        if (!rts.empty() && rt.Components() != rts.front()->Components())
        {
            ARC_ERROR("PluginHost: refusing to attach a Runtime with its own ComponentRegistry -- "
                      "every world one module serves must share the PRIMARY's (spec 2026-09-15 s4); "
                      "build it as Runtime(process, mode, primary.Components())");
            return false;
        }
        rts.push_back(&rt);
        // A world joining an ALREADY-LOADED host gets the module's matching systems
        // now; the EngineContext's primary-derived fields do not change (the primary
        // cannot change while loaded -- DetachRuntime refuses dropping it). Before
        // Load() there is nothing to instantiate: Load()'s own InstantiateAll covers
        // every world attached by then.
        if (m_impl->current || !m_impl->plugins.empty())
            rt.InstantiateModuleSystems();
        return true;
    }

    void PluginHost::DetachRuntime(Runtime& rt) noexcept
    {
        auto& rts = m_impl->runtimes;
        const auto it = std::find(rts.begin(), rts.end(), &rt);
        if (it == rts.end())
            return;
        if (it == rts.begin() && (m_impl->current || !m_impl->plugins.empty()))
        {
            ARC_ERROR("PluginHost: refusing to detach the PRIMARY Runtime of a loaded host -- "
                      "it is the module's own world; Unload() first");
            return;
        }
        // The world that is leaving still holds this module's SYSTEMS and, in its
        // registry, ENTITIES whose component descriptors point into the module image.
        // The moment it is erased from `runtimes` it drops out of TeardownImage's
        // ClearSystems/ResetRegistry loop, so both have to happen HERE -- otherwise
        // those descriptors dangle as soon as the module unmaps: exactly the
        // permanent-dangling-descriptor class diagnosed 2026-07-31 (TeardownImage's
        // purge comment tells that story).
        //
        // NO UnregisterModuleRange here, deliberately: every attached world SHARES the
        // primary's ComponentRegistry, so purging the module's descriptors would strip
        // them from the worlds that are STAYING. Emptying THIS world's registry is the
        // whole job; the descriptors themselves die with the image, for all worlds at
        // once, in TeardownImage.
        rt.ClearSystems();
        rt.ResetRegistry();
        rts.erase(it);
    }

    void PluginHost::RefreshEngineContext()
    {
        m_impl->RefreshContext();
    }

    const EngineContext* PluginHost::Context() const noexcept
    {
        return &m_impl->ctx;
    }

    std::span<Runtime* const> PluginHost::Runtimes() const noexcept
    {
        return std::span<Runtime* const>(m_impl->runtimes.data(), m_impl->runtimes.size());
    }

    bool PluginHost::Load()
    {
        // A host with no world attached has nothing to hand the module: EngineContext
        // ::engine would be null and the module's Init would fault on its first
        // Registry() call. A programmer error at the call site, not a runtime state --
        // hence the assert. But the assert COMPILES OUT in Release (final-review fix
        // wave, minor 3), and "Init faults on a null engine" is not a Release failure
        // mode worth keeping: refuse the load for real, in every configuration, so the
        // header's "Load() refuses" is true as written.
        ARC_ASSERT(!m_impl->runtimes.empty(),
                   "PluginHost::Load: attach at least one Runtime first (AttachRuntime)");
        if (m_impl->runtimes.empty())
        {
            ARC_ERROR("PluginHost::Load: refused -- no Runtime is attached "
                      "(AttachRuntime first; the module's EngineContext::engine would be null)");
            return false;
        }

        // See UiContextGuard's comment: a plugin's Init may switch GImGui
        // and never switch it back (Sandbox.cpp:102), so every public entry
        // point restores whatever was current on entry before returning.
        const UiContextGuard uiGuard(m_impl->PrimaryHooks());

        // Plugins-only host (no primary game module) -- the editor opening a project that has
        // plugin modules but no gameModule. Skip the primary copy/load/ABI/rollback path and
        // bring up just the secondaries (LoadInitPlugins unwinds itself on failure). With a
        // primary present this branch is never taken, so the delicate path below and every
        // [hotreload] test stay byte-identical.
        if (m_impl->source.empty())
        {
            if (!m_impl->LoadInitPlugins())
                return false;
            m_impl->InstantiateAll();
            return true;
        }

        const std::string name = m_impl->source.stem().string();

        const std::uint32_t g = m_impl->gen + 1;
        PluginImage img;
        bool copied = false;
        for (int attempt = 0; attempt < 5 && !copied; ++attempt)
        {
            copied = m_impl->CopyVersioned(g, img);
            if (!copied)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!copied)
        {
            ARC_ERROR("plugin: cannot copy source DLL");
            return false;
        }

        PluginResolveError resolveError;
        std::optional<Plugin> plugin = Plugin::Load(img.dll, &resolveError);
        m_impl->RefreshContext();
        const bool initRan = plugin && m_impl->InitImage(*plugin);
        if (!initRan)
        {
            if (plugin)
            {
                img.plugin = std::move(*plugin);
                m_impl->TeardownImage(img, false);
            }
            else   // a real load failure, not Init()-returned-false -- name the cause
            {
                PublishPluginLoadFailure(name, m_impl->source, resolveError);
            }
            m_impl->DeleteFiles(img);
            ARC_ERROR("plugin: initial load failed");
            return false;
        }

        img.plugin = std::move(*plugin);
        m_impl->current = std::move(img);
        m_impl->gen = g;
        Diagnostics::Clear("plugin:" + name);
        ARC_INFO("plugin loaded (gen {})", g);

        // Bring up the secondary plugins after the primary (they share the Runtime). A
        // plugin failure unwinds the whole session -- no half-loaded host.
        if (!m_impl->LoadInitPlugins())
        {
            Unload();
            return false;
        }
        // Every module that is going to register factories this session has now run
        // its Init: give each attached world the systems its NetMode matches (spec s4).
        m_impl->InstantiateAll();
        return true;
    }

    void PluginHost::Unload()
    {
        if (!m_impl->current && m_impl->plugins.empty())
            return;

        // See UiContextGuard's comment (Load() above).
        const UiContextGuard uiGuard(m_impl->PrimaryHooks());

        // Quiesce plugins (reverse order) while everything is still mapped; the primary's
        // TeardownImage performs the SINGLE shared-state reset (audio/systems/registry)
        // with all module DLLs still loaded (component descriptors may point into any).
        m_impl->ShutdownPluginsLive();
        if (m_impl->current)
        {
            m_impl->TeardownImage(*m_impl->current, true);
            m_impl->DeleteFiles(*m_impl->current);
            m_impl->current.reset();
        }
        else
        {
            // Plugins-only (no primary loaded): do the shared reset here, once -- for
            // every attached world, the same set TeardownImage covers.
            for (Runtime* rt : m_impl->runtimes)
            {
                if (IClientHooks* h = rt->ClientHooks())
                    h->OnModuleTeardown();
                rt->ClearSystems();
                rt->ResetRegistry();
            }
        }
        m_impl->DisownPluginImages();
        m_impl->plugins.clear();   // unload plugin DLLs AFTER the reset
    }

    bool PluginHost::Reload(bool restoreState)
    {
        // See UiContextGuard's comment (Load() above).
        const UiContextGuard uiGuard(m_impl->PrimaryHooks());

        // Plugins-only host: no primary to reload, and secondaries load once and never
        // hot-reload -- so a reload request is a no-op success (nothing to rebuild).
        if (m_impl->source.empty())
            return true;

        // Multi-module: quiesce plugins (kept mapped) so the primary's reset doesn't touch
        // a running secondary, run the UNCHANGED primary reload, then re-establish the
        // plugins on the post-reload registry. With no plugins both calls are no-ops, so
        // Reload == ReloadPrimary exactly (the single-module hot-reload contract). Note:
        // secondaries re-Init AFTER the primary here (a documented ordering nuance vs boot)
        // and rebuild their own state rather than snapshotting it.
        // The net-driver refusal is ReloadPrimary's own first act (spec s5); ask it
        // here too, BEFORE the secondaries are quiesced, so a refused reload bounces
        // nothing at all -- not the primary, not a secondary, not a generation.
        if (m_impl->RefuseReloadForActiveNetDriver())
            return false;
        m_impl->ShutdownPluginsLive();
        const bool ok = m_impl->ReloadPrimary(restoreState);
        m_impl->InitPluginsLive();
        // The re-established secondaries re-registered their factories in InitPluginsLive
        // (after ReloadPrimary's own InstantiateAll); idempotent, so this only adds what
        // they brought back.
        m_impl->InstantiateAll();
        return ok;
    }

    void PluginHost::Poll()
    {
        if (m_impl->source.empty())
            return;   // no primary to watch (plugins-only host); secondaries don't hot-reload

        std::error_code ec;
        const auto wt = std::filesystem::last_write_time(m_impl->source, ec);
        if (ec) return;
        if (wt == m_impl->lastWrite)
        {
            m_impl->pending = false;
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!m_impl->pending || wt != m_impl->pendingWrite)
        {
            m_impl->pending = true;
            m_impl->pendingWrite = wt;
            m_impl->pendingSince = now;
            return;
        }
        if (now - m_impl->pendingSince >= std::chrono::milliseconds(250))
        {
            m_impl->pending = false;
            Reload(true);
        }
    }

    void PluginHost::FixedUpdateAll(double dt)
    {
        // See UiContextGuard's comment (Load() above). FixedUpdate is not
        // expected to touch ImGui, but the guard is cheap (two pointer ops)
        // and this is a hot per-frame call, so it costs nothing to hold the
        // same "never leaks a context change" guarantee every entry point
        // here makes, rather than special-casing "the ones we know misbehave
        // today."
        const UiContextGuard uiGuard(m_impl->PrimaryHooks());
        if (const PluginVTable* vt = Vtable(); vt && vt->FixedUpdate) vt->FixedUpdate(dt);
        for (auto& img : m_impl->plugins)
            if (img.plugin && img.plugin->VTable().FixedUpdate) img.plugin->VTable().FixedUpdate(dt);
    }

    void PluginHost::UpdateAll(double dt, double alpha)
    {
        // See UiContextGuard's comment (Load() above).
        const UiContextGuard uiGuard(m_impl->PrimaryHooks());
        if (const PluginVTable* vt = Vtable(); vt && vt->Update) vt->Update(dt, alpha);
        for (auto& img : m_impl->plugins)
            if (img.plugin && img.plugin->VTable().Update) img.plugin->VTable().Update(dt, alpha);
    }

    void PluginHost::DrawUIAll()
    {
        // See UiContextGuard's comment (Load() above). DrawUI is the ONE
        // entry point that is SUPPOSED to touch ImGui (the plugin draws its
        // own HUD into its own offscreen "game" context, composited into the
        // viewport texture by the caller) -- the guard does not interfere
        // with that: it only restores whatever context was current on ENTRY
        // once THIS FUNCTION returns, so every plugin's DrawUI is still free
        // to leave the game context set for as long as it's running.
        const UiContextGuard uiGuard(m_impl->PrimaryHooks());
        if (const PluginVTable* vt = Vtable(); vt && vt->DrawUI) vt->DrawUI();
        for (auto& img : m_impl->plugins)
            if (img.plugin && img.plugin->VTable().DrawUI) img.plugin->VTable().DrawUI();
    }

    bool PluginHost::IsLoaded() const noexcept
    {
        return m_impl->current.has_value() && m_impl->current->IsLoaded();
    }

    const PluginVTable* PluginHost::Vtable() const noexcept
    {
        if (!m_impl->current || !m_impl->current->plugin)
            return nullptr;
        return &m_impl->current->plugin->VTable();
    }

    std::uint32_t PluginHost::Generation() const noexcept
    {
        return m_impl->gen;
    }
}

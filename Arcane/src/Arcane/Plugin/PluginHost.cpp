#include <Arcane/Plugin/PluginHost.hpp>

#include <Arcane/Plugin/Plugin.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>

#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <imgui.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
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
        // one GImGui in the process -- imgui is exported from Arcane.dll and
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
        struct ImGuiContextGuard
        {
            ImGuiContextGuard() noexcept : saved(ImGui::GetCurrentContext()) {}
            ~ImGuiContextGuard() noexcept { ImGui::SetCurrentContext(saved); }
            ImGuiContextGuard(const ImGuiContextGuard&) = delete;
            ImGuiContextGuard& operator=(const ImGuiContextGuard&) = delete;
            ImGuiContext* saved;
        };
    }

    struct PluginHost::Impl
    {
        Runtime&               runtime;
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

        Impl(Runtime& rt, std::filesystem::path src)
            : runtime(rt), source(std::move(src))
        {
            tempDir = std::filesystem::temp_directory_path() / "arcane_plugins" / HostProcessTag();
            ctx.typeContext   = runtime.TypeContext();
            ctx.workScheduler = runtime.WorkScheduler();
            ctx.taskExecutor  = runtime.TaskExecutor();
            ctx.engine        = &runtime;
            RefreshContext();
        }

        void RefreshContext()
        {
            ctx.abiVersion    = kGamePluginABIVersion;
            ctx.imguiContext  = runtime.ImGuiContext();
            ctx.imguiAlloc    = runtime.ImGuiAlloc();
            ctx.imguiFree     = runtime.ImGuiFree();
            ctx.imguiUserData = runtime.ImGuiUserData();
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
            runtime.ResetAudio();
            runtime.ClearSystems();
            // Reset while the module is still loaded: registered component destructors
            // may point into plugin code.
            runtime.ResetRegistry();
            img.plugin.reset();
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
                std::optional<Plugin> p = Plugin::Load(src);
                RefreshContext();
                if (!p || !p->VTable().Init(&ctx))
                {
                    if (p && p->VTable().Shutdown) p->VTable().Shutdown();
                    ShutdownPluginsLive();
                    plugins.clear();
                    ARC_ERROR("plugin: failed to load secondary '{}'", src.generic_string());
                    return false;
                }
                PluginImage img;
                img.plugin = std::move(*p);
                plugins.push_back(std::move(img));
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
                if (img.plugin) img.plugin->VTable().Init(&ctx);
            }
        }

        // The primary module's full reload sequence (copy + ABI + SaveState/LoadState +
        // last-good rollback). Delicate + heavily tested; kept byte-for-byte as the
        // single-module host -- PluginHost::Reload only wraps plugin re-establishment
        // around it, and with no plugins the wrapper is a no-op.
        bool ReloadPrimary(bool restoreState);
    };

    bool PluginHost::Impl::ReloadPrimary(bool restoreState)
    {
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

        std::optional<PluginImage> previous = std::move(current);
        current.reset();
        if (previous)
            TeardownImage(*previous, true);

        if (!restoreState)
            runtime.ResetRegistry();

        std::optional<Plugin> loadedNext = Plugin::Load(next.dll);
        RefreshContext();
        const bool initRan = loadedNext && loadedNext->VTable().Init(&ctx);
        bool ok = initRan;
        if (ok && restoreState)
        {
            Astra::BinaryReader r(snapshot);
            ok = loadedNext->VTable().LoadState(r);
        }

        if (ok)
        {
            next.plugin = std::move(*loadedNext);
            current = std::move(next);
            gen = nextGen;
            if (previous)
                DeleteFiles(*previous);
            ARC_INFO("plugin reloaded (gen {}, snapshot {} bytes)", nextGen, snapshot.size());
            return true;
        }

        if (loadedNext)
        {
            next.plugin = std::move(*loadedNext);
            TeardownImage(next, initRan);
        }
        DeleteFiles(next);
        runtime.ClearSystems();

        bool rolledBack = false;
        if (previous && !previous->dll.empty())
        {
            std::optional<Plugin> rollback = Plugin::Load(previous->dll);
            RefreshContext();
            if (rollback && rollback->VTable().Init(&ctx))
            {
                if (restoreState && !snapshot.empty())
                {
                    Astra::BinaryReader r(snapshot);
                    if (!rollback->VTable().LoadState(r))
                        ARC_ERROR("plugin: rollback LoadState failed; last-good running but state may be lost");
                }
                previous->plugin = std::move(*rollback);
                rolledBack = true;
            }
        }

        if (rolledBack)
        {
            current = std::move(previous);
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

    PluginHost::PluginHost(Runtime& runtime, std::filesystem::path src)
        : m_impl(std::make_unique<Impl>(runtime, std::move(src))) {}

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

    bool PluginHost::Load()
    {
        // See ImGuiContextGuard's comment: a plugin's Init may switch GImGui
        // and never switch it back (Sandbox.cpp:102), so every public entry
        // point restores whatever was current on entry before returning.
        const ImGuiContextGuard imguiGuard;

        // Plugins-only host (no primary game module) -- the editor opening a project that has
        // plugin modules but no gameModule. Skip the primary copy/load/ABI/rollback path and
        // bring up just the secondaries (LoadInitPlugins unwinds itself on failure). With a
        // primary present this branch is never taken, so the delicate path below and every
        // [hotreload] test stay byte-identical.
        if (m_impl->source.empty())
            return m_impl->LoadInitPlugins();

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

        std::optional<Plugin> plugin = Plugin::Load(img.dll);
        m_impl->RefreshContext();
        const bool initRan = plugin && plugin->VTable().Init(&m_impl->ctx);
        if (!initRan)
        {
            if (plugin)
            {
                img.plugin = std::move(*plugin);
                m_impl->TeardownImage(img, false);
            }
            m_impl->DeleteFiles(img);
            ARC_ERROR("plugin: initial load failed");
            return false;
        }

        img.plugin = std::move(*plugin);
        m_impl->current = std::move(img);
        m_impl->gen = g;
        ARC_INFO("plugin loaded (gen {})", g);

        // Bring up the secondary plugins after the primary (they share the Runtime). A
        // plugin failure unwinds the whole session -- no half-loaded host.
        if (!m_impl->LoadInitPlugins())
        {
            Unload();
            return false;
        }
        return true;
    }

    void PluginHost::Unload()
    {
        if (!m_impl->current && m_impl->plugins.empty())
            return;

        // See ImGuiContextGuard's comment (Load() above).
        const ImGuiContextGuard imguiGuard;

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
            // Plugins-only (no primary loaded): do the shared reset here, once.
            m_impl->runtime.ResetAudio();
            m_impl->runtime.ClearSystems();
            m_impl->runtime.ResetRegistry();
        }
        m_impl->plugins.clear();   // unload plugin DLLs AFTER the reset
    }

    bool PluginHost::Reload(bool restoreState)
    {
        // See ImGuiContextGuard's comment (Load() above).
        const ImGuiContextGuard imguiGuard;

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
        m_impl->ShutdownPluginsLive();
        const bool ok = m_impl->ReloadPrimary(restoreState);
        m_impl->InitPluginsLive();
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
        // See ImGuiContextGuard's comment (Load() above). FixedUpdate is not
        // expected to touch ImGui, but the guard is cheap (two pointer ops)
        // and this is a hot per-frame call, so it costs nothing to hold the
        // same "never leaks a context change" guarantee every entry point
        // here makes, rather than special-casing "the ones we know misbehave
        // today."
        const ImGuiContextGuard imguiGuard;
        if (const PluginVTable* vt = Vtable(); vt && vt->FixedUpdate) vt->FixedUpdate(dt);
        for (auto& img : m_impl->plugins)
            if (img.plugin && img.plugin->VTable().FixedUpdate) img.plugin->VTable().FixedUpdate(dt);
    }

    void PluginHost::UpdateAll(double dt, double alpha)
    {
        // See ImGuiContextGuard's comment (Load() above).
        const ImGuiContextGuard imguiGuard;
        if (const PluginVTable* vt = Vtable(); vt && vt->Update) vt->Update(dt, alpha);
        for (auto& img : m_impl->plugins)
            if (img.plugin && img.plugin->VTable().Update) img.plugin->VTable().Update(dt, alpha);
    }

    void PluginHost::DrawUIAll()
    {
        // See ImGuiContextGuard's comment (Load() above). DrawUI is the ONE
        // entry point that is SUPPOSED to touch ImGui (the plugin draws its
        // own HUD into its own offscreen "game" context, composited into the
        // viewport texture by the caller) -- the guard does not interfere
        // with that: it only restores whatever context was current on ENTRY
        // once THIS FUNCTION returns, so every plugin's DrawUI is still free
        // to leave the game context set for as long as it's running.
        const ImGuiContextGuard imguiGuard;
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

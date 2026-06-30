#include "PluginHost.hpp"

#include "Plugin.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>

#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
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
}

struct PluginHost::Impl
{
    Arcane::Runtime&      runtime;
    std::filesystem::path source;
    std::filesystem::path tempDir;
    Arcane::EngineContext ctx{};
    std::optional<PluginImage> current;
    std::uint32_t gen = 0;

    std::filesystem::file_time_type lastWrite{};
    bool                            pending = false;
    std::filesystem::file_time_type pendingWrite{};
    std::chrono::steady_clock::time_point pendingSince{};

    Impl(Arcane::Runtime& rt, std::filesystem::path src)
        : runtime(rt), source(std::move(src))
    {
        tempDir = std::filesystem::temp_directory_path() / "arcane_plugins";
        ctx.typeContext   = runtime.TypeContext();
        ctx.workScheduler = runtime.WorkScheduler();
        ctx.taskExecutor  = runtime.TaskExecutor();
        ctx.engine        = &runtime;
        RefreshContext();
    }

    void RefreshContext()
    {
        ctx.abiVersion    = Arcane::kGamePluginABIVersion;
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

        const Arcane::PluginVTable& vt = img.plugin->VTable();
        if (callShutdown && vt.Shutdown)
            vt.Shutdown();
        // Drop plugin-created audio handles before clearing systems/registry, the
        // same window in which we tear down plugin-owned ECS state. The Loom
        // refactor unified all teardown paths (unload, init-failure, reload-of-
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
};

PluginHost::PluginHost(Arcane::Runtime& runtime, std::filesystem::path src)
    : m_impl(std::make_unique<Impl>(runtime, std::move(src))) {}

PluginHost::~PluginHost()
{
    Unload();
}

bool PluginHost::Load()
{
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
    return true;
}

void PluginHost::Unload()
{
    if (!m_impl->current)
        return;

    m_impl->TeardownLive();
    m_impl->DeleteFiles(*m_impl->current);
    m_impl->current.reset();
}

bool PluginHost::Reload(bool restoreState)
{
    const std::uint32_t nextGen = m_impl->gen + 1;
    PluginImage next;
    if (!m_impl->CopyVersioned(nextGen, next))
        return false;

    std::vector<std::byte> snapshot;
    if (restoreState && m_impl->current && m_impl->current->plugin)
    {
        const Arcane::PluginVTable& vt = m_impl->current->plugin->VTable();
        if (vt.SaveState)
        {
            Astra::BinaryWriter w(snapshot);
            vt.SaveState(w);
            if (w.HasError())
            {
                ARC_ERROR("plugin: SaveState failed; aborting reload, keeping live plugin");
                m_impl->DeleteFiles(next);
                return false;
            }
        }
    }

    std::optional<PluginImage> previous = std::move(m_impl->current);
    m_impl->current.reset();
    if (previous)
        m_impl->TeardownImage(*previous, true);

    if (!restoreState)
        m_impl->runtime.ResetRegistry();

    std::optional<Plugin> loadedNext = Plugin::Load(next.dll);
    m_impl->RefreshContext();
    const bool initRan = loadedNext && loadedNext->VTable().Init(&m_impl->ctx);
    bool ok = initRan;
    if (ok && restoreState)
    {
        Astra::BinaryReader r(snapshot);
        ok = loadedNext->VTable().LoadState(r);
    }

    if (ok)
    {
        next.plugin = std::move(*loadedNext);
        m_impl->current = std::move(next);
        m_impl->gen = nextGen;
        if (previous)
            m_impl->DeleteFiles(*previous);
        ARC_INFO("plugin reloaded (gen {}, snapshot {} bytes)", nextGen, snapshot.size());
        return true;
    }

    if (loadedNext)
    {
        next.plugin = std::move(*loadedNext);
        m_impl->TeardownImage(next, initRan);
    }
    m_impl->DeleteFiles(next);
    m_impl->runtime.ClearSystems();

    if (previous && !previous->dll.empty())
    {
        std::optional<Plugin> rollback = Plugin::Load(previous->dll);
        m_impl->RefreshContext();
        if (rollback && rollback->VTable().Init(&m_impl->ctx))
        {
            if (restoreState && !snapshot.empty())
            {
                Astra::BinaryReader r(snapshot);
                if (!rollback->VTable().LoadState(r))
                    ARC_ERROR("plugin: rollback LoadState failed; last-good running but state may be lost");
            }
            previous->plugin = std::move(*rollback);
        }
    }

    m_impl->current = std::move(previous);
    ARC_ERROR("plugin reload failed (gen {}); rolled back to last-good", nextGen);
    return false;
}

void PluginHost::Poll()
{
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

bool PluginHost::IsLoaded() const noexcept
{
    return m_impl->current.has_value() && m_impl->current->IsLoaded();
}

const Arcane::PluginVTable* PluginHost::Vtable() const noexcept
{
    if (!m_impl->current || !m_impl->current->plugin)
        return nullptr;
    return &m_impl->current->plugin->VTable();
}

std::uint32_t PluginHost::Generation() const noexcept
{
    return m_impl->gen;
}

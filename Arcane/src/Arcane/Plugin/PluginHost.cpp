#include <Arcane/Plugin/PluginHost.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/DynamicLibrary.hpp>

#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace Arcane
{
    namespace
    {
        struct Image
        {
            Detail::LibHandle     handle = nullptr;
            PluginVTable          vt{};
            std::filesystem::path dll;
            std::filesystem::path pdb;
            uint32_t              gen = 0;
        };

        bool Resolve(Detail::LibHandle h, PluginVTable& vt)
        {
            void* a = Detail::DLSym(h, PluginEntry::kABIVersion);
            void* i = Detail::DLSym(h, PluginEntry::kInit);
            void* s = Detail::DLSym(h, PluginEntry::kShutdown);
            void* f = Detail::DLSym(h, PluginEntry::kFixedUpdate);
            void* u = Detail::DLSym(h, PluginEntry::kUpdate);
            void* sv = Detail::DLSym(h, PluginEntry::kSaveState);
            void* ld = Detail::DLSym(h, PluginEntry::kLoadState);
            if (!a || !i || !s || !f || !u || !sv || !ld) return false;
            vt.ABIVersion  = reinterpret_cast<uint32_t(*)()>(a);
            vt.Init        = reinterpret_cast<bool(*)(EngineContext*)>(i);
            vt.Shutdown    = reinterpret_cast<void(*)()>(s);
            vt.FixedUpdate = reinterpret_cast<void(*)(double)>(f);
            vt.Update      = reinterpret_cast<void(*)(double,double)>(u);
            vt.SaveState   = reinterpret_cast<void(*)(Astra::BinaryWriter&)>(sv);
            vt.LoadState   = reinterpret_cast<bool(*)(Astra::BinaryReader&)>(ld);
            return true;
        }
    }

    struct PluginHost::Impl
    {
        Runtime&              runtime;
        std::filesystem::path source;
        std::filesystem::path tempDir;
        EngineContext         ctx{};
        Image                 current;
        uint32_t              gen = 0;

        std::filesystem::file_time_type lastWrite{};
        bool                            pending = false;
        std::filesystem::file_time_type pendingWrite{};
        std::chrono::steady_clock::time_point pendingSince{};

        Impl(Runtime& rt, std::filesystem::path src)
            : runtime(rt), source(std::move(src))
        {
            tempDir = std::filesystem::temp_directory_path() / "arcane_plugins";
            ctx.abiVersion    = kGamePluginABIVersion;
            ctx.typeContext   = runtime.TypeContext();
            ctx.workScheduler = runtime.WorkScheduler();
            ctx.engine        = &runtime;
        }

        bool CopyVersioned(uint32_t g, Image& out)
        {
            std::error_code ec;
            std::filesystem::create_directories(tempDir, ec);
            const std::string stem = source.stem().string();
            out.dll = tempDir / (stem + "_" + std::to_string(g) + ".dll");
            if (!std::filesystem::copy_file(source, out.dll,
                    std::filesystem::copy_options::overwrite_existing, ec) || ec)
                return false;   // source busy (msbuild writing) or missing -> not ready

            std::filesystem::path srcPdb = source; srcPdb.replace_extension(".pdb");
            if (std::filesystem::exists(srcPdb))
            {
                out.pdb = tempDir / (stem + "_" + std::to_string(g) + ".pdb");
                std::filesystem::copy_file(srcPdb, out.pdb,
                    std::filesystem::copy_options::overwrite_existing, ec);  // best-effort
            }
            out.gen = g;
            lastWrite = std::filesystem::last_write_time(source, ec);
            return true;
        }

        void DeleteFiles(const Image& img)
        {
            std::error_code ec;
            if (!img.dll.empty()) std::filesystem::remove(img.dll, ec);
            if (!img.pdb.empty()) std::filesystem::remove(img.pdb, ec);
        }

        void TeardownLive()   // Shutdown + ClearSystems + ResetRegistry + FreeLibrary; KEEP files for rollback
        {
            if (current.handle)
            {
                if (current.vt.Shutdown) current.vt.Shutdown();
                runtime.ClearSystems();
                // MUST reset the registry BEFORE DLClose: the ComponentRegistry's component
                // descriptors (defaultConstruct, destruct, moveConstruct, etc.) were last set by
                // the plugin's ReRegisterComponent<T>() and point into plugin code. Destroying
                // entities after DLClose = call into freed code = SIGSEGV. Reset (which runs
                // destructors on live entities while the plugin is still mapped) makes the
                // registry safe for the Runtime destructor when it runs after the plugin is gone.
                runtime.ResetRegistry();
                Detail::DLClose(current.handle);
                current.handle = nullptr;
            }
        }
    };

    PluginHost::PluginHost(Runtime& runtime, std::filesystem::path src)
        : m_impl(std::make_unique<Impl>(runtime, std::move(src))) {}
    PluginHost::~PluginHost() { Unload(); }

    bool PluginHost::Load()
    {
        const uint32_t g = m_impl->gen + 1;
        Image img;
        // Bounded retry: a freshly built DLL can be transiently read-locked by antivirus.
        // (Reload/Poll need no retry -- the watcher reattempts every frame.)
        bool copied = false;
        for (int attempt = 0; attempt < 5 && !copied; ++attempt)
        {
            copied = m_impl->CopyVersioned(g, img);
            if (!copied) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!copied) { ARC_ERROR("plugin: cannot copy source DLL"); return false; }
        img.handle = Detail::DLOpen(img.dll);
        const bool resolved = img.handle
            && Resolve(img.handle, img.vt)
            && img.vt.ABIVersion() == kGamePluginABIVersion;
        const bool initRan = resolved && img.vt.Init(&m_impl->ctx);
        if (!initRan)
        {
            if (img.handle)
            {
                // Init may have registered systems / created entities before failing; clear
                // them while the DLL is still mapped (their descriptors point into it).
                m_impl->runtime.ClearSystems();
                m_impl->runtime.ResetRegistry();
                Detail::DLClose(img.handle);
            }
            m_impl->DeleteFiles(img);
            ARC_ERROR("plugin: initial load failed");
            return false;
        }
        m_impl->current = img;
        m_impl->gen = g;
        ARC_INFO("plugin loaded (gen {})", g);
        return true;
    }

    void PluginHost::Unload()
    {
        m_impl->TeardownLive();
        m_impl->DeleteFiles(m_impl->current);
        m_impl->current = Image{};
    }

    bool PluginHost::Reload(bool restoreState)
    {
        // 1. copy-first: also the lock/readiness probe. If the source is still being
        //    written by msbuild, leave the live plugin untouched and retry next Poll.
        const uint32_t nextGen = m_impl->gen + 1;
        Image next;
        if (!m_impl->CopyVersioned(nextGen, next))
            return false;

        // 2. snapshot the live plugin's state. If SaveState errored, ABORT while the live
        //    plugin is still fully intact (never tear down against a corrupt snapshot).
        std::vector<std::byte> snapshot;
        if (restoreState && m_impl->current.handle && m_impl->current.vt.SaveState)
        {
            Astra::BinaryWriter w(snapshot);
            m_impl->current.vt.SaveState(w);
            if (w.HasError())
            {
                ARC_ERROR("plugin: SaveState failed; aborting reload, keeping live plugin");
                m_impl->DeleteFiles(next);
                return false;
            }
        }

        // 3. tear down the live plugin (resets the registry while it is still mapped; keeps
        //    its versioned files for rollback).
        Image previous = m_impl->current;
        m_impl->TeardownLive();

        // 4. load + (fresh-reset) + init + (optionally) restore the new image.
        if (!restoreState)
            m_impl->runtime.ResetRegistry();   // covers the no-current-plugin case; harmless when TeardownLive already reset
        next.handle = Detail::DLOpen(next.dll);
        const bool resolved = next.handle
            && Resolve(next.handle, next.vt)
            && next.vt.ABIVersion() == kGamePluginABIVersion;
        const bool initRan = resolved && next.vt.Init(&m_impl->ctx);
        bool ok = initRan;
        if (ok && restoreState)
        {
            Astra::BinaryReader r(snapshot);
            ok = next.vt.LoadState(r);
        }

        if (ok)
        {
            m_impl->current = next;
            m_impl->gen = nextGen;
            m_impl->DeleteFiles(previous);
            ARC_INFO("plugin reloaded (gen {}, snapshot {} bytes)", nextGen, snapshot.size());
            return true;
        }

        // 5. FAILURE -> rollback to the previous last-good image; never a lost session.
        //    Shutdown only if Init actually ran (ABI mismatch never ran Init); reset the
        //    registry before DLClose so any entities a failed Init created don't outlive it.
        if (next.handle)
        {
            if (initRan && next.vt.Shutdown) next.vt.Shutdown();
            m_impl->runtime.ResetRegistry();
            Detail::DLClose(next.handle);
        }
        m_impl->DeleteFiles(next);
        m_impl->runtime.ClearSystems();
        if (!previous.dll.empty())
        {
            previous.handle = Detail::DLOpen(previous.dll);
            if (previous.handle && Resolve(previous.handle, previous.vt) && previous.vt.Init(&m_impl->ctx))
            {
                if (restoreState && !snapshot.empty())
                {
                    Astra::BinaryReader r(snapshot);
                    if (!previous.vt.LoadState(r))
                        ARC_ERROR("plugin: rollback LoadState failed; last-good running but state may be lost");
                }
            }
        }
        m_impl->current = previous;
        ARC_ERROR("plugin reload failed (gen {}); rolled back to last-good", nextGen);
        return false;
    }

    void PluginHost::Poll()
    {
        std::error_code ec;
        const auto wt = std::filesystem::last_write_time(m_impl->source, ec);
        if (ec) return;
        if (wt == m_impl->lastWrite) { m_impl->pending = false; return; }

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
            Reload(true);   // copy-first inside handles a still-locked file
        }
    }

    bool                PluginHost::IsLoaded()   const noexcept { return m_impl->current.handle != nullptr; }
    const PluginVTable* PluginHost::Vtable()     const noexcept { return m_impl->current.handle ? &m_impl->current.vt : nullptr; }
    uint32_t            PluginHost::Generation() const noexcept { return m_impl->gen; }
}

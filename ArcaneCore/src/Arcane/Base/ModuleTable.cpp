// Crash window plan 1 (Task 3): ModuleTable implementation. See the header
// for the "why" -- this file is the double buffer: two fixed arrays, a
// mutex on the write side, an atomic index and a lock-free linear scan on
// the read side. Nothing here allocates from the heap or throws.

#include <Arcane/Base/ModuleTable.hpp>

#include <atomic>
#include <cstring>
#include <mutex>

namespace Arcane::Diagnostics
{
    namespace
    {
        // Two fixed-size buffers; g_active names which one readers see.
        // File-scope (not function-local statics) so Refresh can index them
        // directly under the write mutex without a static-init race on the
        // hot Find() path.
        ModuleEntry       g_tables[2][ModuleTable::kMax];
        std::size_t       g_counts[2] = { 0, 0 };
        std::atomic<int>  g_active{0};
        std::mutex        g_writeMutex;
    }

    void ModuleTable::Refresh(std::span<const ForeignModules::LoadedModule> modules) noexcept
    {
        std::lock_guard lock(g_writeMutex);

        const int active   = g_active.load(std::memory_order_relaxed);
        const int inactive = 1 - active;

        const std::size_t count = modules.size() < kMax ? modules.size() : kMax;
        for (std::size_t i = 0; i < count; ++i)
        {
            ModuleEntry& e = g_tables[inactive][i];
            e.base = modules[i].base;
            e.size = modules[i].size;
            strncpy_s(e.name, modules[i].name.c_str(), _TRUNCATE);
        }
        g_counts[inactive] = count;

        // Publishes the new buffer: every write above is visible to any
        // Find() that observes this store (its load is memory_order_acquire).
        g_active.store(inactive, std::memory_order_release);
    }

    const ModuleEntry* ModuleTable::Find(std::uint64_t address) noexcept
    {
        const int active = g_active.load(std::memory_order_acquire);
        const std::size_t count = g_counts[active];
        for (std::size_t i = 0; i < count; ++i)
        {
            const ModuleEntry& e = g_tables[active][i];
            if (address >= e.base && address < e.base + e.size)
                return &e;
        }
        return nullptr;
    }

    std::size_t ModuleTable::Count() noexcept
    {
        const int active = g_active.load(std::memory_order_acquire);
        return g_counts[active];
    }
}

#pragma once

// Asset bookkeeping ported from the client's cache.lua: one entry per key
// with refcount + byte-size accounting and an LRU "used" tick. A cached
// FAILURE is a real entry with no object, so callers distinguish "miss"
// (never tried) from "known-failed" (do not retry). Header-only and
// engine-internal; the exported surface is the Assets facade.
//
// Lua divergence note: cache.lua's put() initialises refs=1 on new entries
// and uses obj=false as the failure sentinel. The C++ version separates
// concerns: refs default to 0 (caller controls via Acquire/Release) and a
// dedicated `failed` bool avoids T{} being ambiguous with a valid
// default-constructed T. Semantics stay faithful -- failures are real cache
// entries, Get returns T{} for both miss and failure, Has+IsFailure give
// callers all the information they need.

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Arcane
{
    template <typename T>
    class AssetCache
    {
    public:
        // Default: a self-contained recency clock.
        AssetCache() = default;

        // Shared recency clock: several caches tick ONE counter, so their
        // `used` stamps are globally comparable and a facade-level sweep can
        // pick a true cross-cache LRU (the Assets byte-budget path).
        explicit AssetCache(uint64_t* sharedClock)
        {
            if (sharedClock)
                m_tick = sharedClock;
        }

        // Non-copyable/movable: m_tick may alias m_ownTick, and no caller
        // needs value semantics (caches live as facade members).
        AssetCache(const AssetCache&) = delete;
        AssetCache& operator=(const AssetCache&) = delete;

        bool Has(const std::string& key) const
        {
            return m_entries.find(key) != m_entries.end();
        }

        bool IsFailure(const std::string& key) const
        {
            auto it = m_entries.find(key);
            return it != m_entries.end() && it->second.failed;
        }

        // Returns the object (bumping recency) or a default-constructed T
        // for both miss and failure -- pair with Has/IsFailure.
        T Get(const std::string& key)
        {
            auto it = m_entries.find(key);
            if (it == m_entries.end() || it->second.failed)
                return T{};
            it->second.used = ++(*m_tick);
            return it->second.obj;
        }

        void Put(const std::string& key, T obj, uint64_t bytes)
        {
            Entry& e = m_entries[key];
            // Unsigned wraparound is intentional and exact: two's-complement
            // modular arithmetic makes total + (bytes - e.bytes) correct even
            // when bytes < e.bytes (update shrinks the entry). The running
            // total never goes below zero because we only put real sizes.
            m_totalBytes += bytes - e.bytes;
            e.obj = std::move(obj);
            e.bytes = bytes;
            e.failed = false;
            e.used = ++(*m_tick);
        }

        void PutFailure(const std::string& key)
        {
            Entry& e = m_entries[key];
            // e.bytes is 0 for a fresh entry, so the subtraction is a no-op there.
            m_totalBytes -= e.bytes;
            e.obj = T{};
            e.bytes = 0;
            e.failed = true;
            e.used = ++(*m_tick);
        }

        void Acquire(const std::string& key)
        {
            auto it = m_entries.find(key);
            if (it != m_entries.end())
                ++it->second.refs;
        }

        void Release(const std::string& key)
        {
            auto it = m_entries.find(key);
            if (it != m_entries.end() && it->second.refs > 0)
                --it->second.refs;
        }

        // Removes the entry unless pinned by refs. Returns true on removal.
        bool Evict(const std::string& key)
        {
            auto it = m_entries.find(key);
            if (it == m_entries.end() || it->second.refs > 0)
                return false;
            m_totalBytes -= it->second.bytes;
            m_entries.erase(it);
            return true;
        }

        // Oldest unpinned entry's key, or empty when none (LRU sweep seam).
        // O(n) scan -- adequate for v1 cache sizes.
        std::string LeastRecentKey() const
        {
            std::string best;
            uint64_t bestUsed = UINT64_MAX;
            for (const auto& [key, e] : m_entries)
            {
                if (e.refs == 0 && e.used < bestUsed)
                {
                    bestUsed = e.used;
                    best = key;
                }
            }
            return best;
        }

        // Budget-sweep candidate: the oldest unpinned entry with bytes > 0.
        // Unlike LeastRecentKey, memoized failures (0 bytes) are skipped --
        // evicting them frees nothing and destroys their do-not-retry memo.
        // Writes the key + its recency stamp (comparable across caches on a
        // shared clock) and returns true; false when nothing is evictable.
        // O(n) scan -- same v1 tradeoff as LeastRecentKey.
        bool LeastRecentEvictable(std::string& keyOut, uint64_t& usedOut) const
        {
            bool found = false;
            uint64_t bestUsed = UINT64_MAX;
            for (const auto& [key, e] : m_entries)
            {
                if (e.refs == 0 && e.bytes > 0 && e.used < bestUsed)
                {
                    bestUsed = e.used;
                    keyOut = key;
                    usedOut = e.used;
                    found = true;
                }
            }
            return found;
        }

        // Drops every entry (objects + memoized failures) and resets the byte
        // total. The recency tick is left monotonic. Used when the backing
        // resource an entry depends on changes (e.g. the render device rebinds).
        void Clear()
        {
            m_entries.clear();
            m_totalBytes = 0;
        }

        uint64_t TotalBytes() const { return m_totalBytes; }
        size_t Count() const { return m_entries.size(); }

    private:
        struct Entry
        {
            T obj{};
            uint32_t refs = 0;
            uint64_t bytes = 0;
            uint64_t used = 0;
            bool failed = false;
        };

        std::unordered_map<std::string, Entry> m_entries;
        uint64_t m_ownTick = 0;
        uint64_t* m_tick = &m_ownTick;   // own clock unless shared (see ctor)
        uint64_t m_totalBytes = 0;
    };
}

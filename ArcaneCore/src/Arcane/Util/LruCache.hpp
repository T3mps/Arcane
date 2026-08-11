#pragma once

// Reusable single-threaded LRU cache. Bounded by a capacity set at
// construction; inserting beyond the cap evicts the least-recently-used
// entry. Every Get touches the entry (moves it to the MRU end); Peek is
// the const, no-touch variant for diagnostic lookups.
//
// Lookup and insertion are O(1). EraseIf is O(n) — used by sweep paths
// that need to walk the whole list (e.g. RateLimiter's age-based
// cleanup).
//
// NOT thread-safe. The caller owns concurrency; the typical pattern is
// to hold an external std::mutex across any LruCache mutation. This
// matches RateLimiter's existing m_mutex contract.
//
// Designed for the RateLimiter audit M-V2-10 fix (2026-06-02) — the
// old fixed-cap unordered_map refused new inserts once the cap was hit,
// which let a unique-key spray (e.g. 10k IPs) block legit users from
// the limiter. LRU eviction guarantees forward progress for new entries
// at the cost of evicting whichever existing entry is genuinely
// least-recently-touched — exactly the entry an attacker spray hasn't
// re-probed.
//
// Type parameters:
//   K     — key type (must be hashable, equality-comparable)
//   V     — value type (must be move-constructible)
//   Hash  — optional custom hasher (defaults to std::hash<K>)

#include <cstddef>
#include <functional>
#include <list>
#include <unordered_map>
#include <utility>

namespace Arcane
{
    template <typename K, typename V, typename Hash = std::hash<K>>
    class LruCache
    {
    public:
        explicit LruCache(std::size_t capacity)
            : m_capacity(capacity == 0 ? 1 : capacity)
        {
            // Reserve to roughly the cap so the hash map doesn't rehash
            // mid-life. unordered_map's default load factor is 1.0, so
            // capacity is sufficient.
            m_map.reserve(m_capacity);
        }

        std::size_t Capacity() const noexcept { return m_capacity; }
        std::size_t Size() const noexcept     { return m_list.size(); }
        bool        Empty() const noexcept    { return m_list.empty(); }

        // Returns a mutable pointer to the value, or nullptr if absent.
        // Touches the entry (moves it to MRU). Caller may mutate *V in
        // place — the touch happens before returning, so even a no-op
        // mutation still refreshes recency. Lifetime is tied to the
        // cache; pointer is invalidated by any subsequent Put / Erase /
        // EraseIf / Clear.
        V* Get(const K& key)
        {
            auto it = m_map.find(key);
            if (it == m_map.end()) return nullptr;
            Touch(it->second);
            return &it->second->second;
        }

        // Read-only lookup that does NOT touch the entry. Use for
        // diagnostic / cooldown-status queries that shouldn't count as
        // a fresh access.
        const V* Peek(const K& key) const
        {
            auto it = m_map.find(key);
            if (it == m_map.end()) return nullptr;
            return &it->second->second;
        }

        // Audit H-V3-11 (2026-06-03): mutable no-touch lookup. Callers
        // that need to update the value without counting as a fresh
        // access (e.g. RateLimiter incrementing attempts on a path that
        // may still reject) use this in tandem with explicit Touch() on
        // their accept branch. Touching on a reject is what M-V2-10
        // accidentally did via Get; that inverted the LRU policy in the
        // attacker's favor (sustained spray keeps attacker entries MRU,
        // legit users drift toward eviction).
        V* Peek(const K& key)
        {
            auto it = m_map.find(key);
            if (it == m_map.end()) return nullptr;
            return &it->second->second;
        }

        // Audit H-V3-11 (2026-06-03): explicit MRU refresh. No-op if the
        // key is absent. Returns true if an entry was touched. Pair with
        // Peek() (mutable) when the caller wants "mutate, decide, maybe
        // touch" semantics.
        bool Touch(const K& key)
        {
            auto it = m_map.find(key);
            if (it == m_map.end()) return false;
            Touch(it->second);
            return true;
        }

        // Insert or update. Touches the entry (moves to MRU). When
        // inserting a new key at capacity, evicts the LRU entry first.
        // Returns a pointer to the stored value (always non-null).
        V* Put(const K& key, V value)
        {
            auto it = m_map.find(key);
            if (it != m_map.end())
            {
                it->second->second = std::move(value);
                Touch(it->second);
                return &it->second->second;
            }

            if (m_list.size() >= m_capacity)
            {
                EvictLru();
            }

            // Audit L-Q3-5 (2026-06-03): try_emplace constructs the map
            // entry in place. Previous form `m_map[key] = m_list.begin()`
            // default-constructed a ListIter at the slot then move-assigned
            // — a wasted default-construct + assign cycle on every fresh
            // insert. The list emplace already owns one copy of the key.
            m_list.emplace_front(key, std::move(value));
            m_map.try_emplace(key, m_list.begin());
            return &m_list.front().second;
        }

        // Erase by key. Returns true if an entry was removed.
        bool Erase(const K& key)
        {
            auto it = m_map.find(key);
            if (it == m_map.end()) return false;
            m_list.erase(it->second);
            m_map.erase(it);
            return true;
        }

        // Walk the list and erase entries where pred(key, value)
        // returns true. Iteration is MRU→LRU. Returns the number of
        // entries removed. Predicate must NOT mutate the cache.
        template <typename Pred>
        std::size_t EraseIf(Pred pred)
        {
            std::size_t removed = 0;
            for (auto it = m_list.begin(); it != m_list.end(); )
            {
                if (pred(static_cast<const K&>(it->first),
                         static_cast<const V&>(it->second)))
                {
                    m_map.erase(it->first);
                    it = m_list.erase(it);
                    ++removed;
                }
                else
                {
                    ++it;
                }
            }
            return removed;
        }

        void Clear() noexcept
        {
            m_list.clear();
            m_map.clear();
        }

        // Diagnostic: peek at the key that would be evicted next. Returns
        // nullptr when the cache is empty. Does NOT touch the entry, and
        // does not return a value pointer (use Peek for that).
        const K* LruKey() const
        {
            if (m_list.empty()) return nullptr;
            return &m_list.back().first;
        }

    private:
        using Entry    = std::pair<K, V>;
        using ListType = std::list<Entry>;
        using ListIter = typename ListType::iterator;

        // Splice the entry to the MRU end (front). std::list::splice is
        // O(1) and preserves iterator validity, so the map's stored
        // iterators stay good.
        void Touch(ListIter it)
        {
            if (it != m_list.begin())
                m_list.splice(m_list.begin(), m_list, it);
        }

        // Drop the entry at the LRU end (back). Assumes non-empty.
        void EvictLru()
        {
            const auto& victim = m_list.back();
            m_map.erase(victim.first);
            m_list.pop_back();
        }

        std::size_t m_capacity;
        ListType    m_list;   // front = MRU, back = LRU
        std::unordered_map<K, ListIter, Hash> m_map;
    };

} // namespace Arcane

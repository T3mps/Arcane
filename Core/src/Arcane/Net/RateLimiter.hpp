#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <Arcane/Util/Logger.hpp>
#include <Arcane/Util/LruCache.hpp>

namespace Arcane
{
    // Global rate limiting toggle.
    //
    // DEFAULT: enabled. Audit C3 (2026-06-02) â€” leaving rate limiting off in
    // Release turned every documented limit (login, registration, pulls, debug
    // AddCurrency) into a no-op and gave authenticated clients an unbounded
    // mint/brute-force surface.
    //
    // To disable for local dev, pass `--no-rate-limit` on the CLI of any
    // service. That parser branch is compiled out of Release builds (NDEBUG),
    // so a shipped binary can't be coaxed into the off state from arguments.
    // If you need to flip this at runtime in a Release build for a controlled
    // test, do it from gated server-only code and log loudly.
    inline std::atomic<bool> g_rateLimitingEnabled{true};

    class RateLimiter
    {
    public:
        // Audit M8 (2026-06-02) â€” hard cap on the per-key map so an IP-spray
        // attacker can't inflate memory by churning unique keys.
        //
        // Audit M-V2-10 (2026-06-02) â€” eviction policy switched from M8's
        // pure "refuse new entry at cap" to LRU eviction (see LruCache.hpp).
        // The old policy fully blocked legit users from the limiter once a
        // unique-key spray filled the cap; legit logins came back "rate
        // limited" with no recourse. LRU eviction guarantees forward
        // progress: a new key always gets a slot, at the cost of evicting
        // the genuinely least-recently-touched existing entry â€” exactly
        // the entry an attacker spray hasn't re-probed. Sustained attacker
        // presence still doesn't grow memory past the cap; total churn is
        // bounded by it; legit users never lose access as a side-effect
        // of someone else's spray.
        //
        // The 10-min idle cleanup is preserved as memory hygiene â€” keeps
        // the cache size small when the service is mostly idle. The 30s
        // AGGRESSIVE_SWEEP path that M8 added is gone (no longer needed â€”
        // LRU handles cap pressure automatically).
        static constexpr std::size_t MAX_RECORDS = 10000;

        struct Config
        {
            int maxAttempts = 5;
            int windowSeconds = 60;
            int cooldownSeconds = 30;
        };

        RateLimiter() : m_records(MAX_RECORDS) {}

        bool Allow(const std::string& key, const Config& config)
        {
            // If rate limiting is globally disabled, always allow
            if (!g_rateLimitingEnabled)
                return true;

            std::lock_guard<std::mutex> lock(m_mutex);
            auto now = std::chrono::steady_clock::now();

            CleanupExpired(now);

            // Audit H-V3-11 (2026-06-03): use Peek (no-touch) for the
            // initial lookup and Touch() explicitly on accept branches
            // only. Previously Get touched on every probe — including
            // cooldown-rejected ones — which inverted M-V2-10's stated
            // goal: a sustained attacker spray kept attacker entries at
            // MRU and pushed legit users' (older but valid) entries
            // toward eviction. The invariant now is: touch iff allow.
            Record* record = m_records.Peek(key);
            if (record == nullptr)
            {
                // New entry. Put touches naturally (inserts at MRU).
                // LruCache evicts the LRU entry automatically when at cap.
                m_records.Put(key, Record{now, 1, false, now});
                return true;
            }

            if (record->inCooldown)
            {
                auto cooldownEnd = record->cooldownStart + std::chrono::seconds(config.cooldownSeconds);
                if (now < cooldownEnd)
                {
                    LOG_AUTH_TRACE("Rate limit: {} still in cooldown", key);
                    return false;   // reject: no touch
                }
                LOG_AUTH_DEBUG("Rate limit: {} cooldown expired, resetting", key);
                *record = {now, 1, false, now};
                m_records.Touch(key);
                return true;
            }

            auto windowEnd = record->windowStart + std::chrono::seconds(config.windowSeconds);
            if (now >= windowEnd)
            {
                *record = {now, 1, false, now};
                m_records.Touch(key);
                return true;
            }

            record->attempts++;
            if (record->attempts > config.maxAttempts)
            {
                record->inCooldown = true;
                record->cooldownStart = now;
                LOG_AUTH_WARN("Rate limit triggered for {}: {} attempts in {}s, cooldown {}s",
                    key, record->attempts, config.windowSeconds, config.cooldownSeconds);
                return false;   // reject: no touch (attacker just tripped limit)
            }

            m_records.Touch(key);
            return true;
        }

        int GetCooldownRemaining(const std::string& key, const Config& config)
        {
            // If rate limiting is globally disabled, no cooldown
            if (!g_rateLimitingEnabled)
                return 0;

            std::lock_guard<std::mutex> lock(m_mutex);
            auto now = std::chrono::steady_clock::now();

            // Diagnostic query â€” Peek does NOT touch the LRU position. A
            // "how long is my cooldown?" call shouldn't refresh the
            // entry's recency relative to a genuine Allow() attempt.
            const Record* record = m_records.Peek(key);
            if (record == nullptr || !record->inCooldown)
                return 0;

            auto cooldownEnd = record->cooldownStart + std::chrono::seconds(config.cooldownSeconds);
            if (now >= cooldownEnd)
                return 0;

            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(cooldownEnd - now);
            return static_cast<int>(remaining.count());
        }

        void Reset(const std::string& key)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_records.Erase(key);
        }

    private:
        struct Record
        {
            std::chrono::steady_clock::time_point windowStart;
            int attempts = 0;
            bool inCooldown = false;
            std::chrono::steady_clock::time_point cooldownStart;
        };

        void CleanupExpired(std::chrono::steady_clock::time_point now)
        {
            if (++m_cleanupCounter < 100)
                return;
            m_cleanupCounter = 0;

            auto expiry = now - std::chrono::minutes(10);
            m_records.EraseIf([&](const std::string&, const Record& r) {
                return r.windowStart < expiry && r.cooldownStart < expiry;
            });
        }

        LruCache<std::string, Record> m_records;
        std::mutex m_mutex;
        int m_cleanupCounter = 0;
    };

    // Rate limit configs
    namespace RateLimits
    {
        // Login: 10 attempts per minute, 30s cooldown
        inline RateLimiter::Config Login() { return {10, 60, 30}; }

        // Registration: 5 attempts per minute, 60s cooldown
        inline RateLimiter::Config Registration() { return {5, 60, 60}; }

        // Pulls: 120 per minute (~2/sec), 30s cooldown
        inline RateLimiter::Config Pulls() { return {120, 60, 30}; }

        // Add currency (debug): 30 per minute, 30s cooldown
        inline RateLimiter::Config AddCurrency() { return {30, 60, 30}; }
    }
} // namespace Arcane

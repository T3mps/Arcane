// RateLimiter behavioral coverage (E03-1). The limiter reads
// steady_clock directly (no time-injection seam), so determinism comes
// from the seam the API already has: Config's integer seconds accept 0,
// which places every subsequent call exactly at/past the expiry
// boundary. windowSeconds=0 exercises the inclusive `now >= windowEnd`
// reset edge; cooldownSeconds=0 exercises the exclusive `now <
// cooldownEnd` cooldown-expiry edge. Non-zero windows (60s/3600s) are
// used where the test must stay INSIDE the window -- the suite runs in
// milliseconds, so those never expire mid-test. No sleeps, no
// production seam added.
//
// LRU-eviction behavior (cap = RateLimiter::MAX_RECORDS) is observed
// through the public API only: an evicted key loses its record, so a
// previously-tripped key that suddenly Allow()s again proves eviction,
// and a denied (N+1)th attempt proves record continuity (survival).
#include <atomic>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Net/RateLimiter.hpp>

using Arcane::RateLimiter;

namespace
{
    using Config = RateLimiter::Config;

    // Drive `count` fresh unique keys through Allow(). Each insert lands
    // at MRU and pushes older entries toward the LRU end (and past it,
    // once the cache is at cap). Keys are globally unique across calls
    // so spray batches never collide with each other.
    void Spray(RateLimiter& limiter, const Config& cfg, std::size_t count)
    {
        static std::atomic<unsigned long long> next{0};
        for (std::size_t i = 0; i < count; ++i)
            limiter.Allow("spray_" + std::to_string(next++), cfg);
    }

    // RAII save/flip/restore for the global kill switch so a test can
    // never leak a disabled limiter into the rest of the suite.
    struct ToggleGuard
    {
        bool saved;
        explicit ToggleGuard(bool value)
            : saved(Arcane::g_rateLimitingEnabled.load())
        {
            Arcane::g_rateLimitingEnabled = value;
        }
        ~ToggleGuard() { Arcane::g_rateLimitingEnabled = saved; }
    };
}

TEST_CASE("RateLimiter: allows exactly maxAttempts in a window, then denies", "[ratelimiter]")
{
    RateLimiter limiter;
    const Config cfg{3, 60, 60};

    // Attempts 1..maxAttempts are allowed (attempts > maxAttempts is
    // the trip condition, so the boundary attempt is still allowed).
    REQUIRE(limiter.Allow("k", cfg));
    REQUIRE(limiter.Allow("k", cfg));
    REQUIRE(limiter.Allow("k", cfg));

    // Attempt maxAttempts+1 trips the limit; further probes stay denied.
    REQUIRE_FALSE(limiter.Allow("k", cfg));
    REQUIRE_FALSE(limiter.Allow("k", cfg));
}

TEST_CASE("RateLimiter: tripping the limit starts a reported cooldown", "[ratelimiter]")
{
    RateLimiter limiter;
    const Config cfg{1, 60, 30};

    REQUIRE(limiter.Allow("k", cfg));
    REQUIRE_FALSE(limiter.Allow("k", cfg));   // trip -> cooldown

    // duration_cast truncation may shave the first second off.
    const int remaining = limiter.GetCooldownRemaining("k", cfg);
    REQUIRE(remaining > 0);
    REQUIRE(remaining <= cfg.cooldownSeconds);

    // Probing during cooldown keeps rejecting and does not clear it.
    REQUIRE_FALSE(limiter.Allow("k", cfg));
    REQUIRE(limiter.GetCooldownRemaining("k", cfg) > 0);
}

TEST_CASE("RateLimiter: window expiry resets the attempt budget", "[ratelimiter]")
{
    RateLimiter limiter;
    // windowSeconds=0: every call after the first sits exactly at/past
    // windowEnd, exercising the inclusive `now >= windowEnd` reset edge.
    const Config cfg{1, 0, 60};

    for (int i = 0; i < 5; ++i)
        REQUIRE(limiter.Allow("k", cfg));
}

TEST_CASE("RateLimiter: cooldown expiry resets the record", "[ratelimiter]")
{
    RateLimiter limiter;
    // cooldownSeconds=0: cooldownEnd == cooldownStart, so `now <
    // cooldownEnd` is false on the very next call -- expired.
    const Config cfg{1, 60, 0};

    REQUIRE(limiter.Allow("k", cfg));
    REQUIRE_FALSE(limiter.Allow("k", cfg));                 // trip
    REQUIRE(limiter.GetCooldownRemaining("k", cfg) == 0);   // already over
    REQUIRE(limiter.Allow("k", cfg));                       // expired -> reset, attempt 1
    REQUIRE_FALSE(limiter.Allow("k", cfg));                 // fresh window trips again
}

TEST_CASE("RateLimiter: GetCooldownRemaining is 0 for unknown or untripped keys", "[ratelimiter]")
{
    RateLimiter limiter;
    const Config cfg{5, 60, 30};

    REQUIRE(limiter.GetCooldownRemaining("never_seen", cfg) == 0);

    REQUIRE(limiter.Allow("k", cfg));
    REQUIRE(limiter.GetCooldownRemaining("k", cfg) == 0);
}

TEST_CASE("RateLimiter: Reset clears a tripped key immediately", "[ratelimiter]")
{
    RateLimiter limiter;
    const Config cfg{1, 60, 3600};

    REQUIRE(limiter.Allow("k", cfg));
    REQUIRE_FALSE(limiter.Allow("k", cfg));   // trip
    REQUIRE_FALSE(limiter.Allow("k", cfg));

    limiter.Reset("k");
    REQUIRE(limiter.Allow("k", cfg));
    REQUIRE(limiter.GetCooldownRemaining("k", cfg) == 0);
}

TEST_CASE("RateLimiter: keys are isolated from each other", "[ratelimiter]")
{
    RateLimiter limiter;
    const Config cfg{2, 60, 3600};

    // Trip A.
    REQUIRE(limiter.Allow("a", cfg));
    REQUIRE(limiter.Allow("a", cfg));
    REQUIRE_FALSE(limiter.Allow("a", cfg));

    // B has its own untouched budget...
    REQUIRE(limiter.Allow("b", cfg));
    REQUIRE(limiter.Allow("b", cfg));
    REQUIRE_FALSE(limiter.Allow("b", cfg));   // ...and its own trip point.

    // A's cooldown never bled into a fresh key.
    REQUIRE(limiter.GetCooldownRemaining("a", cfg) > 0);
    REQUIRE(limiter.GetCooldownRemaining("c", cfg) == 0);
    REQUIRE(limiter.Allow("c", cfg));
}

TEST_CASE("RateLimiter: global disable bypasses limiting without erasing records", "[ratelimiter]")
{
    RateLimiter limiter;
    const Config cfg{1, 60, 3600};

    REQUIRE(limiter.Allow("k", cfg));
    REQUIRE_FALSE(limiter.Allow("k", cfg));   // trip while enabled

    {
        ToggleGuard off(false);
        // Disabled: everything is allowed and no cooldown is reported.
        REQUIRE(limiter.Allow("k", cfg));
        REQUIRE(limiter.Allow("k", cfg));
        REQUIRE(limiter.GetCooldownRemaining("k", cfg) == 0);
    }

    // Re-enabled: the pre-disable cooldown record is still in force --
    // the kill switch bypasses, it does not wipe state.
    REQUIRE_FALSE(limiter.Allow("k", cfg));
    REQUIRE(limiter.GetCooldownRemaining("k", cfg) > 0);
}

// ---- LRU eviction under key-spray pressure (cap = MAX_RECORDS) -----------

TEST_CASE("RateLimiter: a unique-key spray evicts the LRU entry at cap", "[ratelimiter]")
{
    RateLimiter limiter;
    const Config cfg{1, 3600, 3600};

    // Trip the victim, then confirm it is locked out.
    REQUIRE(limiter.Allow("victim", cfg));
    REQUIRE_FALSE(limiter.Allow("victim", cfg));
    REQUIRE_FALSE(limiter.Allow("victim", cfg));

    // MAX_RECORDS fresh keys push the victim (the oldest entry) out.
    Spray(limiter, cfg, RateLimiter::MAX_RECORDS);

    // The evicted victim gets a brand-new record: allowed again. This is
    // the M-V2-10 forward-progress guarantee observed from the outside.
    REQUIRE(limiter.Allow("victim", cfg));
}

TEST_CASE("RateLimiter: an allowed key is touched to MRU and survives spray pressure", "[ratelimiter]")
{
    RateLimiter limiter;
    const Config cfg{10, 3600, 3600};

    REQUIRE(limiter.Allow("legit", cfg));               // attempt 1

    // Fill the cache to exactly cap; "legit" is now the LRU entry.
    Spray(limiter, cfg, RateLimiter::MAX_RECORDS - 1);

    // An accepted attempt touches the entry back to MRU (attempt 2).
    REQUIRE(limiter.Allow("legit", cfg));

    // 5000 more unique keys evict 5000 stale spray entries -- but not
    // the freshly-touched legit key.
    Spray(limiter, cfg, 5000);

    // Record continuity proof: attempts 3..10 are still allowed, and the
    // 11th is denied. Had "legit" been evicted, the counter would have
    // restarted and the final attempt would be allowed.
    for (int attempt = 3; attempt <= 10; ++attempt)
        REQUIRE(limiter.Allow("legit", cfg));
    REQUIRE_FALSE(limiter.Allow("legit", cfg));
}

TEST_CASE("RateLimiter: cooldown-rejected probes do not refresh recency", "[ratelimiter]")
{
    // Audit H-V3-11 invariant: touch iff allow. An attacker hammering a
    // key that is in cooldown must NOT keep that entry at MRU -- it
    // drifts to LRU and gets evicted like any other stale entry.
    RateLimiter limiter;
    const Config cfg{1, 3600, 3600};

    REQUIRE(limiter.Allow("attacker", cfg));
    REQUIRE_FALSE(limiter.Allow("attacker", cfg));      // trip (no touch)

    // 9998 fresh keys: cache size = 9999, attacker is the LRU entry.
    Spray(limiter, cfg, RateLimiter::MAX_RECORDS - 2);

    // Hammer the cooldown-rejected key. None of these may touch it.
    for (int i = 0; i < 5; ++i)
        REQUIRE_FALSE(limiter.Allow("attacker", cfg));

    // Two more inserts: the first fills the cache to cap, the second
    // evicts the LRU entry -- which must still be "attacker" despite
    // the probes above.
    Spray(limiter, cfg, 2);

    // Evicted -> fresh record -> allowed.
    REQUIRE(limiter.Allow("attacker", cfg));
}

// MeshCache -- debt 14 (F2c debts arc closeout doc, "Standing debts from the fix
// wave's scoped re-review"): Clear() forgot warnedNeverRequested, so a project
// switch could silently swallow the new project's first pre-Request Query().
// There is no existing MeshCache test file (it is otherwise only exercised
// indirectly through SceneRenderResolverTest.cpp), so this is a small, narrowly
// scoped addition rather than an extension of something that already existed.
//
// Uses the log-capture idiom mirrored from SpriteMaterialCacheTest.cpp/
// SerializationNegativeTest.cpp/MaterialGraphTest.cpp -- Query()'s own comment
// documents the WARN as latched, and that latch is unobservable through
// Query()'s RETURN VALUE alone (PendingCook comes back identically whether the
// WARN fired or was suppressed), so a WARN COUNT is the only way to pin the
// reset this debt adds.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/MeshCache.hpp>

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <memory>

using namespace Arcane;

namespace
{
    // Mirrored verbatim from SpriteMaterialCacheTest.cpp's own copy, counting
    // occurrences instead of keeping only the last -- this test needs to tell
    // "warned once" from "warned twice", not just capture the wording. Filtered
    // to WARN level (final-review M4): the unfiltered version incremented on
    // ANY message reaching Log::Engine(), so an unrelated INFO -- a future
    // addition inside Query(), a background worker -- could turn
    // CHECK(warns == 1) into a spurious red that reads as a debt-14 regression.
    std::shared_ptr<spdlog::sinks::callback_sink_mt> AttachLogCounter(int& count)
    {
        auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&count](const spdlog::details::log_msg& msg)
            {
                if (msg.level == spdlog::level::warn)
                    ++count;
            });
        Log::Engine()->sinks().push_back(cb);
        return cb;
    }
    void DetachLogCapture(const std::shared_ptr<spdlog::sinks::callback_sink_mt>& cb)
    {
        auto& sinks = Log::Engine()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), cb), sinks.end());
    }
}

TEST_CASE("mesh cache: Query on a never-requested guid warns once, and Clear() "
          "lets it warn again", "[render][mesh]")
{
    MeshCache::Services services;   // resolveAsset left unset -- Request() is never
                                     // called by this test, only Query()
    MeshCache cache(services);

    int warns = 0;
    auto cb = AttachLogCounter(warns);

    const Guid probe = Guid::Generate();
    CHECK(cache.Query(probe) == MeshResolveState::PendingCook);
    CHECK(warns == 1);

    // Latched: a second Query for the SAME never-requested guid does not warn
    // again, and neither does a different never-requested guid -- Query's own
    // comment documents this as a one-shot latch, not a per-guid one.
    CHECK(cache.Query(probe) == MeshResolveState::PendingCook);
    CHECK(cache.Query(Guid::Generate()) == MeshResolveState::PendingCook);
    CHECK(warns == 1);

    // Debt 14: Clear() (the project-switch contract) resets the latch alongside
    // `requested`, so the new project's first pre-Request Query() is a fresh
    // instance of the same situation, not a repeat this process has already
    // reported.
    cache.Clear();
    CHECK(cache.Query(probe) == MeshResolveState::PendingCook);
    CHECK(warns == 2);

    DetachLogCapture(cb);
}

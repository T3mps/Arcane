#pragma once

// Routes Mosaic guard failures into Catch2, and lets a test REQUIRE that a
// guard fires.
//
// PER-MODULE SLOT: Mosaic::detail::g_assertHandler is an `inline` atomic, so
// each binary has its own. Installing here covers guards compiled INTO the test
// exe. Guards inside ArcaneClient.dll are routed by that module's own
// installer -- see Arcane/Base/Assert.hpp, whose two-part exported-handler +
// inline-installer shape exists for exactly this reason.
//
// The handler returns AssertAction::Continue, never Break: an unattended test
// process must not execute an int3, which is the same reasoning Mosaic's own
// IsDebuggerPresent guard gives (Assert.hpp:53-62).
//
// THREAD PRECONDITION -- NOT ENFORCED: Handler() below mutates s_active's
// m_count/m_lastMessage/m_lastExpression with no synchronization, and
// s_active is a plain (non-atomic) pointer. Mosaic::detail::g_assertHandler
// is a process/module-global slot, not thread-local, so Handler() runs on
// WHATEVER thread fires a guard while a scope is alive. ArcaneAssertScope
// therefore requires every guard it observes to fire on the constructing
// thread only. If a guard instead fires on another thread while a scope is
// alive, this is a genuine data race, not merely a wrong count: concurrent
// unsynchronized writes to std::string are undefined behaviour. This is safe
// for every test in this file (all single-threaded); it is deliberately NOT
// hardened with atomics/a mutex/thread_local here, since the right choice
// among those depends on a multi-threaded use this scope does not have yet.

#include <Mosaic/Assert.hpp>

// PUBLIC Catch2 headers only. catch_test_macros.hpp is what defines
// REQUIRE/CHECK and pulls in whatever internals they expand to, so naming
// catch2/internal/catch_decomposer.hpp here bought nothing and made this
// header depend on a path Catch2 is free to move between versions -- an
// upgrade hazard for no benefit. The vendored 3.15.0 builds identically
// without it.
#include <catch2/catch_test_macros.hpp>

#include <string>

// Counts guard failures for the duration of its scope and restores whatever
// handler was installed before it -- including another scope's, so nesting is
// safe. Reports the last message so a test can assert WHICH guard fired, not
// merely that one did.
class ArcaneAssertScope
{
public:
    ArcaneAssertScope()
        : m_previous(Mosaic::detail::g_assertHandler.load(std::memory_order_acquire))
        , m_previousUser(Mosaic::detail::g_assertUser.load(std::memory_order_acquire))
    {
        s_active = this;
        Mosaic::SetAssertHandler(&ArcaneAssertScope::Handler, nullptr);
    }

    ~ArcaneAssertScope()
    {
        Mosaic::SetAssertHandler(m_previous, m_previousUser);
        s_active = m_outer;
    }

    ArcaneAssertScope(const ArcaneAssertScope&)            = delete;
    ArcaneAssertScope& operator=(const ArcaneAssertScope&) = delete;

    [[nodiscard]] int Count() const noexcept { return m_count; }
    [[nodiscard]] const std::string& LastMessage() const noexcept { return m_lastMessage; }
    [[nodiscard]] const std::string& LastExpression() const noexcept { return m_lastExpression; }

private:
    static Mosaic::AssertAction Handler(const Mosaic::AssertContext& ctx, void*) noexcept
    {
        if (s_active)
        {
            ++s_active->m_count;
            s_active->m_lastMessage    = ctx.message    ? ctx.message    : "";
            s_active->m_lastExpression = ctx.expression ? ctx.expression : "";
        }
        return Mosaic::AssertAction::Continue;
    }

    Mosaic::AssertHandler m_previous;
    void*                 m_previousUser;
    ArcaneAssertScope*    m_outer = s_active;
    int                   m_count = 0;
    std::string           m_lastMessage;
    std::string           m_lastExpression;

    static inline ArcaneAssertScope* s_active = nullptr;
};

// Require/check that the wrapped expression makes a Mosaic guard fire. The
// inverse direction of the routing above, and the reason a guard's own
// behaviour is testable at all.
#define ARC_INTERNAL_ASSERT_FIRES(expr, requireIt)                                 \
    do {                                                                           \
        ArcaneAssertScope arcScope_;                                               \
        (void)(expr);                                                              \
        if (requireIt) {                                                           \
            REQUIRE(arcScope_.Count() > 0);                                        \
        } else {                                                                   \
            CHECK(arcScope_.Count() > 0);                                          \
        }                                                                          \
    } while (false)

#define REQUIRE_ARC_ENSURE(expr) ARC_INTERNAL_ASSERT_FIRES(expr, true)
#define CHECK_ARC_ENSURE(expr)   ARC_INTERNAL_ASSERT_FIRES(expr, false)
#define REQUIRE_ARC_ASSERT(expr) ARC_INTERNAL_ASSERT_FIRES(expr, true)
#define CHECK_ARC_ASSERT(expr)   ARC_INTERNAL_ASSERT_FIRES(expr, false)

// GPU crash diagnostics arc, Task 3: GpuBreadcrumbs is a pure CPU-side ring
// that derives, from marker-completion evidence a backend reports, which
// named scopes were still in flight when a GPU queue died. Pure unit -- no
// GPU/OS dependency; every call is exercised directly (no test double).
//
// Naming note: the brief describes the derivation accessor loosely as
// "Snapshot(...) const" returning a nested `struct Snapshot`. A method
// literally named the same as its own nested return type both fails to
// compile out-of-line and fails inside its own body (MSVC C2761/C2371 --
// the C++ "struct stat" hiding rule only tolerates the bare in-class
// declaration, not a usable implementation). The nested `Snapshot{
// lastCompleted, inFlight }` shape is kept exactly as specified; the
// accessor is named `Capture()` instead. See task-3-report.md.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/GpuBreadcrumbs.hpp>

#include <string>

using namespace Arcane;

TEST_CASE("GpuBreadcrumbs begin/end pairing yields empty inFlight and lastCompleted = last ended scope", "[diag]")
{
    GpuBreadcrumbs bc;

    const std::uint32_t idA = bc.BeginScope("PassA");
    bc.OnMarkerWritten(idA, true);
    bc.EndScope(idA);
    bc.OnMarkerWritten(idA, false);

    const std::uint32_t idB = bc.BeginScope("PassB");
    bc.OnMarkerWritten(idB, true);
    bc.EndScope(idB);
    bc.OnMarkerWritten(idB, false);

    const GpuBreadcrumbs::Snapshot snap = bc.Capture();
    CHECK(snap.inFlight.empty());
    CHECK(snap.lastCompleted == "PassB");
}

TEST_CASE("GpuBreadcrumbs reports a begun-not-ended scope in inFlight together with its still-open ancestors", "[diag]")
{
    GpuBreadcrumbs bc;

    // "A" is opened but never gets a marker report of its own (simulating a
    // backend that only confirmed the deepest marker before the device
    // died) -- it must still surface via the ancestor-by-depth rule (no
    // end marker observed), not via any begin-marker evidence of its own.
    const std::uint32_t idA = bc.BeginScope("A");
    const std::uint32_t idB = bc.BeginScope("B"); // nested inside A
    bc.OnMarkerWritten(idB, true);
    (void)idA;

    const GpuBreadcrumbs::Snapshot snap = bc.Capture();
    REQUIRE(snap.inFlight.size() == 2);
    CHECK(snap.inFlight[0] == "A");
    CHECK(snap.inFlight[1] == "B");
    CHECK(snap.lastCompleted.empty());
}

TEST_CASE("GpuBreadcrumbs includes a CPU-closed ancestor in inFlight when its own end marker was never observed", "[diag]")
{
    GpuBreadcrumbs bc;

    // The realistic crash scenario this tool exists for: the CPU races
    // ahead of the GPU, so EndScope fires for BOTH Outer and Inner
    // immediately, back-to-back, long before the GPU catches up. A backend
    // then confirms only Inner's begin marker before the device dies --
    // Outer never gets ANY marker report of its own, and its EndScope
    // (CPU-side) already fired. Outer must still surface as in-flight: GPU
    // execution order guarantees an enclosing scope's end marker is
    // written after its descendants', so Inner still running means Outer
    // is too -- CPU-side EndScope state must not gate this.
    const std::uint32_t outer = bc.BeginScope("Outer");
    const std::uint32_t inner = bc.BeginScope("Inner"); // nested inside Outer
    bc.EndScope(inner);
    bc.EndScope(outer);
    bc.OnMarkerWritten(inner, true);

    const GpuBreadcrumbs::Snapshot snap = bc.Capture();
    REQUIRE(snap.inFlight.size() == 2);
    CHECK(snap.inFlight[0] == "Outer");
    CHECK(snap.inFlight[1] == "Inner");
    CHECK(snap.lastCompleted.empty());
}

TEST_CASE("GpuBreadcrumbs ring eviction keeps derivation correct past capacity", "[diag]")
{
    GpuBreadcrumbs bc;

    constexpr int kTotal = static_cast<int>(GpuBreadcrumbs::kRingCapacity) + 44; // push well past capacity
    constexpr int kOpenTail = 10; // last kOpenTail scopes: end marker never confirmed

    for (int i = 0; i < kTotal; ++i)
    {
        const std::uint32_t id = bc.BeginScope("scope" + std::to_string(i));
        bc.OnMarkerWritten(id, true);
        bc.EndScope(id); // always closed CPU-side immediately -> every scope stays flat (depth 0)
        if (i < kTotal - kOpenTail)
            bc.OnMarkerWritten(id, false);
        // else: the GPU is simulated to have died before confirming this scope's end marker.
    }

    const GpuBreadcrumbs::Snapshot snap = bc.Capture();

    // The oldest (kTotal - kRingCapacity) scopes were evicted; derivation
    // must still be correct over exactly what remains.
    REQUIRE(snap.inFlight.size() == static_cast<std::size_t>(kOpenTail));
    CHECK(snap.inFlight.front() == "scope" + std::to_string(kTotal - kOpenTail));
    CHECK(snap.inFlight.back() == "scope" + std::to_string(kTotal - 1));
    CHECK(snap.lastCompleted == "scope" + std::to_string(kTotal - kOpenTail - 1));
}

TEST_CASE("GpuBreadcrumbs Snapshot with zero markers reports empty strings and never crashes", "[diag]")
{
    SECTION("nothing was ever begun")
    {
        GpuBreadcrumbs bc;
        const GpuBreadcrumbs::Snapshot snap = bc.Capture();
        CHECK(snap.lastCompleted.empty());
        CHECK(snap.inFlight.empty());
    }

    SECTION("a scope was begun but no marker was ever reported for it")
    {
        GpuBreadcrumbs bc;
        (void)bc.BeginScope("Lonely");
        const GpuBreadcrumbs::Snapshot snap = bc.Capture();
        CHECK(snap.lastCompleted.empty());
        CHECK(snap.inFlight.empty());
    }

    SECTION("OnMarkerWritten/EndScope on an id that was never issued is a safe no-op")
    {
        GpuBreadcrumbs bc;
        bc.OnMarkerWritten(9999, true);
        bc.OnMarkerWritten(9999, false);
        bc.EndScope(9999);
        const GpuBreadcrumbs::Snapshot snap = bc.Capture();
        CHECK(snap.lastCompleted.empty());
        CHECK(snap.inFlight.empty());
    }
}

TEST_CASE("GpuBreadcrumbs ring capacity is 256 scopes per queue", "[diag]")
{
    CHECK(GpuBreadcrumbs::kRingCapacity == 256);
}

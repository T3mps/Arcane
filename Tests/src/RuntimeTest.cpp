// Runtime is the engine facade EngineContext.engine points at. It owns the substrate
// that outlives reloads: TypeContext (installed in Arcane.dll), persistent
// ComponentRegistry, a swappable Registry, schedulers, RunLoop, and the JobSystem.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Audio/AudioDevice.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/RegistrySnapshot.hpp>
#include <Arcane/Serialization/ResourceSerialization.hpp>

#include <Astra/Serialization/SerializationError.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/Reflection.hpp>

#include <atomic>
#include <utility>

namespace { struct Counter { int value = 0; }; }
namespace { ASTRA_REFLECT_TYPE(Counter) ASTRA_REFLECT_FIELD(Counter, value) ASTRA_END_REFLECT_TYPE() }

// A void(Registry&) LAMBDA does NOT satisfy Astra's LambdaLike (that concept is for
// per-entity lambdas); systems must be NAMED types registered via AddSystem<T>().
namespace { struct NoOpSystem { void operator()(Astra::Registry&) const {} }; }

TEST_CASE("Runtime boots a usable substrate", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.TypeContext() != nullptr);
    REQUIRE(rt.WorkScheduler() != nullptr);
    REQUIRE(rt.WorkScheduler()->WorkerCount() >= 1);
    REQUIRE(rt.AudioSystem().IsInitialized());
    CHECK(rt.AssetsFacade().Stats().count == 0);
    REQUIRE(rt.TaskExecutor() != nullptr);
    REQUIRE(rt.TaskExecutor()->WorkerCount() >= 1);

    rt.Components()->RegisterComponent<Counter>();
    auto& reg = rt.Registry();
    for (int i = 0; i < 8; ++i) reg.CreateEntityWith(Counter{i});

    int seen = 0;
    reg.CreateView<Counter>().ForEach([&](Astra::Entity, Counter&) { ++seen; });
    CHECK(seen == 8);
}

TEST_CASE("Runtime resets audio without disturbing the engine substrate", "[runtime][audio]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.AudioSystem().IsInitialized());

    rt.ResetAudio();

    CHECK(rt.AudioSystem().IsInitialized());
    CHECK(rt.WorkScheduler() != nullptr);
    CHECK(rt.Registry().IsEmpty());
}

TEST_CASE("Runtime snapshot/restore preserves state AND the scheduler", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Counter>();
    auto& reg = rt.Registry();
    constexpr int kN = 2048;
    for (int i = 0; i < kN; ++i) reg.CreateEntityWith(Counter{7});

    auto snapResult = rt.SnapshotRegistry();
    REQUIRE(snapResult.IsOk());
    std::vector<std::byte>& snap = *snapResult.GetValue();
    REQUIRE(!snap.empty());

    // Mutate the live registry, then restore the snapshot.
    reg.CreateView<Counter>().ForEach([](Astra::Entity, Counter& c) { c.value = 0; });
    REQUIRE(rt.RestoreRegistry(snap));

    std::atomic<int> visited{0};
    std::atomic<int> sum{0};
    rt.Registry().CreateView<Counter>().ParallelForEach([&](Astra::Entity, Counter& c) {
        visited.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(c.value, std::memory_order_relaxed);
    });
    CHECK(visited.load() == kN);     // state survived
    CHECK(sum.load() == 7 * kN);     // values survived (== 7, not the mutated 0)
}

TEST_CASE("Runtime RestoreRegistry keeps the RunLoop object stable", "[runtime]")
{
    // A restore swaps the live registry, which the RunLoop references. It MUST rebind
    // the existing loop in place, NOT destroy + recreate it: consumers cache the
    // RunLoop* handed out by Runtime::Loop() at init (a plugin's SetLoop, a host
    // toolbar), and recreating would leave every such pointer dangling -> a
    // use-after-free the next time they touch it. This pins the "same object survives a
    // restore" invariant.
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Counter>();
    rt.Registry().CreateEntityWith(Counter{7});

    Arcane::RunLoop* before = &rt.Loop();
    rt.Loop().SetPaused(true);           // observable pre-restore state

    auto snapResult = rt.SnapshotRegistry();
    REQUIRE(snapResult.IsOk());
    REQUIRE(rt.RestoreRegistry(*snapResult.GetValue()));

    Arcane::RunLoop* after = &rt.Loop();
    CHECK(before == after);              // SAME object -> a cached RunLoop* stays valid
    CHECK_FALSE(after->IsPaused());      // rebind resets transient sim-time state to defaults

    // ResetRegistry holds the same invariant.
    rt.ResetRegistry();
    CHECK(&rt.Loop() == before);
}

TEST_CASE("Runtime ClearSystems empties all phase schedulers", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Schedulers().fixedUpdate.AddSystem<NoOpSystem>();   // each scheduler has its own
    rt.Schedulers().update.AddSystem<NoOpSystem>();        // type index, so reusing the
    rt.Schedulers().render.AddSystem<NoOpSystem>();        // same type across them is fine
    rt.ClearSystems();
    CHECK(rt.Schedulers().fixedUpdate.Empty());
    CHECK(rt.Schedulers().update.Empty());
    CHECK(rt.Schedulers().render.Empty());
}

TEST_CASE("Runtime ResetRegistry empties the registry but keeps the ComponentRegistry", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Counter>();
    rt.Registry().CreateEntityWith(Counter{42});
    REQUIRE(rt.Registry().Size() == 1);

    rt.ResetRegistry();
    CHECK(rt.Registry().IsEmpty());

    // The shared ComponentRegistry still knows Counter -> this must not crash and must land.
    rt.Registry().CreateEntityWith(Counter{1});
    CHECK(rt.Registry().Size() == 1);
}

// E02-4: a Save failure must surface as an actionable Result error, not an
// empty-but-"ok" snapshot. Registry::Save() to memory is infallible, so the
// propagation contract is proven through the pure FinishSnapshot seam and the
// Runtime call site's success path.
TEST_CASE("FinishSnapshot propagates a Save failure instead of masking it", "[runtime][serialization]")
{
    using SR = Arcane::Serialization::SnapshotResult;

    auto failed = Arcane::Serialization::FinishSnapshot(SR::Err(Astra::SerializationError::IOError));
    REQUIRE(failed.IsErr());
    CHECK(*failed.GetError() == Astra::SerializationError::IOError);

    std::vector<std::byte> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
    auto ok = Arcane::Serialization::FinishSnapshot(SR::Ok(bytes));
    REQUIRE(ok.IsOk());
    CHECK(ok.GetValue()->size() == 3);
}

TEST_CASE("Runtime SnapshotRegistry returns an actionable Result", "[runtime][serialization]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Counter>();
    rt.Registry().CreateEntityWith(Counter{7});

    auto snap = rt.SnapshotRegistry();
    REQUIRE(snap.IsOk());
    CHECK_FALSE(snap.GetValue()->empty());
}

// E02-1: a snapshot->restore round-trip must preserve registered serializable
// resources, not just entities/components. SceneRoot is registered by default;
// a second resource type is registered here to prove the seam handles a SET of
// resources (not a SceneRoot special case).
namespace
{
    struct CameraSnapshot { float zoom = 1.0f; float x = 0.0f; float y = 0.0f; };

    bool SaveCamera(const Astra::Registry& reg, Astra::BinaryWriter& w)
    {
        const CameraSnapshot* c = reg.GetResource<CameraSnapshot>();
        if (!c) return false;
        w(c->zoom); w(c->x); w(c->y);
        return true;
    }
    bool LoadCamera(Astra::Registry& reg, Astra::BinaryReader& r)
    {
        CameraSnapshot c;
        r(c.zoom); r(c.x); r(c.y);
        if (r.HasError()) return false;
        reg.SetResource<CameraSnapshot>(std::move(c));
        return true;
    }
}

TEST_CASE("Runtime snapshot/restore round-trips registered serializable resources", "[runtime][serialization][scene]")
{
    // Register the extra resource codec into the process-wide set the engine
    // snapshots with (idempotent -- safe if another test already registered it).
    Arcane::Serialization::SerializableResources().Register(
        Arcane::Serialization::ResourceCodec{
            Astra::TypeID<CameraSnapshot>::Hash(), &SaveCamera, &LoadCamera });

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    Astra::Entity root = rt.Registry().CreateEntity();
    rt.Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    rt.Registry().SetResource<CameraSnapshot>(CameraSnapshot{2.5f, 10.0f, 20.0f});

    auto snap = rt.SnapshotRegistry();
    REQUIRE(snap.IsOk());

    // Clobber both resources on the live registry, then restore from the snapshot.
    rt.Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{Astra::Entity::Invalid()});
    rt.Registry().SetResource<CameraSnapshot>(CameraSnapshot{});
    REQUIRE(rt.RestoreRegistry(*snap.GetValue()));

    // Both survived (RestoreRegistry swapped the registry, so re-fetch it).
    const Arcane::SceneRoot* sr = rt.Registry().GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    CHECK(sr->entity == root);                 // entity id survived Save/Load
    CHECK(rt.Registry().IsValid(sr->entity));  // and still resolves in the loaded registry

    const CameraSnapshot* cam = rt.Registry().GetResource<CameraSnapshot>();
    REQUIRE(cam != nullptr);
    CHECK(cam->zoom == Catch::Approx(2.5f));
    CHECK(cam->x == Catch::Approx(10.0f));
    CHECK(cam->y == Catch::Approx(20.0f));
}

// Fix 4 (review): drive Runtime::RestoreRegistry's resources.IsErr() branch
// end-to-end (the isolated corruption cases for ReadResourceSection itself
// are covered in SerializationNegativeTest.cpp). A perfectly valid registry
// blob paired with a corrupt resource-section tail must fail cleanly and
// leave the live world completely untouched -- RestoreRegistry is documented
// as transactional (load into a local registry first, only swap on full
// success), so this proves the resource-section failure actually aborts the
// swap rather than partially applying it.
TEST_CASE("Runtime RestoreRegistry rejects a valid registry blob with a corrupt resource section", "[runtime][serialization]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Counter>();
    rt.Registry().CreateEntityWith(Counter{42});

    auto snap = rt.SnapshotRegistry();
    REQUIRE(snap.IsOk());

    // Split the valid frame, then re-frame the SAME valid registry blob with a
    // deliberately corrupt resource section (count claims an entry that isn't
    // there -> CorruptedData in ReadResourceSection).
    auto frame = Arcane::Serialization::ParseSnapshot(*snap.GetValue());
    REQUIRE(frame.IsOk());
    const std::vector<std::byte> registryBlob(frame.GetValue()->registry.begin(),
                                               frame.GetValue()->registry.end());

    std::vector<std::byte> corruptSection;
    Astra::BinaryWriter w(corruptSection);
    w(static_cast<uint32_t>(1));   // count = 1, but no entry bytes follow

    const std::vector<std::byte> bytes = Arcane::Serialization::FrameBytes(registryBlob, corruptSection);

    CHECK_FALSE(rt.RestoreRegistry(bytes));

    // World untouched: the live registry still has its original entity/value,
    // proving RestoreRegistry did not swap in the (registry-valid-but-
    // resource-corrupt) loaded registry.
    int seen = 0;
    rt.Registry().CreateView<Counter>().ForEach([&](Astra::Entity, Counter& c)
    {
        ++seen;
        CHECK(c.value == 42);
    });
    CHECK(seen == 1);
}

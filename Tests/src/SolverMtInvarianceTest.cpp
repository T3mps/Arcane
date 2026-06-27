#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>

using namespace Arcane::Physics;

TEST_CASE("PhysicsWorld accepts an executor and steps with it (serial default)", "[physics][solvermt]")
{
    PhysicsWorld w{};
    Arcane::SerialTaskExecutor serial;
    w.SetExecutor(&serial);                 // explicit serial
    w.Step(1.0f / 60.0f);                   // must not crash; serial path unchanged
    SUCCEED("stepped with an injected executor");

    PhysicsWorld w2{};
    w2.SetExecutor(nullptr);                // null -> falls back to the world's serial default
    w2.Step(1.0f / 60.0f);
    SUCCEED("stepped with null executor (serial fallback)");
}

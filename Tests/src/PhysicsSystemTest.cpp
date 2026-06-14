// M6 P3.3 -- PhysicsSystem + PhysicsResource + transform sync.
//
// Tests:
//   1. Dynamic body under gravity: after N fixed steps, LocalTransform.position.y
//      has increased (fallen), and WorldTransform.matrix[2].y agrees (propagation ran).
//   2. Kinematic body with authored velocity: after N steps, LocalTransform.position.x
//      has increased proportionally to velocity * dt * N.
//   3. Remove: after DestroyEntity, the body row is removed from the PhysicsResource's
//      entityToBody map on the next PhysicsSystem invocation.
//   4. Determinism: two identical runs of the same scene yield exactly the same
//      final LocalTransform positions (binary ==, not Approx).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>

using Catch::Approx;

namespace
{
    // Fixed timestep used throughout: 60 Hz.
    constexpr float kDt = 1.0f / 60.0f;
    // Gravity magnitude: +Y down, matching the dynamics test convention.
    constexpr float kGravityY = 400.0f;
    // Kinematic body speed (pixels/s, +X direction).
    constexpr float kKinSpeed = 100.0f;

    // ---------------------------------------------------------------------------
    // Build a registry + resource for use by all tests.
    //   - SceneRoot entity (no physics)
    //   - Dynamic body entity (circle r=0.5, at origin, no initial velocity)
    //   - Kinematic body entity (circle r=0.5, at (50,0), velocity=(kKinSpeed,0))
    // Returns {registry, root, dynEntity, kinEntity}.
    // ---------------------------------------------------------------------------
    struct SceneHandles
    {
        std::shared_ptr<Astra::ComponentRegistry> components;
        Astra::Registry*                          reg = nullptr;
        Astra::Entity                             root{};
        Astra::Entity                             dyn{};
        Astra::Entity                             kin{};
    };

    SceneHandles BuildScene(Astra::Registry& reg)
    {
        Arcane::RegisterSceneComponents(reg);
        Arcane::RegisterPhysicsComponents(reg);

        // Physics world resource: gravity +Y down.
        Arcane::Physics::WorldDef wd;
        wd.gravityY = kGravityY;
        reg.SetResource(Arcane::PhysicsResource{
            std::make_unique<Arcane::Physics::PhysicsWorld>(wd),
            {}
        });

        // SceneRoot (no physics, just anchors the hierarchy).
        Astra::Entity root = reg.CreateEntity();
        Arcane::LocalTransform rootLT;
        rootLT.position = glm::vec2(0.0f, 0.0f);
        reg.AddComponent<Arcane::LocalTransform>(root, rootLT);
        reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

        // Dynamic body entity: free-falls under gravity.
        Astra::Entity dyn = reg.CreateEntity();
        {
            Arcane::LocalTransform lt; lt.position = glm::vec2(0.0f, 0.0f);
            reg.AddComponent<Arcane::LocalTransform>(dyn, lt);
            reg.AddComponent<Arcane::WorldTransform>(dyn, Arcane::WorldTransform{});

            Arcane::RigidBody2D rb;
            rb.type = Arcane::Physics::BodyType::Dynamic;
            reg.AddComponent<Arcane::RigidBody2D>(dyn, rb);

            Arcane::Collider2D col;
            col.kind   = Arcane::Physics::ShapeKind::Circle;
            col.radius = 0.5f;
            reg.AddComponent<Arcane::Collider2D>(dyn, col);

            reg.AddComponent<Arcane::PhysicsBodyRef>(dyn, Arcane::PhysicsBodyRef{});
            reg.SetParent(dyn, root);
        }

        // Kinematic body entity: constant velocity in +X.
        Astra::Entity kin = reg.CreateEntity();
        {
            Arcane::LocalTransform lt; lt.position = glm::vec2(50.0f, 0.0f);
            reg.AddComponent<Arcane::LocalTransform>(kin, lt);
            reg.AddComponent<Arcane::WorldTransform>(kin, Arcane::WorldTransform{});

            Arcane::RigidBody2D rb;
            rb.type     = Arcane::Physics::BodyType::Kinematic;
            rb.velocity = glm::vec2(kKinSpeed, 0.0f);
            reg.AddComponent<Arcane::RigidBody2D>(kin, rb);

            Arcane::Collider2D col;
            col.kind   = Arcane::Physics::ShapeKind::Circle;
            col.radius = 0.5f;
            reg.AddComponent<Arcane::Collider2D>(kin, col);

            reg.AddComponent<Arcane::PhysicsBodyRef>(kin, Arcane::PhysicsBodyRef{});
            reg.SetParent(kin, root);
        }

        SceneHandles h;
        h.reg  = &reg;
        h.root = root;
        h.dyn  = dyn;
        h.kin  = kin;
        return h;
    }

    // Run N fixed steps via PhysicsSystem (physics only, not via RunLoop, so we
    // can test in isolation), then run TransformPropagationSystem once.
    void RunNSteps(Astra::Registry& reg, int n)
    {
        Arcane::PhysicsSystem   physics(kDt);
        Arcane::TransformPropagationSystem propagate;
        for (int i = 0; i < n; ++i)
        {
            physics(reg);
        }
        propagate(reg);
    }
} // anonymous namespace

// ---------------------------------------------------------------------------
// TEST 1 — Dynamic body falls under gravity
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsSystem: dynamic body falls under gravity (LocalTransform updated)", "[physics]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    auto h = BuildScene(reg);

    constexpr int kSteps = 10;
    RunNSteps(reg, kSteps);

    const auto* lt = reg.GetComponent<Arcane::LocalTransform>(h.dyn);
    REQUIRE(lt != nullptr);

    // After 10 steps at 60 Hz with gravityY = 400 (+Y down), y must be well positive.
    // Forward-Euler lower bound: 0.5 * g * (dt*N)^2 ≈ 0.5*400*(10/60)^2 ≈ 5.6 px.
    // A bound of 5.0f is safely below the true displacement but well above 0, so
    // a near-zero or sign-flipped gravity is caught.
    CHECK(lt->position.y > 5.0f);

    // WorldTransform must match: TransformPropagationSystem ran; root is at origin,
    // so the child's world matrix column 2 = its local position.
    const auto* wt = reg.GetComponent<Arcane::WorldTransform>(h.dyn);
    REQUIRE(wt != nullptr);
    CHECK(wt->matrix[2].y == Approx(lt->position.y).margin(1e-4f));
}

// ---------------------------------------------------------------------------
// TEST 2 — Kinematic body moves at authored velocity
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsSystem: kinematic body moves by authored velocity", "[physics]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    auto h = BuildScene(reg);

    constexpr int   kSteps = 30;
    const float startX = 50.0f;

    RunNSteps(reg, kSteps);

    const auto* lt = reg.GetComponent<Arcane::LocalTransform>(h.kin);
    REQUIRE(lt != nullptr);

    // Kinematic body at (50, 0), velocity (kKinSpeed, 0):
    // expected X = 50 + kKinSpeed * kDt * kSteps (exact forward-Euler, one Step per
    // invocation, constant dt).  The margin(0.01f) is an absolute sub-pixel tolerance
    // that only absorbs float accumulation; it is intentionally tight to catch
    // off-by-one-step or dt-scale bugs.
    const float expectedX = startX + kKinSpeed * kDt * static_cast<float>(kSteps);
    CHECK(lt->position.x == Approx(expectedX).margin(0.01f));

    // Y should stay near zero (no gravity on kinematics, no vertical velocity).
    CHECK(std::abs(lt->position.y) < 1.0f);
}

// ---------------------------------------------------------------------------
// TEST 3 — Body row removed when entity is destroyed
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsSystem: body row removed after entity is destroyed", "[physics]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    auto h = BuildScene(reg);

    // Run one step to register both bodies.
    {
        Arcane::PhysicsSystem physics(kDt);
        physics(reg);
    }

    // Confirm both entities are tracked in the resource.
    {
        const auto* res = reg.GetResource<Arcane::PhysicsResource>();
        REQUIRE(res != nullptr);
        CHECK(res->entityToBody.count(h.dyn) == 1);
        CHECK(res->entityToBody.count(h.kin) == 1);
    }

    // Destroy the dynamic entity.
    reg.DestroyEntity(h.dyn);

    // Run another step: PhysicsSystem should detect the dead entity and remove its row.
    {
        Arcane::PhysicsSystem physics(kDt);
        physics(reg);
    }

    {
        const auto* res = reg.GetResource<Arcane::PhysicsResource>();
        REQUIRE(res != nullptr);
        CHECK(res->entityToBody.count(h.dyn) == 0);  // removed
        CHECK(res->entityToBody.count(h.kin) == 1);  // still alive
    }
}

// ---------------------------------------------------------------------------
// TEST 4 — Determinism: two identical runs produce exactly the same positions
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsSystem: two identical runs yield bit-exact LocalTransform positions", "[physics]")
{
    constexpr int kSteps = 20;

    // ---- Run A ----
    glm::vec2 posA_dyn{}, posA_kin{};
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry regA(components);
        auto hA = BuildScene(regA);
        RunNSteps(regA, kSteps);
        posA_dyn = regA.GetComponent<Arcane::LocalTransform>(hA.dyn)->position;
        posA_kin = regA.GetComponent<Arcane::LocalTransform>(hA.kin)->position;
    }

    // ---- Run B (fresh registry, identical setup) ----
    glm::vec2 posB_dyn{}, posB_kin{};
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry regB(components);
        auto hB = BuildScene(regB);
        RunNSteps(regB, kSteps);
        posB_dyn = regB.GetComponent<Arcane::LocalTransform>(hB.dyn)->position;
        posB_kin = regB.GetComponent<Arcane::LocalTransform>(hB.kin)->position;
    }

    // Exact binary equality: same code path, same inputs, same f32 ops.
    CHECK(posA_dyn.x == posB_dyn.x);
    CHECK(posA_dyn.y == posB_dyn.y);
    CHECK(posA_kin.x == posB_kin.x);
    CHECK(posA_kin.y == posB_kin.y);
}

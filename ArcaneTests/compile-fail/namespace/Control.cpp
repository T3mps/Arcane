// Control for scripts/namespace-compile-fail.ps1. This TU must compile:
// the game include names World, and the ECS facade names Entity.
#include <Arcane/Ecs.hpp>
#include <Arcane/Physics2D.hpp>

#include <type_traits>

static_assert(std::is_class_v<Arcane::Physics2D::World>);
static_assert(std::is_same_v<Arcane::Entity, Astra::Entity>);

int main()
{
    return 0;
}

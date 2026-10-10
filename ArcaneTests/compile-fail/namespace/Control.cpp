// Control for scripts/namespace-compile-fail.ps1. This TU must compile:
// the flat gameplay names, and the engine-internal Phys alias.
#include <Arcane/Ecs.hpp>
#include <Arcane/Physics2D.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>

#include <type_traits>

static_assert(std::is_class_v<Arcane::PhysicsWorld2D>);
static_assert(std::is_same_v<Arcane::Entity, Astra::Entity>);
static_assert(std::is_class_v<Arcane::View<int>>);
static_assert(std::is_class_v<Arcane::RigidBody2D>);
static_assert(std::is_class_v<Arcane::Detail::Physics2D::Phys::PhysicsWorld>);

int main()
{
    return 0;
}

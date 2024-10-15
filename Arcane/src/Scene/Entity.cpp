#include "arcpch.h"
#include "Entity.h"

ARC::Entity::Entity(entt::entity handle, Scene* scene) : m_handle(handle), m_scene(scene) {}

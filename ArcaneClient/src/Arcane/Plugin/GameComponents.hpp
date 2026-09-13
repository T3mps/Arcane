#pragma once

// GameComponents: a game module's component roster, self-registered.
//
//   // Health.cpp
//   #include "Health.hpp"
//   #include <Arcane/Plugin/GameComponents.hpp>
//   ARCANE_COMPONENT(MyGame::Health)
//
//   // GamePlugin_Init, ONCE, after SetTypeContext:
//   g_module = new Astra::ComponentModule(
//       Astra::ComponentModule::Open(ctx->engine->Components(), "MyGame"));
//   Arcane::Game::RegisterComponents(*g_module);
//
// Each ARCANE_COMPONENT line links one registrar node into a MODULE-LOCAL list
// at DLL load; RegisterComponents drains that list into the module's own
// Astra::ComponentModule. This is what lets Assets -> Create -> C++ Class
// produce a component that is live after one Rebuild Game Module with no hand
// edit to Init -- the same effect UE gets from UHT's generated registration,
// without a header tool. (UE's IMPLEMENT_PRIMARY_GAME_MODULE is the eventual
// home for the Init boilerplate too; see the editor<->IDE surface notes.)
//
// Why components and not systems: a component's registration ORDER carries
// no meaning (ComponentIDs are a per-process counter, nothing persists them),
// so a static-initialisation-ordered list is exactly good enough. A system's
// order in its scheduler IS a design act (propagation before submission), and
// static-init order across TUs is unspecified -- so systems stay explicit in
// Init, with Astra::Before/After for the dependencies that matter.
//
// The ComponentModule contract (ComponentModule.hpp: "NEVER a plugin-side
// static/global object" whose destructor does live cleanup) is respected by
// construction: a registrar node is a struct of a function pointer, a C
// string and a next pointer -- trivially destructible -- and the list head is
// a plain pointer. DLL_PROCESS_DETACH runs no destructor of ours. The
// ComponentModule itself stays the heap-held handle Init owns and Shutdown
// deletes, exactly as HotReloadPlugin.cpp shows.
//
// Module-locality: RegistrarHead() is an inline function with a function-local
// static. Neither is exported, so every DLL that includes this header gets its
// OWN head -- a game module's list never sees the engine's, a plugin's never
// sees the game's. Header-only, no ABI surface.

#include <Astra/Component/ComponentModule.hpp>

#include <cstddef>

namespace Arcane::Game
{
    struct ComponentRegistrar
    {
        void (*registerFn)(Astra::ComponentModule&);
        const char*         typeName;   // the ARCANE_COMPONENT argument, stringified
        ComponentRegistrar* next;
    };

    namespace Detail
    {
        inline ComponentRegistrar*& RegistrarHead()
        {
            static ComponentRegistrar* head = nullptr;   // constant-initialised: no init-order hazard
            return head;
        }
    }

    // Link `node` at the head of this module's list. Called from the dynamic
    // initialiser ARCANE_COMPONENT plants; returns true only so it can sit in
    // a static bool's initialiser. Order among nodes is static-init order --
    // unspecified across TUs, and irrelevant (see the header comment).
    inline bool LinkComponentRegistrar(ComponentRegistrar& node)
    {
        node.next = Detail::RegistrarHead();
        Detail::RegistrarHead() = &node;
        return true;
    }

    // This module's registrars, most recently linked first. For diagnostics
    // ("module X brought N component(s)") and tests; RegisterComponents is
    // the consumer that matters.
    [[nodiscard]] inline const ComponentRegistrar* ComponentRegistrars()
    {
        return Detail::RegistrarHead();
    }

    // Drain the list into `module`: every ARCANE_COMPONENT type of THIS module
    // is registered through the module's own handle, so the descriptors and
    // meta are owned by (and torn down with) that handle exactly as a hand-
    // written Register<T>() would be. Returns how many were registered. Call
    // ONCE per Init; a second call would ask the module to register each type
    // again.
    inline std::size_t RegisterComponents(Astra::ComponentModule& module)
    {
        std::size_t n = 0;
        for (const ComponentRegistrar* r = Detail::RegistrarHead(); r; r = r->next)
        {
            r->registerFn(module);
            ++n;
        }
        return n;
    }
}

// Declare T (a reflected component type -- ASTRA_REFLECT_TYPE'd) as one of this
// module's components. Namespace scope, in exactly ONE .cpp per type: it
// defines statics, so a header would register T once per including TU. T may
// be namespace-qualified ("MyGame::Health") -- nothing here token-pastes it;
// uniqueness comes from __COUNTER__ inside an anonymous namespace.
#define ARCANE_COMPONENT(T) ARCANE_COMPONENT_IMPL_(T, __COUNTER__)
#define ARCANE_COMPONENT_IMPL_(T, N) ARCANE_COMPONENT_IMPL2_(T, N)
#define ARCANE_COMPONENT_IMPL2_(T, N)                                                        \
    namespace                                                                               \
    {                                                                                       \
        ::Arcane::Game::ComponentRegistrar arcaneComponentRegistrar_##N{                    \
            [](::Astra::ComponentModule& module) { module.Register<T>(); }, #T, nullptr };  \
        const bool arcaneComponentRegistrarLinked_##N =                                     \
            ::Arcane::Game::LinkComponentRegistrar(arcaneComponentRegistrar_##N);           \
    }

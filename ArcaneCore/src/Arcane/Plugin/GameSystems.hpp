#pragma once

// GameSystems: a game module's system-factory roster, self-registered.
//
//   // PlayerControllerSystem.cpp
//   #include "PlayerControllerSystem.hpp"
//   #include <Arcane/Plugin/GameSystems.hpp>
//   ARCANE_SYSTEM(MyGame::PlayerControllerSystem,
//                 Arcane::RoleMask::Client,
//                 Arcane::SystemPhase::Update)
//
// ARCANE_GAME_MODULE drains this module-local list while PluginHost's owner
// bracket is open. The result is still a factory per Runtime, never a static
// system instance: each world instantiates only the entries matching its role.
//
// Static initialization does not define semantic system order. Automatic
// systems must express dependencies with Arcane::Before/After traits; systems
// whose construction requires runtime values remain explicit RegisterSystem
// calls in GameModule::OnInit. Registrar nodes contain only pointers and enums,
// so unloading a DLL never runs a registrar destructor under the loader lock.
// The inline list head is private to each loaded module image.

#include <Arcane/Plugin/SystemFactory.hpp>

#include <Arcane/Ecs.hpp>   // Arcane::SystemScheduler + the parameter-system vocabulary

#include <Astra/Core/TypeID.hpp>

#include <cstddef>
#include <string>
#include <tuple>

namespace Arcane::Game
{
    struct SystemRegistrar
    {
        void (*registerFn)(SystemFactoryTable&, RoleMask, SystemPhase);
        const char*      typeName;
        RoleMask         mask;
        SystemPhase      phase;
        SystemRegistrar* next;
    };

    namespace Detail
    {
        inline SystemRegistrar*& SystemRegistrarHead()
        {
            static SystemRegistrar* head = nullptr;
            return head;
        }

        // Shared by automatic registration and GameModule::RegisterSystem so
        // both paths create byte-for-byte equivalent factory entries.
        template <class System, class... Args>
        void AddSystemFactory(SystemFactoryTable& table,
                              RoleMask mask,
                              SystemPhase phase,
                              Args... args)
        {
            table.Add(SystemFactoryEntry{
                // ARCANE_INTERNAL_BEGIN: the entry name is Astra's TypeID spelling of the system type
                std::string(Astra::TypeID<System>::Name()), mask, phase,
                // ARCANE_INTERNAL_END
                [args...](Arcane::SystemScheduler& scheduler)
                {
                    // Two system shapes (input-seam spec s5.2): a PARAMETER
                    // system (operator() over View&/Res/ResMut/Commands --
                    // the game-facing style) goes through Astra's param path,
                    // keyed by its own type and ordered by its SystemTraits;
                    // a registry-style system (operator()(Registry&) + traits)
                    // through the typed path, as before.
                    // ARCANE_INTERNAL_BEGIN: the shape test is Astra's own concept
                    if constexpr (Astra::ParamFunctor<System>)
                        std::ignore = scheduler.AddSystem(System{args...});
                    else
                        std::ignore = scheduler.AddSystem<System>(args...);
                    // ARCANE_INTERNAL_END
                },
                nullptr });
        }

        template <class System>
        void AddDefaultSystemFactory(SystemFactoryTable& table,
                                     RoleMask mask,
                                     SystemPhase phase)
        {
            AddSystemFactory<System>(table, mask, phase);
        }
    }

    inline bool LinkSystemRegistrar(SystemRegistrar& node)
    {
        node.next = Detail::SystemRegistrarHead();
        Detail::SystemRegistrarHead() = &node;
        return true;
    }

    [[nodiscard]] inline const SystemRegistrar* SystemRegistrars()
    {
        return Detail::SystemRegistrarHead();
    }

    // Return the accepted-entry delta rather than the number traversed: the
    // factory table may recoverably reject a duplicate type/phase declaration.
    inline std::size_t RegisterSystems(SystemFactoryTable& table)
    {
        const std::size_t before = table.Size();
        for (const SystemRegistrar* r = Detail::SystemRegistrarHead(); r; r = r->next)
            r->registerFn(table, r->mask, r->phase);
        return table.Size() - before;
    }
}

// Declare a default-constructible system in exactly one .cpp. A header would
// create one registrar per including translation unit and be rejected as a
// duplicate at module load. __COUNTER__ supplies an internal-linkage symbol;
// the system type itself may be namespace-qualified.
#define ARCANE_SYSTEM(T, Role, Phase) ARCANE_SYSTEM_IMPL_(T, Role, Phase, __COUNTER__)
#define ARCANE_SYSTEM_IMPL_(T, Role, Phase, N) ARCANE_SYSTEM_IMPL2_(T, Role, Phase, N)
#define ARCANE_SYSTEM_IMPL2_(T, Role, Phase, N)                                      \
    namespace                                                                        \
    {                                                                                \
        ::Arcane::Game::SystemRegistrar arcaneSystemRegistrar_##N{                   \
            &::Arcane::Game::Detail::AddDefaultSystemFactory<T>, #T, Role, Phase,   \
            nullptr };                                                               \
        const bool arcaneSystemRegistrarLinked_##N =                                 \
            ::Arcane::Game::LinkSystemRegistrar(arcaneSystemRegistrar_##N);          \
    }

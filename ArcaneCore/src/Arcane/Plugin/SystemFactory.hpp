#pragma once

// SystemFactory: the module contract's system-factory table (Core-DLL split, spec
// docs/specs/2026-09-15-core-dll-split-design.md s4).
//
// THE SHAPE (UE/DOTS): a game module registers its systems ONCE per DLL load --
// a type, an explicit role mask and a phase -- and then EVERY Runtime in the
// process instantiates the subset its own NetMode matches. A dedicated-server
// world takes the Server-masked systems, a client world the Client-masked ones,
// and a Standalone/ListenServer world both. That is what lets one loaded module
// serve N worlds (in-process PIE, an embedded server world beside the editor's
// client world) without the module knowing how many there are.
//
// WHERE IT LIVES: on the ProcessContext (spec s3 / plan 1 P8) -- Core-owned, one
// per process, outliving every module image. The `instantiate` std::function is
// compiled INTO the module, so PluginHost clears a module's entries by owner key
// BEFORE its image unmaps (PluginHost::TeardownImage).
//
// NET MODE IS NOT THE LAUNCH FLAG: ProcessContext::IsDedicatedServerProcess()
// describes how the PROCESS was launched; a system branches on its Runtime's
// NetMode. Conflating the two is UE's IsRunningDedicatedServer-vs-NetMode bug
// class, and RoleMaskTest.cpp pins them apart.

#include <Arcane/Core/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Astra { class SystemScheduler; }

namespace Arcane
{
    // The engine's per-phase scheduler trio (Arcane/Sim/SystemSchedulers.hpp).
    // Declared, not included: this header is on the game-module include surface
    // and reaches PluginABI.hpp, so it stays cheap.
    struct SystemSchedulers;

    enum class NetMode  : std::uint8_t { Standalone, DedicatedServer, ListenServer, Client };
    enum class RoleMask : std::uint8_t { Server = 1, Client = 2, Both = 3 };
    enum class SystemPhase : std::uint8_t { FixedUpdate, Update, Render };

    // RoleMask is a bitmask, so it needs the two operators RoleMatches is written
    // in terms of. `&` yields the raw bits rather than a RoleMask: the result of a
    // mask test is a truth value, not a role.
    [[nodiscard]] constexpr RoleMask operator|(RoleMask a, RoleMask b) noexcept
    {
        return static_cast<RoleMask>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
    }
    [[nodiscard]] constexpr std::uint8_t operator&(RoleMask a, RoleMask b) noexcept
    {
        return static_cast<std::uint8_t>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
    }

    // The roles a world in `m` actually plays. ListenServer is BOTH -- it is a
    // server that also presents -- which is exactly why the mask and the mode are
    // separate concepts.
    [[nodiscard]] constexpr RoleMask RolesOf(NetMode m) noexcept
    {
        switch (m)
        {
            case NetMode::Standalone:      return RoleMask::Both;
            case NetMode::DedicatedServer: return RoleMask::Server;
            case NetMode::ListenServer:    return RoleMask::Both;
            case NetMode::Client:          return RoleMask::Client;
        }
        return RoleMask::Both;
    }
    [[nodiscard]] constexpr bool RoleMatches(RoleMask mask, NetMode m) noexcept
    {
        return (mask & RolesOf(m)) != 0;
    }

    [[nodiscard]] ARCANE_CORE_API const char* ToString(NetMode m) noexcept;

    struct SystemFactoryEntry
    {
        std::string  name;      // the system type's name (log + census)
        RoleMask     mask  = RoleMask::Both;
        SystemPhase  phase = SystemPhase::FixedUpdate;
        std::function<void(Astra::SystemScheduler&)> instantiate;   // AddSystem<T>(args...) -- lives in the MODULE; cleared before unmap (P8)
        const void*  owner = nullptr;   // the registering image (PluginHost clears by owner)
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std::vector/std::string members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_CORE_API SystemFactoryTable
    {
    public:
        // Append one entry. The `owner` field is IGNORED while an owner is open
        // (BeginOwner below) -- the table stamps the open image instead, so a
        // module never has to name, or be able to name, its own image base.
        void Add(SystemFactoryEntry e);

        // Bracket a module image's Init. BeginOwner first CLEARS whatever that
        // owner registered before, so re-running an image's Init (a secondary
        // re-established across a primary hot reload) re-registers rather than
        // double-registers: an image's Init is its ONE registration point.
        void BeginOwner(const void* owner) noexcept;
        void EndOwner() noexcept;

        // Drop everything an image registered. PluginHost calls this while the
        // image is still MAPPED -- the std::functions are compiled into it.
        void ClearOwner(const void* owner) noexcept;

        [[nodiscard]] std::size_t Size() const noexcept;
        [[nodiscard]] std::span<const SystemFactoryEntry> Entries() const noexcept;

        // Runs every entry whose mask matches `mode` against the right phase scheduler; returns how many.
        std::size_t InstantiateInto(SystemSchedulers& into, NetMode mode) const;

    private:
        std::vector<SystemFactoryEntry> m_entries;
        const void*                     m_openOwner = nullptr;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

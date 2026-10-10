#pragma once

// Arcane::GameInput -- the READ-ONLY gameplay-input view a client world
// publishes as a registry resource (input-seam spec 2026-10-02 s4), and
// Arcane::ActionRef, a by-name action handle. Systems declare it as a param:
//
//     Arcane::ActionRef jump{"Player", "Jump"};             // a system member
//     void operator()(Arcane::ECS::Res<Arcane::GameInput> input) // input->PressedThisFixedStep(jump)
//
// Query only: maps, control schemes, rebinding and profiles stay on
// Client()->GameInput() (the LocalInputUser). Every client world carries one,
// even with no input asset configured (it then answers zero/false); a server
// world has none. Header-only: every call forwards to LocalInputUser's
// exported methods, so this adds no DLL surface.

#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/LocalInputUser.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace Arcane
{
    // A gameplay action named by map + action. Resolves lazily against the
    // GameInput that queries it and re-resolves whenever the input user's
    // Generation() moves (a hot-edited asset, a project switch). An action that
    // does not exist answers zero/false and warns ONCE per generation.
    class ActionRef
    {
    public:
        ActionRef(std::string_view map, std::string_view action) : m_map(map), m_action(action) {}

        [[nodiscard]] const std::string& Map() const noexcept    { return m_map; }
        [[nodiscard]] const std::string& Action() const noexcept { return m_action; }

    private:
        friend class GameInput;
        static constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();

        std::string m_map;
        std::string m_action;
        mutable std::optional<Guid> m_id;
        mutable std::uint64_t m_resolvedGeneration = kNever;
        mutable std::uint64_t m_warnedGeneration   = kNever;
    };

    class GameInput
    {
    public:
        // TRANSIENT (IN-8 ruling, spec s4 amendment): never written into a
        // registry snapshot. It holds a pointer to THIS process's client-side
        // LocalInputUser, so a world seeded from a client snapshot (the
        // EmbeddedServer) must not inherit it, and a restore must not revive a
        // stale copy; ClientRuntime republishes it every frame and fixed step.
        static constexpr bool AstraTransientResource = true;

        GameInput() = default;
        explicit GameInput(const LocalInputUser* user) noexcept : m_user(user) {}

        [[nodiscard]] bool HasUser() const noexcept { return m_user != nullptr; }
        [[nodiscard]] std::uint64_t Generation() const noexcept { return m_user ? m_user->Generation() : 0; }

        [[nodiscard]] std::optional<Guid> FindAction(std::string_view map, std::string_view action) const
        {
            return m_user ? m_user->FindAction(map, action) : std::nullopt;
        }

        [[nodiscard]] std::optional<Guid> Resolve(const ActionRef& ref) const
        {
            if (!m_user) return std::nullopt;
            const std::uint64_t gen = m_user->Generation();
            if (ref.m_resolvedGeneration != gen)
            {
                ref.m_id = m_user->FindAction(ref.m_map, ref.m_action);
                ref.m_resolvedGeneration = gen;
            }
            if (!ref.m_id && ref.m_warnedGeneration != gen)
            {
                ref.m_warnedGeneration = gen;
                ARC_WARN("input: action '{}.{}' is not in the selected gameplay input asset -- it reads as zero",
                         ref.m_map, ref.m_action);
            }
            return ref.m_id;
        }

        [[nodiscard]] InputActionValue Value(const ActionRef& r) const { const auto id = Resolve(r); return id ? Value(*id) : InputActionValue{}; }
        [[nodiscard]] bool Down(const ActionRef& r) const                  { const auto id = Resolve(r); return id && Down(*id); }
        [[nodiscard]] bool Pressed(const ActionRef& r) const               { const auto id = Resolve(r); return id && Pressed(*id); }
        [[nodiscard]] bool Released(const ActionRef& r) const              { const auto id = Resolve(r); return id && Released(*id); }
        [[nodiscard]] bool PressedThisFixedStep(const ActionRef& r) const  { const auto id = Resolve(r); return id && PressedThisFixedStep(*id); }
        [[nodiscard]] bool ReleasedThisFixedStep(const ActionRef& r) const { const auto id = Resolve(r); return id && ReleasedThisFixedStep(*id); }

        [[nodiscard]] InputActionValue Value(const Guid& id) const { return m_user ? m_user->Value(id) : InputActionValue{}; }
        [[nodiscard]] bool Down(const Guid& id) const                  { return m_user && m_user->Down(id); }
        [[nodiscard]] bool Pressed(const Guid& id) const               { return m_user && m_user->Pressed(id); }
        [[nodiscard]] bool Released(const Guid& id) const              { return m_user && m_user->Released(id); }
        [[nodiscard]] bool PressedThisFixedStep(const Guid& id) const  { return m_user && m_user->PressedThisFixedStep(id); }
        [[nodiscard]] bool ReleasedThisFixedStep(const Guid& id) const { return m_user && m_user->ReleasedThisFixedStep(id); }

    private:
        const LocalInputUser* m_user = nullptr;
    };
}

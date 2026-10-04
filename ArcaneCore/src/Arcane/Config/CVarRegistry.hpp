#pragma once

#include <Arcane/Config/CVarHandle.hpp>
#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Core/Api.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    enum class SetResult : std::uint8_t
    {
        Applied,          // accepted into the pending store; visible after Publish
        RefusedWeaker,    // a stronger SetBy already holds the value
        Stale,            // handle generation does not match
        TypeMismatch,
        Denied,           // permission / cheat gate
    };

    struct CVarDesc
    {
        std::string_view name;
        CVarType type = CVarType::Bool;
        CVarValue defaultValue = CVarValue::Bool(false);
        std::optional<CVarValue> min;
        std::optional<CVarValue> max;
        CVarFlags flags = CVarFlags::None;
        std::string_view help;
        std::string_view module;    // who declared it; unload drops the cvar
    };

    struct CVarHistoryRecord
    {
        SetBy by = SetBy::Default;
        CVarValue value = CVarValue::Bool(false);
        std::string module;         // who set it; empty for the constructor default
    };

    struct CVarExplain
    {
        std::string name;
        std::string help;
        CVarType type = CVarType::Bool;
        CVarFlags flags = CVarFlags::None;
        CVarValue published = CVarValue::Bool(false);
        CVarValue pending = CVarValue::Bool(false);
        SetBy setBy = SetBy::Default;
        std::vector<CVarHistoryRecord> history;   // oldest first; the back is current
    };

    struct CVarListEntry
    {
        std::string name;
        std::string help;
        CVarType type = CVarType::Bool;
        CVarFlags flags = CVarFlags::None;
    };

    struct ExecResult
    {
        bool ok = false;
        std::string text;
    };

    using CommandFn = void (*)(std::string_view args, std::string& out, void* user);

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)
#endif
    class ARC_CORE_API CVarRegistry
    {
    public:
        CVarRegistry();
        ~CVarRegistry();
        CVarRegistry(const CVarRegistry&) = delete;
        CVarRegistry& operator=(const CVarRegistry&) = delete;

        static CVarRegistry& Get();

        // Dev-flagged declarations are refused (stale handle, not stored) when
        // this registry was built with devCvars=false. Dist passes false.
        explicit CVarRegistry(bool devCvars);

        // Set when Register fails. Empty after a successful register.
        [[nodiscard]] const std::string& LastError() const;

        [[nodiscard]] CVarHandle Register(const CVarDesc& desc);
        [[nodiscard]] bool RegisterCommand(std::string name, CVarFlags flags, std::string help,
                                           std::string module, CommandFn fn, void* user);

        [[nodiscard]] CVarHandle Find(std::string_view name) const;
        [[nodiscard]] std::optional<CVarValue> Get(CVarHandle handle) const;

        // sourceModule tags the history record so UnregisterModule can pop it.
        SetResult Set(CVarHandle handle, CVarValue value, SetBy by,
                      std::string_view sourceModule = {},
                      Permission permission = Permission::Editor);

        void UnregisterModule(std::string_view module);

        // Snapshot readers. No-op when nothing is pending. Callbacks run after
        // the swap, on this thread, and a Set from a callback waits for the
        // next Publish.
        void Publish();

        // Fires on the publishing thread when the published value changes.
        // A Set from the callback is pending until the next Publish.
        using ChangeFn = void (*)(CVarHandle handle, void* user);
        void AddCallback(CVarHandle handle, ChangeFn fn, void* user);

        // Drop Console and Code history on every Cheat cvar and mark them
        // dirty. Config layers remain. Called when `cheats` publishes false.
        void RevertCheats();

        // Drop every history record of rung `by` on every cvar and mark the
        // touched ones dirty; the next Publish shows what the other rungs
        // hold. Runtime drops a closing project's User layer this way, so the
        // next project's archive cannot inherit it (T3-D2).
        void RevertLayer(SetBy by);

        [[nodiscard]] std::optional<CVarExplain> Explain(std::string_view name) const;
        [[nodiscard]] std::vector<CVarListEntry> List() const;   // skips Hidden; skips Dev if compiled out
        // Live commands, with List()'s rule (skips Hidden; skips Dev when
        // compiled out). `type` is meaningless for a command (left Bool).
        [[nodiscard]] std::vector<CVarListEntry> ListCommands() const;
        [[nodiscard]] ExecResult Execute(std::string_view line, Permission permission,
                                        SetBy by = SetBy::Console);

        // The published bool of the cvar named "cheats". False when absent.
        [[nodiscard]] bool CheatsEnabled() const;

    private:
        struct Slot;
        struct Command;
        struct Impl;
        Impl* m;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

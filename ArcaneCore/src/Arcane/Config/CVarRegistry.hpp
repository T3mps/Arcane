#pragma once

#include <Arcane/Config/CVarHandle.hpp>
#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Core/Api.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane
{
    enum class SetResult : std::uint8_t
    {
        Applied,          // accepted into the pending store; visible after Publish
        RefusedWeaker,    // recorded beneath a stronger SetBy, which still wins
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
        // Settings metadata (settings spec 2026-10-03 s4.2). Appended, so the
        // positional aggregate form above keeps compiling.
        std::string_view displayName;        // empty = derived from the last name segment
        std::string_view keywords;           // space-separated search words
        std::string_view widget;             // "", "asset:<kind>", "path:file", "path:dir", "keychord", "font", "slider"
        Audience audience = Audience::Game;
        SettingScope scope = SettingScope::Project;
        ApplyMode apply = ApplyMode::Live;
        std::int32_t order = 0;              // stable sort within a category
        std::string_view categoryPath;       // empty = derived from the dotted name ("Physics/Solver")
        std::vector<std::string> enumNames;  // an Enum's ordered names (required for one)
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

    // The full descriptor of one listed cvar (settings spec s8.1): what a game's
    // settings menu or the editor windows draw a row from. displayName and
    // categoryPath are already derived when the declaration left them empty.
    struct CVarListEntryEx
    {
        std::string name;
        std::string displayName;
        std::string help;
        std::string categoryPath;
        std::string widget;
        CVarType type = CVarType::Bool;
        CVarFlags flags = CVarFlags::None;
        Audience audience = Audience::Game;
        SettingScope scope = SettingScope::Project;
        ApplyMode apply = ApplyMode::Live;
        std::int32_t order = 0;
        std::optional<CVarValue> min;
        std::optional<CVarValue> max;
        std::vector<std::string> enumNames;
        CVarValue value = CVarValue::Bool(false);          // the published value
        CVarValue defaultValue = CVarValue::Bool(false);   // the Default rung's value
    };

    // Everything a declaration said, with the derived display strings filled
    // in (settings spec s4.2). What the settings windows and PlayerSettings read.
    struct CVarMetadata
    {
        std::string name;
        std::string help;
        std::string module;                  // the declaring module
        CVarType type = CVarType::Bool;
        CVarFlags flags = CVarFlags::None;   // as stored (UserSettable is derived from the audience)
        CVarValue defaultValue = CVarValue::Bool(false);   // after the range clamp
        std::optional<CVarValue> min;
        std::optional<CVarValue> max;
        std::string displayName;             // declared, else DeriveCVarDisplayName(name)
        std::string keywords;
        std::string widget;
        Audience audience = Audience::Game;
        SettingScope scope = SettingScope::Project;
        ApplyMode apply = ApplyMode::Live;
        std::int32_t order = 0;
        std::string categoryPath;            // declared, else DeriveCVarCategoryPath(name)
        std::vector<std::string> enumNames;
    };

    struct ExecResult
    {
        bool ok = false;
        std::string text;
    };

    // A command's reply (settings spec s4.5): it says whether it succeeded.
    struct CommandResult
    {
        bool        ok = false;
        std::string text;
    };
    using CommandFn = CommandResult (*)(std::string_view args, void* user);
    // The pre-S1 text-only form. It is still accepted, wrapped, and treated as ok.
    using LegacyCommandFn = void (*)(std::string_view args, std::string& out, void* user);

    // Who asked, for a game's policy and the audit sink (settings spec s3.2, s9).
    // `roles` is a game-defined bitmask; `gameData` is the game's own pointer.
    struct CVarCaller
    {
        std::string_view id;
        std::uint64_t    roles = 0;
        void*            gameData = nullptr;
    };

    enum class PolicyVerdict : std::uint8_t { Default, Allow, Deny };

    struct CVarInfo
    {
        std::string_view name;
        CVarType         type = CVarType::Bool;
        CVarFlags        flags = CVarFlags::None;
        Audience         audience = Audience::Game;
    };

    struct CVarRequest
    {
        CVarContext       context = CVarContext::Editor;
        const CVarCaller* caller = nullptr;
        bool              write = false;
    };

    // A game's access policy (settings spec s3.2). It is asked about every
    // non-Editor read and write; Default keeps the engine table. It cannot:
    //   - reach an Editor or Hidden setting;
    //   - make a Protected value readable outside ServerAdmin;
    //   - bring back a Dev setting that a Dist build compiled out.
    using CVarPolicyFn = PolicyVerdict (*)(const CVarInfo&, const CVarRequest&, void* user);

    struct CVarAuditRecord
    {
        std::string   name, callerId, oldValue, newValue;
        CVarContext   context = CVarContext::Editor;
        PolicyVerdict verdict = PolicyVerdict::Default;   // Default: the engine table decided
        std::int64_t  unixMs = 0;
    };
    using CVarAuditFn = void (*)(const CVarAuditRecord&, void* user);

    // The published values, immutable once built (settings spec s4.6). Publish
    // swaps a fresh one in atomically. A reader keeps the copy it loaded alive
    // for as long as it holds it, so worker threads never see a torn value.
    struct CVarSnapshot
    {
        struct Entry
        {
            std::uint32_t generation = 0;
            bool          alive = false;
            CVarValue     value = CVarValue::Bool(false);
        };
        std::uint64_t      serial = 0;    // +1 per swap
        std::vector<Entry> entries;       // indexed by CVarHandle::index

        [[nodiscard]] std::optional<CVarValue> Get(CVarHandle h) const
        {
            if (h.index >= entries.size()) return std::nullopt;
            const Entry& e = entries[h.index];
            if (!e.alive || e.generation != h.generation) return std::nullopt;
            return e.value;
        }
    };

    // One config rung, read from a directory of <category>.json files.
    struct CVarLayerDir
    {
        SetBy                 by = SetBy::EngineConfig;
        std::filesystem::path dir;
        std::string           sourceModule;
    };

    // Every config rung a host applies, weakest first, plus its --set list
    // (settings spec s4.4). ApplyLayersFor re-applies all of them to the cvars
    // of a module that (re)loaded, so it gets exactly the values a cold boot
    // would give it.
    struct LayerSources
    {
        std::vector<CVarLayerDir> dirs;
        std::vector<std::string>  commandLine;                      // "name=value", SetBy::CommandLine
        CVarContext               commandLineContext = CVarContext::Editor;
    };

    // RAII. While one is alive on this thread, cvar and command registrations
    // and callbacks are attributed to `module` (settings spec s4.3/s4.4). The
    // plugin host opens one around a module's load (its statics run inside
    // LoadLibrary, on this thread) and around its Init. Scopes nest; the
    // innermost one wins.
    class ARC_CORE_API CVarModuleScope
    {
    public:
        explicit CVarModuleScope(std::string_view module);
        ~CVarModuleScope();
        CVarModuleScope(const CVarModuleScope&) = delete;
        CVarModuleScope& operator=(const CVarModuleScope&) = delete;
    };

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

        // An empty desc.module is filled from CurrentModule() (settings spec s4.3).
        [[nodiscard]] CVarHandle Register(const CVarDesc& desc);
        [[nodiscard]] bool RegisterCommand(std::string name, CVarFlags flags, std::string help,
                                           std::string module, CommandFn fn, void* user);
        [[nodiscard]] bool RegisterCommand(std::string name, CVarFlags flags, std::string help,
                                           std::string module, LegacyCommandFn fn, void* user);

        [[nodiscard]] CVarHandle Find(std::string_view name) const;
        [[nodiscard]] bool IsCompiledOut(std::string_view name) const;   // a Dev cvar this registry refused (devCvars=false)
        // Renames (settings spec s4.7). `oldName` resolves to `newName` in
        // config files, --set and the console, with one warning per old name;
        // WriteCVarArchive writes only `newName` and drops the old key.
        // Refused (false) when either name is empty, they are equal, `oldName`
        // is a live cvar or command, `oldName` already aliases a DIFFERENT
        // name, or `newName` is itself an old name. The same pair again is a
        // success (a reloaded module re-runs its statics). Renaming a name
        // others alias to (b -> c while a -> b) re-points them: no chains.
        // Aliases are not module-scoped: an unload leaves them.
        bool RegisterAlias(std::string_view oldName, std::string_view newName);
        // Find, then the alias table, warning once per old name. For names a
        // PERSON wrote -- files, --set, the console. Code uses Find.
        [[nodiscard]] CVarHandle Resolve(std::string_view name);
        [[nodiscard]] std::string AliasTarget(std::string_view oldName) const;   // empty when not an alias
        [[nodiscard]] std::vector<std::pair<std::string, std::string>> Aliases() const;   // (old, new), by old
        [[nodiscard]] std::optional<CVarValue> Get(CVarHandle handle) const;
        // The declaration's metadata. nullopt for a stale handle.
        [[nodiscard]] std::optional<CVarMetadata> Metadata(CVarHandle handle) const;
        // May `ctx`, for `caller`, READ this setting: the Editor always; any
        // other context through the audience x context table, then the game's
        // policy (settings spec s3.2). False for a stale handle and for a
        // setting absent there (Editor or Hidden outside the editor). The
        // registry's own commands (cvar_explain) ask it, since a CommandFn
        // carries no caller: the Execute that dispatched them lends its own.
        [[nodiscard]] bool CanRead(CVarHandle handle, CVarContext ctx, const CVarCaller* caller = nullptr) const;

        // sourceModule tags the history record so UnregisterModule can pop it.
        // Outside the Editor context the write goes through the audience x
        // context table, then the game's policy (SetPolicy; settings spec
        // s3.2): Denied when they refuse. A Server setting's change, or its
        // policy denial, reaches the audit sink (SetAuditSink).
        SetResult Set(CVarHandle handle, CVarValue value, SetBy by,
                      std::string_view sourceModule = {},
                      CVarContext ctx = CVarContext::Editor,
                      const CVarCaller* caller = nullptr);

        void UnregisterModule(std::string_view module);

        // Re-apply every rung in `layers` (directories, then the --set items) to
        // the cvars `module` declared, and nothing else; then Publish. Defined
        // in CVarConfig.cpp beside the file reader.
        void ApplyLayersFor(std::string_view module, const LayerSources& layers);

        // The innermost CVarModuleScope on this thread, else empty.
        [[nodiscard]] static std::string_view ScopedModule() noexcept;
        // ScopedModule(), else ArcaneCore's own ARC_MODULE_NAME ("ArcaneCore").
        // Code in another module names ITSELF through Detail::CallerModule()
        // (CVarModule.hpp), which falls back to that module's own define.
        [[nodiscard]] static std::string_view CurrentModule() noexcept;
        // The module that declared the cvar; empty for a stale handle.
        [[nodiscard]] std::string ModuleOf(CVarHandle handle) const;

        // Snapshot readers. No-op when nothing is pending. Callbacks run after
        // the swap, on this thread, and a Set from a callback waits for the
        // next Publish.
        void Publish();

        // Wait-free for readers (any thread). The snapshot changes only at a
        // Publish that changed a value, and at Register/UnregisterModule.
        [[nodiscard]] std::shared_ptr<const CVarSnapshot> Snapshot() const;
        // Publish now, outside the frame boundary, for tools and tests. Main
        // thread only (the thread that constructed this registry); asserts.
        void PublishImmediate();

        // Fires on the publishing thread when the published value changes.
        // A Set from the callback is pending until the next Publish.
        using ChangeFn = void (*)(CVarHandle handle, void* user);
        void AddCallback(CVarHandle handle, ChangeFn fn, void* user);

        // Drop Console and Code history on every Cheat cvar and mark them
        // dirty. Config layers remain. Called when `server.cheats` publishes false.
        void RevertCheats();

        // Drop every history record of rung `by` on every cvar and mark the
        // touched ones dirty; the next Publish shows what the other rungs
        // hold. Runtime drops a closing project's User layer this way, so the
        // next project's archive cannot inherit it (T3-D2).
        void RevertLayer(SetBy by);

        // The game's policy: one per process. `module` is the module that
        // installed it, and UnregisterModule(module) clears it. A null fn clears it now.
        void SetPolicy(CVarPolicyFn fn, void* user, std::string_view module);
        // Receives every non-Editor change of a Server setting, and every
        // policy denial of one (settings spec s3.2, s9). A null fn uninstalls it.
        void SetAuditSink(CVarAuditFn fn, void* user);

        [[nodiscard]] std::optional<CVarExplain> Explain(std::string_view name) const;
        // Skips Hidden; skips Dev if compiled out. The Editor view (the
        // default) is everything else: what the archive writes and the settings
        // windows show. Any other context sees only what it may READ (settings
        // spec s3.2): no Editor-audience setting, no Protected one outside
        // ServerAdmin. cvarlist and the console's completion use the caller's.
        [[nodiscard]] std::vector<CVarListEntry> List(CVarContext ctx = CVarContext::Editor) const;
        // List()'s rule (skips Hidden; skips Dev when compiled out), with the
        // whole descriptor. Main thread: it reads the slots, not the snapshot.
        [[nodiscard]] std::vector<CVarListEntryEx> ListEx() const;
        // Live commands, with List()'s rule (skips Hidden; skips Dev when
        // compiled out). `type` is meaningless for a command (left Bool).
        [[nodiscard]] std::vector<CVarListEntry> ListCommands() const;
        [[nodiscard]] ExecResult Execute(std::string_view line, CVarContext ctx,
                                        SetBy by = SetBy::Console,
                                        const CVarCaller* caller = nullptr);

        // The published bool of server.cheats (alias "cheats"). False when absent.
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

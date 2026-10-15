#pragma once

// The settings windows' PURE edit core (settings arc S3, spec s3.3, s6.3,
// s6.4): which rung a row edits, whether it is overridden, the All projects
// / This project switch, and the undo commands every edit returns. An edit
// is applied when the verb runs (SetRung/RevertRung + Publish); the
// returned SettingEditCommand only reverses and replays it. No ImGui.

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Edit/Command.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    enum class SettingsWindowKind : std::uint8_t { Preferences, Project };
    enum class PrefMode : std::uint8_t { AllProjects, ThisProject };

    [[nodiscard]] const char* RungLabel(SetBy by) noexcept;
    // The history source a window edit records: the loaders' own names
    // (Runtime.cpp: "project", "user"), so an edit replaces the record its
    // file produced instead of stacking a second one.
    [[nodiscard]] std::string_view RungSource(SetBy by) noexcept;
    [[nodiscard]] std::string FormatSettingValue(const CVarValue& value, const std::vector<std::string>& enumNames);

    struct RowFacts
    {
        SetBy target = SetBy::Project;            // the rung an edit writes
        PrefMode mode = PrefMode::AllProjects;    // Preferences rows only
        SetBy winner = SetBy::Default;            // who holds the effective (pending) value
        bool overridden = false;                  // winner above target: read-only, Clear override
        bool modified = false;                    // effective != default
        bool projectOverride = false;             // Preferences: the User rung holds a value
        bool machineOnly = false;                 // Preferences: LaunchesProgram -- All projects only, no "This project" (S7-SEC)
        CVarValue effective = CVarValue::Bool(false);
        CVarValue defaultValue = CVarValue::Bool(false);
    };
    [[nodiscard]] RowFacts ComputeRowFacts(const CVarRegistry& registry, const CVarDescInfo& desc, SettingsWindowKind window);

    // One rung of one cvar, before and after (nullopt = the rung held no record).
    // `beforeSource` / `afterSource` are the history modules SetRung tags, so
    // undo restores the record UnregisterModule would have popped.
    struct RungChange
    {
        std::string name;
        SetBy rung = SetBy::Project;
        std::optional<CVarValue> before, after;
        std::string beforeSource;
        std::string afterSource;
        CVarHandle handle{};                  // registration at the edit; generation mismatch expires the step
    };
    // Told about every applied change (the archive's dirty set, S3-6).
    using SettingsEditSink = std::function<void(const RungChange&)>;

    // Put rung `rung` of `name` into `state` (a value = SetRung with `sourceModule`;
    // nullopt = RevertRung), then Publish(). The four-argument form tags the
    // record with RungSource(rung) -- the windows' own loaders.
    void ApplyRungState(CVarRegistry& registry, std::string_view name, SetBy rung, const std::optional<CVarValue>& state);
    void ApplyRungState(CVarRegistry& registry, std::string_view name, SetBy rung,
                        const std::optional<CVarValue>& state, std::string_view sourceModule);

    class SettingEditCommand final : public Arcane::ICommand
    {
    public:
        SettingEditCommand(CVarRegistry& registry, std::vector<RungChange> changes, std::string label, SettingsEditSink sink);
        void Undo() override;
        void Redo() override;
        const char* Label() const override { return m_label.c_str(); }
        bool AffectsScene() const override { return false; }   // settings are not scene state (spec s6.3)
        bool IsExpired() const override;                         // a changed cvar was unregistered or re-registered
        [[nodiscard]] const std::vector<RungChange>& Changes() const noexcept { return m_changes; }
    private:
        void Put(const RungChange& change, bool toBefore);
        CVarRegistry& m_registry;
        std::vector<RungChange> m_changes;
        std::string m_label;
        SettingsEditSink m_sink;
    };

    // Applies changes one by one, keeps the real ones, and hands back the step.
    class SettingsEditBuilder
    {
    public:
        SettingsEditBuilder(CVarRegistry& registry, SettingsEditSink sink) : m_registry(registry), m_sink(std::move(sink)) {}
        void Change(std::string_view name, SetBy rung, std::optional<CVarValue> after);
        [[nodiscard]] std::unique_ptr<SettingEditCommand> Finish(std::string label);   // null when nothing changed
    private:
        CVarRegistry& m_registry;
        SettingsEditSink m_sink;
        std::vector<RungChange> m_changes;
    };

    // The row verbs. Each applies its edit and returns the undo step, or null when nothing changed.
    [[nodiscard]] std::unique_ptr<SettingEditCommand> EditSetting(CVarRegistry& registry, const CVarDescInfo& desc,
        SettingsWindowKind window, CVarValue value, const SettingsEditSink& sink);
    [[nodiscard]] std::unique_ptr<SettingEditCommand> ResetSetting(CVarRegistry& registry, const CVarDescInfo& desc,
        SettingsWindowKind window, const SettingsEditSink& sink);
    [[nodiscard]] std::unique_ptr<SettingEditCommand> ClearOverride(CVarRegistry& registry, const CVarDescInfo& desc,
        SettingsWindowKind window, const SettingsEditSink& sink);
    [[nodiscard]] std::unique_ptr<SettingEditCommand> SwitchPrefMode(CVarRegistry& registry, const CVarDescInfo& desc,
        PrefMode to, const SettingsEditSink& sink);
}

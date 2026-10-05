#include "Settings/SettingsEdit.hpp"

#include "Settings/SettingsModel.hpp"   // SettingDisplayName

#include <Arcane/Config/CVarFormat.hpp>

#include <cstdio>

namespace Arcane::Editor
{
    namespace
    {
        // The newest record at or below `rung`: what the row shows under any override.
        std::optional<CVarValue> ValueAtOrBelow(const CVarRegistry& registry, std::string_view name, SetBy rung)
        {
            const std::optional<CVarExplain> e = registry.Explain(name);
            if (!e) return std::nullopt;
            for (auto it = e->history.rbegin(); it != e->history.rend(); ++it)
                if (it->by <= rung) return it->value;
            return std::nullopt;
        }

        // Colour "is default" is one 8-bit hex step, not bit-identity (S1-6).
        bool AtDefault(const CVarValue& v, const CVarValue& def)
        {
            if (v.type == CVarType::Color && def.type == CVarType::Color)
                return CVarColorNearlyEqual(v.AsColor(), def.AsColor());
            return v == def;
        }
    }

    const char* RungLabel(SetBy by) noexcept
    {
        switch (by)
        {
        case SetBy::Default:      return "Default";
        case SetBy::EngineConfig: return "Engine config";
        case SetBy::Plugin:       return "Plugin config";
        case SetBy::Project:      return "Project";
        case SetBy::EditorUser:   return "Editor (all projects)";
        case SetBy::User:         return "User (this project)";
        case SetBy::CommandLine:  return "Command line";
        case SetBy::Code:         return "Code";
        case SetBy::Console:      return "Console";
        }
        return "?";
    }

    std::string_view RungSource(SetBy by) noexcept
    {
        switch (by)
        {
        case SetBy::Project:    return "project";
        case SetBy::User:       return "user";
        case SetBy::EditorUser: return "editor-user";
        default:                return "settings";
        }
    }

    std::string FormatSettingValue(const CVarValue& v, const std::vector<std::string>& enumNames)
    {
        char buf[160];
        switch (v.type)
        {
        case CVarType::Bool:    return v.AsBool() ? "true" : "false";
        case CVarType::Int32:   return std::to_string(v.AsInt32());
        case CVarType::UInt32:  return std::to_string(v.AsUInt32());
        case CVarType::Int64:   return std::to_string(v.AsInt64());
        case CVarType::UInt64:  return std::to_string(v.AsUInt64());
        case CVarType::Float32: std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v.AsFloat32())); return buf;
        case CVarType::Float64: std::snprintf(buf, sizeof(buf), "%g", v.AsFloat64()); return buf;
        case CVarType::String:  return v.AsString();
        case CVarType::Color:
        {
            const CVarColor c = v.AsColor();
            std::snprintf(buf, sizeof(buf), "(%.3f, %.3f, %.3f, %.3f)", c.r, c.g, c.b, c.a);
            return buf;
        }
        case CVarType::Vec2: { const CVarVec2 x = v.AsVec2(); std::snprintf(buf, sizeof(buf), "(%g, %g)", x.x, x.y); return buf; }
        case CVarType::Vec3: { const CVarVec3 x = v.AsVec3(); std::snprintf(buf, sizeof(buf), "(%g, %g, %g)", x.x, x.y, x.z); return buf; }
        case CVarType::Vec4: { const CVarVec4 x = v.AsVec4(); std::snprintf(buf, sizeof(buf), "(%g, %g, %g, %g)", x.x, x.y, x.z, x.w); return buf; }
        case CVarType::Enum:
        {
            const std::int32_t i = v.AsEnum();
            if (i >= 0 && static_cast<std::size_t>(i) < enumNames.size()) return enumNames[static_cast<std::size_t>(i)];
            return std::to_string(i);
        }
        }
        return {};
    }

    RowFacts ComputeRowFacts(const CVarRegistry& registry, const CVarDescInfo& desc, SettingsWindowKind window)
    {
        RowFacts f;
        f.defaultValue = desc.defaultValue;
        const std::optional<CVarExplain> e = registry.Explain(desc.name);
        if (!e) return f;
        f.effective = e->pending;
        f.winner = e->setBy;
        f.modified = !AtDefault(f.effective, f.defaultValue);
        if (window == SettingsWindowKind::Project)
            f.target = SetBy::Project;
        else
        {
            const bool user = registry.RungValue(desc.name, SetBy::User).has_value();
            const bool machine = registry.RungValue(desc.name, SetBy::EditorUser).has_value();
            f.projectOverride = user;
            f.mode = user    ? PrefMode::ThisProject
                   : machine ? PrefMode::AllProjects
                   : desc.scope == SettingScope::PreferencesProject ? PrefMode::ThisProject : PrefMode::AllProjects;
            f.target = f.mode == PrefMode::ThisProject ? SetBy::User : SetBy::EditorUser;
        }
        f.overridden = f.winner > f.target;
        return f;
    }

    void ApplyRungState(CVarRegistry& registry, std::string_view name, SetBy rung, const std::optional<CVarValue>& state)
    {
        if (state) (void)registry.SetRung(name, rung, *state, RungSource(rung));
        else       (void)registry.RevertRung(name, rung);
        registry.Publish();
    }

    SettingEditCommand::SettingEditCommand(CVarRegistry& registry, std::vector<RungChange> changes, std::string label, SettingsEditSink sink)
        : m_registry(registry), m_changes(std::move(changes)), m_label(std::move(label)), m_sink(std::move(sink)) {}

    void SettingEditCommand::Put(const RungChange& c, bool toBefore)
    {
        const std::optional<CVarValue>& state = toBefore ? c.before : c.after;
        ApplyRungState(m_registry, c.name, c.rung, state);
        if (m_sink) m_sink(RungChange{ c.name, c.rung, toBefore ? c.after : c.before, state });
    }

    void SettingEditCommand::Undo()
    {
        for (auto it = m_changes.rbegin(); it != m_changes.rend(); ++it) Put(*it, true);
    }

    void SettingEditCommand::Redo()
    {
        for (const RungChange& c : m_changes) Put(c, false);
    }

    bool SettingEditCommand::IsExpired() const
    {
        for (const RungChange& c : m_changes)
            if (m_registry.Find(c.name).IsStale()) return true;
        return false;
    }

    void SettingsEditBuilder::Change(std::string_view name, SetBy rung, std::optional<CVarValue> after)
    {
        RungChange c{ std::string(name), rung, m_registry.RungValue(name, rung), std::move(after) };
        if (c.before == c.after) return;
        ApplyRungState(m_registry, c.name, c.rung, c.after);
        c.after = m_registry.RungValue(c.name, c.rung);   // SetRung clamps: record what landed
        if (c.before == c.after) return;
        if (m_sink) m_sink(c);
        m_changes.push_back(std::move(c));
    }

    std::unique_ptr<SettingEditCommand> SettingsEditBuilder::Finish(std::string label)
    {
        if (m_changes.empty()) return nullptr;
        return std::make_unique<SettingEditCommand>(m_registry, std::move(m_changes), std::move(label), m_sink);
    }

    std::unique_ptr<SettingEditCommand> EditSetting(CVarRegistry& registry, const CVarDescInfo& desc,
        SettingsWindowKind window, CVarValue value, const SettingsEditSink& sink)
    {
        const RowFacts f = ComputeRowFacts(registry, desc, window);
        if (f.overridden || value.type != desc.type) return nullptr;
        SettingsEditBuilder b(registry, sink);
        b.Change(desc.name, f.target, std::move(value));
        return b.Finish("Edit " + SettingDisplayName(desc));
    }

    std::unique_ptr<SettingEditCommand> ResetSetting(CVarRegistry& registry, const CVarDescInfo& desc,
        SettingsWindowKind window, const SettingsEditSink& sink)
    {
        const RowFacts f = ComputeRowFacts(registry, desc, window);
        if (f.overridden || !f.modified) return nullptr;
        SettingsEditBuilder b(registry, sink);
        b.Change(desc.name, f.target, std::nullopt);
        if (const std::optional<CVarExplain> now = registry.Explain(desc.name); now && !AtDefault(now->pending, desc.defaultValue))
            b.Change(desc.name, f.target, desc.defaultValue);   // a lower rung would show through: pin the default
        return b.Finish("Reset " + SettingDisplayName(desc));
    }

    std::unique_ptr<SettingEditCommand> ClearOverride(CVarRegistry& registry, const CVarDescInfo& desc,
        SettingsWindowKind window, const SettingsEditSink& sink)
    {
        const RowFacts f = ComputeRowFacts(registry, desc, window);
        if (!f.overridden) return nullptr;
        SettingsEditBuilder b(registry, sink);
        b.Change(desc.name, f.winner, std::nullopt);
        return b.Finish("Clear override on " + SettingDisplayName(desc));
    }

    std::unique_ptr<SettingEditCommand> SwitchPrefMode(CVarRegistry& registry, const CVarDescInfo& desc,
        PrefMode to, const SettingsEditSink& sink)
    {
        const RowFacts f = ComputeRowFacts(registry, desc, SettingsWindowKind::Preferences);
        if (f.mode == to) return nullptr;
        SettingsEditBuilder b(registry, sink);
        const std::optional<CVarValue> shown = ValueAtOrBelow(registry, desc.name, SetBy::User);
        if (to == PrefMode::ThisProject)
        {
            b.Change(desc.name, SetBy::User, shown);
            return b.Finish("Override " + SettingDisplayName(desc) + " for this project");
        }
        if (!registry.RungValue(desc.name, SetBy::EditorUser).has_value())
            b.Change(desc.name, SetBy::EditorUser, shown);   // nothing shared yet: this value becomes the shared one
        b.Change(desc.name, SetBy::User, std::nullopt);
        return b.Finish("Use " + SettingDisplayName(desc) + " for all projects");
    }
}

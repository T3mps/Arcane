#include "Settings/SettingsHost.hpp"

#include "Settings/SettingsApply.hpp"
#include "Settings/SettingsWindow.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Platform/Paths.hpp>

#include <chrono>
#include <filesystem>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    ARC_CVAR(cvar_settingsOpenAtBoot, "editor.settings.openAtBoot", std::string, std::string{},
             .flags = ::Arcane::CVarFlags::Dev | ::Arcane::CVarFlags::Hidden,
             .audience = ::Arcane::Audience::Editor,
             .scope = ::Arcane::SettingScope::PreferencesMachine,
             .help = "Automation: open a settings window at boot -- preferences, project or both (headless desk captures).");
    ARC_CVAR(cvar_settingsOpenCategory, "editor.settings.openCategory", std::string, std::string{},
             .flags = ::Arcane::CVarFlags::Dev | ::Arcane::CVarFlags::Hidden,
             .audience = ::Arcane::Audience::Editor,
             .scope = ::Arcane::SettingScope::PreferencesMachine,
             .help = "Automation: the category path (e.g. Editor/Undo) a settings window opened at boot selects.");

    namespace
    {
        struct PageEntry
        {
            SettingsPageRef ref;
            SettingsPageDrawFn fn = nullptr;
            void* user = nullptr;
        };

        struct Host
        {
            SettingsHostConfig config;
            SettingsWindowState prefs, project;
            SettingsArchiveQueue archive;
            SettingsApplyTracker tracker;
            std::vector<PageEntry> pages;
            bool baselined = false;
        };

        Host& TheHost()
        {
            static Host host;
            return host;
        }

        double Now()
        {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        SettingScope WindowScope(SettingsWindowKind kind)
        {
            return kind == SettingsWindowKind::Project ? SettingScope::Project : SettingScope::PreferencesMachine;
        }

        SettingsWindowState& StateFor(Host& h, SettingsWindowKind kind)
        {
            return kind == SettingsWindowKind::Project ? h.project : h.prefs;
        }

        std::filesystem::path RungDir(SetBy rung)
        {
            using Paths::Location;
            const bool project = TheHost().config.projectOpen;
            switch (rung)
            {
            case SetBy::Project:    return project ? Paths::Get(Location::ProjectConfig) : std::filesystem::path{};
            case SetBy::User:       return project ? Paths::Get(Location::GameUserDir) / "Config" : std::filesystem::path{};
            case SetBy::EditorUser: return Paths::Get(Location::EditorUserDir) / "Config";
            default:                return {};
            }
        }

        // S3-6: RungWriter returns bool (true = persisted). WriteCVarRungArchive
        // is void, so a completed call (including "no folder -- session only")
        // reports success; a failed write would otherwise retry every debounce.
        bool WriteRung(SetBy rung, const std::vector<std::string>& names)
        {
            const std::filesystem::path dir = RungDir(rung);
            if (dir.empty())
            {
                ARC_WARN("settings: no folder for the {} rung (no project open) -- {} edit(s) kept for this session only",
                         RungLabel(rung), names.size());
                return true;
            }
            WriteCVarRungArchive(CVarRegistry::Get(), rung, dir, names);
            return true;
        }

        SettingsWindowEnv EnvFor(Host& h, SettingsWindowKind kind)
        {
            SettingsWindowEnv env;
            env.registry = &CVarRegistry::Get();
            env.kind = kind;
            env.title = kind == SettingsWindowKind::Project ? "Project Settings" : "Editor Preferences";
            env.roles = h.config.roles;
            for (const PageEntry& e : h.pages)
                if (InWindow(e.ref.scope, WindowScope(kind))) env.pages.push_back(e.ref);
            env.drawPage = [&h, kind](const std::string& path)
            {
                for (const PageEntry& e : h.pages)
                    if (e.fn && e.ref.categoryPath == path && InWindow(e.ref.scope, WindowScope(kind)))
                        e.fn(e.user);
            };
            env.now = &Now;
            env.archive = &h.archive;
            env.writeRung = &WriteRung;
            env.tracker = &h.tracker;
            env.restartEditor = h.config.restartEditor;
            env.restartBlockedReason = h.config.restartBlockedReason ? h.config.restartBlockedReason() : std::string();
            env.assetRefs = h.config.assetRefs;
            env.browsePath = h.config.browsePath;
            env.projectOpen = h.config.projectOpen;
            return env;
        }
    }

    void ConfigureSettingsHost(SettingsHostConfig config)
    {
        Host& h = TheHost();
        h.config = std::move(config);
        h.prefs.model = SettingsModel{};     // the module roles changed: rebuild both trees
        h.project.model = SettingsModel{};
        if (!h.baselined)
        {
            h.tracker.Observe(CVarRegistry::Get());   // boot: --set and every config rung already applied
            h.baselined = true;
        }
    }

    void SettingsHostProjectClosing()
    {
        Host& h = TheHost();
        h.archive.Flush(&WriteRung);   // into the OUTGOING project's folders
        for (SettingsWindowState* st : { &h.prefs, &h.project })
        {
            SettingsUndo(*st).Clear("Project switched");
            st->textDrafts.clear();
        }
    }

    void SettingsHostOnProjectSwitch(ProjectSwitchPreTeardown verdict)
    {
        CloseSettingsHostIfProjectSwitchAccepted(verdict == ProjectSwitchPreTeardown::Accepted);
    }

    void CloseSettingsHostIfProjectSwitchAccepted(bool accepted)
    {
        if (accepted)
            SettingsHostProjectClosing();
    }

    bool SettingsHostArchivePending()
    {
        return TheHost().archive.Dirty();
    }

    void TickSettingsHost()
    {
        TheHost().archive.Tick(Now(), cvar_settingsSaveDebounceMs.Get(), &WriteRung);
    }

    void FlushSettingsArchives()
    {
        TheHost().archive.Flush(&WriteRung);
    }

    void SettingsWorldCreated()
    {
        TheHost().tracker.WorldCreated(CVarRegistry::Get());
    }

    bool SettingsWindowFocused()
    {
        const Host& h = TheHost();
        return h.prefs.focused || h.project.focused;
    }

    void SelectSettingsCategory(SettingsWindowKind kind, std::string path)
    {
        StateFor(TheHost(), kind).selected = std::move(path);
    }

    std::string SelectedSettingsCategory(SettingsWindowKind kind)
    {
        return StateFor(TheHost(), kind).selected;
    }

    SettingsBootOpen ConsumeSettingsOpenAtBoot()
    {
        SettingsBootOpen out;
        const std::string which = cvar_settingsOpenAtBoot.Get();
        out.preferences = which == "preferences" || which == "both";
        out.project = which == "project" || which == "both";
        const std::string category = cvar_settingsOpenCategory.Get();
        if (!category.empty())
        {
            if (out.preferences) SelectSettingsCategory(SettingsWindowKind::Preferences, category);
            if (out.project) SelectSettingsCategory(SettingsWindowKind::Project, category);
        }
        return out;
    }

    void ApplySettingsPathPick(const std::string& cvar, const std::string& path)
    {
        Host& h = TheHost();
        CVarRegistry& reg = CVarRegistry::Get();
        const std::optional<CVarDescInfo> d = reg.Describe(cvar);
        if (!d || path.empty()) return;
        const SettingsWindowKind kind = d->scope == SettingScope::Project ? SettingsWindowKind::Project
                                                                          : SettingsWindowKind::Preferences;
        if (std::unique_ptr<SettingEditCommand> step = EditSetting(reg, *d, kind, CVarValue::String(path), ArchiveSink(&h.archive, &Now)))
            SettingsUndo(StateFor(h, kind)).Push(std::move(step));
    }

    std::uint64_t BeginSettingsPathBrowse(std::string& cvarSlot, DialogSlot<std::string>& slot, const std::string& cvar)
    {
        cvarSlot = cvar;
        return slot.Arm();
    }

    void ConsumeSettingsPathPick(std::string& cvarSlot, DialogSlot<std::string>& slot)
    {
        if (const auto picked = slot.Take())
            ApplySettingsPathPick(cvarSlot, *picked);
    }

    void RegisterSettingsPage(SettingScope scope, std::string categoryPath, std::string title, SettingsPageDrawFn fn, void* user)
    {
        Host& h = TheHost();
        const bool prefs = scope != SettingScope::Project;
        for (PageEntry& e : h.pages)
            if ((e.ref.scope != SettingScope::Project) == prefs && e.ref.categoryPath == categoryPath)
            {
                e.ref.title = std::move(title);
                e.fn = fn;
                e.user = user;
                return;
            }
        h.pages.push_back(PageEntry{ SettingsPageRef{ scope, std::move(categoryPath), std::move(title) }, fn, user });
    }

    void DrawEditorPreferences(bool* open)
    {
        Host& h = TheHost();
        DrawSettingsWindow(h.prefs, EnvFor(h, SettingsWindowKind::Preferences), open);
    }

    void DrawProjectSettings(bool* open)
    {
        Host& h = TheHost();
        DrawSettingsWindow(h.project, EnvFor(h, SettingsWindowKind::Project), open);
    }
}

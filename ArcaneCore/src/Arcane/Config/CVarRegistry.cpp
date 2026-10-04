#include <Arcane/Config/CVarRegistry.hpp>

#include <Arcane/Config/CVarFormat.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarRef.hpp>

#include <sstream>
#include <unordered_map>
#include <utility>

namespace Arcane
{
    namespace
    {
        const char* SetByName(SetBy by)
        {
            switch (by)
            {
            case SetBy::Default: return "Default";
            case SetBy::EngineConfig: return "EngineConfig";
            case SetBy::Plugin: return "Plugin";
            case SetBy::Project: return "Project";
            case SetBy::User: return "User";
            case SetBy::CommandLine: return "CommandLine";
            case SetBy::Code: return "Code";
            case SetBy::Console: return "Console";
            }
            return "?";
        }

        bool TakesRange(CVarType t)
        {
            return (t >= CVarType::Int32 && t <= CVarType::Float64) || (t >= CVarType::Color && t <= CVarType::Vec4);
        }

        // True when some component of `lo` exceeds `hi` (same type, checked by the caller).
        bool RangeEmpty(const CVarValue& lo, const CVarValue& hi)
        {
            switch (lo.type)
            {
            case CVarType::Int32: return lo.AsInt32() > hi.AsInt32();
            case CVarType::UInt32: return lo.AsUInt32() > hi.AsUInt32();
            case CVarType::Int64: return lo.AsInt64() > hi.AsInt64();
            case CVarType::UInt64: return lo.AsUInt64() > hi.AsUInt64();
            case CVarType::Float32: return lo.AsFloat32() > hi.AsFloat32();
            case CVarType::Float64: return lo.AsFloat64() > hi.AsFloat64();
            case CVarType::Color:
            {
                const CVarColor a = lo.AsColor();
                const CVarColor b = hi.AsColor();
                return a.r > b.r || a.g > b.g || a.b > b.b || a.a > b.a;
            }
            case CVarType::Vec2: { const CVarVec2 a = lo.AsVec2(); const CVarVec2 b = hi.AsVec2(); return a.x > b.x || a.y > b.y; }
            case CVarType::Vec3: { const CVarVec3 a = lo.AsVec3(); const CVarVec3 b = hi.AsVec3(); return a.x > b.x || a.y > b.y || a.z > b.z; }
            case CVarType::Vec4:
            {
                const CVarVec4 a = lo.AsVec4();
                const CVarVec4 b = hi.AsVec4();
                return a.x > b.x || a.y > b.y || a.z > b.z || a.w > b.w;
            }
            default: return false;
            }
        }

        // settings spec s4.1: the hint a String (or, for "slider", a number)
        // carries to the settings window.
        bool WidgetFits(std::string_view widget, CVarType type)
        {
            if (widget.empty()) return true;
            if (widget == "slider") return type >= CVarType::Int32 && type <= CVarType::Float64;
            if (widget == "path:file" || widget == "path:dir" || widget == "keychord" || widget == "font")
                return type == CVarType::String;
            if (widget.starts_with("asset:")) return widget.size() > 6 && type == CVarType::String;
            return false;
        }

        void ListCommand(std::string_view, std::string& out, void* user);
        void ExplainCommand(std::string_view args, std::string& out, void* user);
    }

    struct CVarRegistry::Slot
    {
        std::uint32_t generation = 1;
        bool alive = false;
        CVarType type = CVarType::Bool;
        CVarFlags flags = CVarFlags::None;
        std::string name;
        std::string help;
        std::string declaredBy;
        std::optional<CVarValue> min;
        std::optional<CVarValue> max;
        CVarValue defaultValue = CVarValue::Bool(false);
        std::string displayName;
        std::string keywords;
        std::string widget;
        Audience audience = Audience::Game;
        SettingScope scope = SettingScope::Project;
        ApplyMode apply = ApplyMode::Live;
        std::int32_t order = 0;
        std::string categoryPath;
        std::vector<std::string> enumNames;
        CVarValue published = CVarValue::Bool(false);
        std::vector<CVarHistoryRecord> history;
        bool dirty = false;
        struct Callback { ChangeFn fn = nullptr; void* user = nullptr; };
        std::vector<Callback> callbacks;
    };

    struct CVarRegistry::Command
    {
        std::string name;
        std::string help;
        std::string module;
        CVarFlags flags = CVarFlags::None;
        CommandFn fn = nullptr;
        void* user = nullptr;
        bool alive = true;
    };

    struct CVarRegistry::Impl
    {
        bool devCvars = true;
        std::vector<Slot> slots;
        std::vector<std::uint32_t> freeSlots;
        std::unordered_map<std::string, std::uint32_t> byName;
        std::vector<Command> commands;
        std::unordered_map<std::string, std::uint32_t> commandByName;
        bool publishing = false;
        std::string lastError;
    };

    CVarRegistry::CVarRegistry() : CVarRegistry(true) {}

    CVarRegistry::CVarRegistry(bool devCvars) : m(new Impl)
    {
        m->devCvars = devCvars;
        const bool listed = RegisterCommand("cvarlist", CVarFlags::None, "List registered cvars.", "engine", &ListCommand, this);
        const bool explained = RegisterCommand("cvar_explain", CVarFlags::None, "Show who set a cvar and the history under it.", "engine",
                                               &ExplainCommand, this);
        (void)listed;
        (void)explained;
        // node-page phase s8.2: the command line's history depth. Registered on
        // EVERY registry (test registries included), so through CVarDesc rather
        // than ARC_CVAR (which targets Get() only). ConsoleModel reads it.
        const CVarHandle history = Register(CVarDesc{ "console.historySize", CVarType::Int32, CVarValue::Int32(64),
                                                      CVarValue::Int32(1), CVarValue::Int32(1024), CVarFlags::Archive,
                                                      "Command-line history depth.", "engine" });
        (void)history;
    }

    CVarRegistry::~CVarRegistry() { delete m; }

    const std::string& CVarRegistry::LastError() const { return m->lastError; }

    CVarRegistry& CVarRegistry::Get()
    {
#if defined(ARC_BUILD_DIST)
        static CVarRegistry registry{ false };
#else
        static CVarRegistry registry{ true };
#endif
        return registry;
    }

    namespace
    {
        void ListCommand(std::string_view, std::string& out, void* user)
        {
            auto* self = static_cast<CVarRegistry*>(user);
            for (const CVarListEntry& e : self->List())
            {
                out += e.name;
                out += " (";
                out += CVarTypeName(e.type);
                out += ") ";
                out += e.help;
                out += '\n';
            }
        }

        void ExplainCommand(std::string_view args, std::string& out, void* user)
        {
            auto* self = static_cast<CVarRegistry*>(user);
            while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
            const auto explained = self->Explain(args);
            if (!explained)
            {
                out = "unknown cvar '";
                out += args;
                out += "'";
                return;
            }
            std::vector<std::string> enumNames;
            if (explained->type == CVarType::Enum)
                if (const auto meta = self->Metadata(self->Find(explained->name))) enumNames = meta->enumNames;
            out += explained->name;
            out += " = ";
            out += FormatCVarValue(explained->published, enumNames);
            out += " [";
            out += SetByName(explained->setBy);
            out += "]\n";
            for (const CVarHistoryRecord& h : explained->history)
            {
                out += "  ";
                out += SetByName(h.by);
                if (!h.module.empty()) { out += " ("; out += h.module; out += ")"; }
                out += " = ";
                out += FormatCVarValue(h.value, enumNames);
                out += '\n';
            }
        }
    }

    CVarHandle CVarRegistry::Register(const CVarDesc& desc)
    {
        m->lastError.clear();
        const std::string name{ desc.name };
        const auto refuse = [&](const std::string& why) {
            m->lastError = "cvar '" + name + "' " + why;
            return CVarHandle{};
        };
        if (name.empty())
        {
            m->lastError = "a cvar needs a name";
            return {};
        }
        if (desc.type > CVarType::Enum)
            return refuse("has an unknown type");
        // settings spec s4.2: every row shows its help as the tooltip.
        if (desc.help.empty() && !HasFlag(desc.flags, CVarFlags::Hidden))
            return refuse("has no help text (every setting that is not Hidden needs one)");
        // O4: a default of another type would publish a value its readers cannot take.
        if (desc.defaultValue.type != desc.type)
            return refuse(std::string("has a ") + CVarTypeName(desc.defaultValue.type) + " default but is declared " +
                          CVarTypeName(desc.type));
        if (desc.min || desc.max)
        {
            if (!TakesRange(desc.type))
                return refuse(std::string("is ") + CVarTypeName(desc.type) + ", which takes no min/max");
            for (const std::optional<CVarValue>* bound : { &desc.min, &desc.max })
                if (*bound && (*bound)->type != desc.type)
                    return refuse(std::string("has a ") + CVarTypeName((*bound)->type) + " bound but is declared " +
                                  CVarTypeName(desc.type));
            if (desc.min && desc.max && RangeEmpty(*desc.min, *desc.max))
                return refuse("has an empty range (min > max)");
        }
        if (desc.type == CVarType::Enum)
        {
            if (desc.enumNames.empty())
                return refuse("is an Enum with no names (a reflected enum needs ARC_REFLECT_ENUM, not FLAGS, "
                              "visible ahead of its ARC_CVAR)");
            for (std::size_t i = 0; i < desc.enumNames.size(); ++i)
            {
                if (desc.enumNames[i].empty())
                    return refuse("has an empty Enum name");
                for (std::size_t j = 0; j < i; ++j)
                    if (desc.enumNames[j] == desc.enumNames[i])
                        return refuse("names the Enum value '" + desc.enumNames[i] + "' twice");
            }
            const std::int32_t ordinal = desc.defaultValue.AsEnum();
            if (ordinal < 0 || static_cast<std::size_t>(ordinal) >= desc.enumNames.size())
                return refuse("has an Enum default outside its " + std::to_string(desc.enumNames.size()) + " names");
        }
        else if (!desc.enumNames.empty())
            return refuse(std::string("declares Enum names but is ") + CVarTypeName(desc.type));
        if (!WidgetFits(desc.widget, desc.type))
            return refuse("has widget '" + std::string(desc.widget) + "', which does not fit its type (" +
                          CVarTypeName(desc.type) + ")");
        if (HasFlag(desc.flags, CVarFlags::Dev) && !m->devCvars)
            return refuse("is Dev and this build compiled it out");
        if (const auto existing = m->byName.find(name); existing != m->byName.end())
            return refuse("already registered by module '" + m->slots[existing->second].declaredBy +
                          "', refused from '" + std::string(desc.module) + "'");
        if (m->commandByName.contains(name))
            return refuse("collides with a command");

        std::uint32_t index = 0;
        if (!m->freeSlots.empty())
        {
            index = m->freeSlots.back();
            m->freeSlots.pop_back();
        }
        else
        {
            index = static_cast<std::uint32_t>(m->slots.size());
            m->slots.emplace_back();
        }

        Slot& slot = m->slots[index];
        const std::uint32_t generation = slot.generation == 0 ? 1 : slot.generation;
        slot = Slot{};
        slot.generation = generation;
        slot.alive = true;
        slot.type = desc.type;
        slot.flags = desc.flags;
        if (HasFlag(slot.flags, CVarFlags::Archive)) slot.flags = slot.flags | CVarFlags::UserSettable;
        slot.name = desc.name;
        slot.help = desc.help;
        slot.declaredBy = desc.module;
        slot.min = desc.min;
        slot.max = desc.max;
        slot.published = Clamp(desc.defaultValue, slot.min, slot.max);
        slot.defaultValue = slot.published;
        slot.displayName = desc.displayName.empty() ? DeriveCVarDisplayName(desc.name) : std::string(desc.displayName);
        slot.keywords = desc.keywords;
        slot.widget = desc.widget;
        slot.audience = desc.audience;
        slot.scope = desc.scope;
        slot.apply = desc.apply;
        slot.order = desc.order;
        slot.categoryPath = desc.categoryPath.empty() ? DeriveCVarCategoryPath(desc.name) : std::string(desc.categoryPath);
        slot.enumNames = desc.enumNames;
        slot.history.push_back(CVarHistoryRecord{ SetBy::Default, slot.published, {} });
        slot.dirty = false;
        m->byName.emplace(slot.name, index);
        return CVarHandle{ index, generation };
    }

    bool CVarRegistry::RegisterCommand(std::string name, CVarFlags flags, std::string help,
                                       std::string module, CommandFn fn, void* user)
    {
        if (name.empty() || !fn) return false;
        if (m->byName.contains(name) || m->commandByName.contains(name)) return false;
        if (HasFlag(flags, CVarFlags::Dev) && !m->devCvars) return false;
        const std::uint32_t index = static_cast<std::uint32_t>(m->commands.size());
        m->commands.push_back(Command{ std::move(name), std::move(help), std::move(module), flags, fn, user, true });
        m->commandByName.emplace(m->commands.back().name, index);
        return true;
    }

    CVarHandle CVarRegistry::Find(std::string_view name) const
    {
        const auto it = m->byName.find(std::string(name));
        if (it == m->byName.end()) return {};
        const Slot& slot = m->slots[it->second];
        if (!slot.alive) return {};
        return CVarHandle{ it->second, slot.generation };
    }

    std::optional<CVarValue> CVarRegistry::Get(CVarHandle handle) const
    {
        if (handle.index >= m->slots.size()) return std::nullopt;
        const Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation) return std::nullopt;
        return slot.published;
    }

    std::optional<CVarMetadata> CVarRegistry::Metadata(CVarHandle handle) const
    {
        if (handle.index >= m->slots.size()) return std::nullopt;
        const Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation) return std::nullopt;
        CVarMetadata out;
        out.name = slot.name;
        out.help = slot.help;
        out.module = slot.declaredBy;
        out.type = slot.type;
        out.flags = slot.flags;
        out.defaultValue = slot.defaultValue;
        out.min = slot.min;
        out.max = slot.max;
        out.displayName = slot.displayName;
        out.keywords = slot.keywords;
        out.widget = slot.widget;
        out.audience = slot.audience;
        out.scope = slot.scope;
        out.apply = slot.apply;
        out.order = slot.order;
        out.categoryPath = slot.categoryPath;
        out.enumNames = slot.enumNames;
        return out;
    }

    SetResult CVarRegistry::Set(CVarHandle handle, CVarValue value, SetBy by,
                                std::string_view sourceModule, Permission permission)
    {
        if (handle.index >= m->slots.size()) return SetResult::Stale;
        Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation) return SetResult::Stale;
        if (value.type != slot.type) return SetResult::TypeMismatch;
        if (slot.type == CVarType::Enum &&
            (value.AsEnum() < 0 || static_cast<std::size_t>(value.AsEnum()) >= slot.enumNames.size()))
            return SetResult::TypeMismatch;   // an ordinal outside the declared names

        const bool editor = permission == Permission::Editor;
        if (!editor)
        {
            if (!HasFlag(slot.flags, CVarFlags::UserSettable)) return SetResult::Denied;
            if (HasFlag(slot.flags, CVarFlags::Cheat) && !CheatsEnabled()) return SetResult::Denied;
        }

        const SetBy winner = slot.history.empty() ? SetBy::Default : slot.history.back().by;
        if (by < winner) return SetResult::RefusedWeaker;

        value = Clamp(std::move(value), slot.min, slot.max);
        slot.history.push_back(CVarHistoryRecord{ by, value, std::string(sourceModule) });
        slot.dirty = true;
        if (!m->publishing) { /* stays pending until Publish */ }
        return SetResult::Applied;
    }

    void CVarRegistry::UnregisterModule(std::string_view module)
    {
        std::vector<std::uint32_t> kill;
        for (std::uint32_t i = 0; i < m->slots.size(); ++i)
        {
            Slot& slot = m->slots[i];
            if (!slot.alive) continue;
            if (slot.declaredBy == module)
            {
                kill.push_back(i);
                continue;
            }
            const auto before = slot.history.size();
            std::erase_if(slot.history, [&](const CVarHistoryRecord& h) { return h.module == module; });
            if (slot.history.size() != before) slot.dirty = true;
            if (slot.history.empty())
            {
                slot.history.push_back(CVarHistoryRecord{ SetBy::Default, slot.published, {} });
            }
        }
        for (std::uint32_t index : kill)
        {
            Slot& slot = m->slots[index];
            m->byName.erase(slot.name);
            const std::uint32_t nextGen = slot.generation + 1;
            slot = Slot{};
            slot.generation = nextGen == 0 ? 1 : nextGen;
            slot.alive = false;
            m->freeSlots.push_back(index);
        }
        for (Command& command : m->commands)
        {
            if (command.alive && command.module == module && command.module != "engine")
            {
                m->commandByName.erase(command.name);
                command.alive = false;
            }
        }
    }

    void CVarRegistry::RevertCheats()
    {
        for (Slot& slot : m->slots)
        {
            if (!slot.alive || !HasFlag(slot.flags, CVarFlags::Cheat)) continue;
            const auto before = slot.history.size();
            std::erase_if(slot.history, [](const CVarHistoryRecord& h) {
                return h.by == SetBy::Console || h.by == SetBy::Code;
            });
            if (slot.history.size() != before) slot.dirty = true;
            if (slot.history.empty())
                slot.history.push_back(CVarHistoryRecord{ SetBy::Default, slot.published, {} });
        }
    }

    void CVarRegistry::RevertLayer(SetBy by)
    {
        for (Slot& slot : m->slots)
        {
            if (!slot.alive) continue;
            const auto before = slot.history.size();
            std::erase_if(slot.history, [by](const CVarHistoryRecord& h) { return h.by == by; });
            if (slot.history.size() != before) slot.dirty = true;
            if (slot.history.empty())
                slot.history.push_back(CVarHistoryRecord{ SetBy::Default, slot.published, {} });
        }
    }

    void CVarRegistry::Publish()
    {
        if (m->publishing) return;
        m->publishing = true;
        const bool cheatsWere = CheatsEnabled();
        std::vector<std::uint32_t> changed;
        for (std::uint32_t i = 0; i < m->slots.size(); ++i)
        {
            Slot& slot = m->slots[i];
            if (!slot.alive || !slot.dirty) continue;
            const CVarValue next = slot.history.back().value;
            slot.dirty = false;
            if (!(next == slot.published))
            {
                slot.published = next;
                changed.push_back(i);
            }
        }
        for (std::uint32_t index : changed)
        {
            Slot& slot = m->slots[index];
            for (const Slot::Callback& cb : slot.callbacks)
                if (cb.fn) cb.fn(CVarHandle{ index, slot.generation }, cb.user);
        }
        if (cheatsWere && !CheatsEnabled())
        {
            RevertCheats();
            for (Slot& slot : m->slots)
            {
                if (!slot.alive || !slot.dirty) continue;
                slot.published = slot.history.back().value;
                slot.dirty = false;
            }
        }
        m->publishing = false;
    }

    std::optional<CVarExplain> CVarRegistry::Explain(std::string_view name) const
    {
        const CVarHandle handle = Find(name);
        if (handle.IsStale()) return std::nullopt;
        const Slot& slot = m->slots[handle.index];
        CVarExplain out;
        out.name = slot.name;
        out.help = slot.help;
        out.type = slot.type;
        out.flags = slot.flags;
        out.published = slot.published;
        out.pending = slot.history.back().value;
        out.setBy = slot.history.back().by;
        out.history = slot.history;
        return out;
    }

    std::vector<CVarListEntry> CVarRegistry::List() const
    {
        std::vector<CVarListEntry> out;
        for (const Slot& slot : m->slots)
        {
            if (!slot.alive) continue;
            if (HasFlag(slot.flags, CVarFlags::Hidden)) continue;
            if (HasFlag(slot.flags, CVarFlags::Dev) && !m->devCvars) continue;
            out.push_back(CVarListEntry{ slot.name, slot.help, slot.type, slot.flags });
        }
        return out;
    }

    std::vector<CVarListEntry> CVarRegistry::ListCommands() const
    {
        std::vector<CVarListEntry> out;
        for (const Command& command : m->commands)
        {
            if (!command.alive) continue;
            if (HasFlag(command.flags, CVarFlags::Hidden)) continue;
            if (HasFlag(command.flags, CVarFlags::Dev) && !m->devCvars) continue;
            out.push_back(CVarListEntry{ command.name, command.help, CVarType::Bool, command.flags });
        }
        return out;
    }

    ExecResult CVarRegistry::Execute(std::string_view line, Permission permission, SetBy by)
    {
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        if (line.empty()) return { false, "empty" };
        const auto gap = line.find(' ');
        const std::string name{ line.substr(0, gap) };
        const std::string_view args = gap == std::string_view::npos ? std::string_view{} : line.substr(gap + 1);

        if (const auto cit = m->commandByName.find(name); cit != m->commandByName.end())
        {
            Command& command = m->commands[cit->second];
            if (!command.alive) return { false, "unknown command '" + name + "'" };
            std::string text;
            command.fn(args, text, command.user);
            const bool ok = text.rfind("unknown", 0) != 0;
            return { ok, std::move(text) };
        }

        const CVarHandle handle = Find(name);
        if (handle.IsStale()) return { false, "unknown '" + name + "'" };
        const Slot& slot = m->slots[handle.index];
        if (args.empty())
            return { true, slot.name + " = " + FormatCVarValue(slot.published, slot.enumNames) };
        // The console and --set (ApplyCVarCommandLine) parse every type the same way.
        std::string token{ args };
        while (!token.empty() && token.back() == ' ') token.pop_back();
        std::string error;
        std::optional<CVarValue> parsed = ParseCVarText(token, slot.type, slot.enumNames, error);
        if (!parsed) return { false, error };
        const SetResult result = Set(handle, std::move(*parsed), by, {}, permission);
        if (result == SetResult::Denied) return { false, "denied" };
        if (result == SetResult::RefusedWeaker) return { false, "refused: a stronger source holds " + slot.name };
        if (result != SetResult::Applied) return { false, "rejected" };
        return { true, slot.name + " set (pending publish)" };
    }

    void CVarRegistry::AddCallback(CVarHandle handle, ChangeFn fn, void* user)
    {
        if (handle.index >= m->slots.size()) return;
        Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation || !fn) return;
        slot.callbacks.push_back(Slot::Callback{ fn, user });
    }

    bool CVarRegistry::CheatsEnabled() const
    {
        const CVarHandle handle = Find("cheats");
        const auto value = Get(handle);
        return value && value->type == CVarType::Bool && value->AsBool();
    }

    CVarHandle Detail::RegisterDeclaredCVar(const CVarDesc& desc)
    {
        CVarRegistry& registry = CVarRegistry::Get();
        const CVarHandle handle = registry.Register(desc);
#if defined(ARC_BUILD_DIST)
        const bool compiledOut = HasFlag(desc.flags, CVarFlags::Dev);   // Get()'s registry is built without Dev cvars
#else
        const bool compiledOut = false;
#endif
        if (handle.IsStale() && !compiledOut)
            ARC_ERROR("cvar: the declaration of '{}' (module '{}') was refused: {}", desc.name, desc.module,
                      registry.LastError());
        return handle;
    }
}

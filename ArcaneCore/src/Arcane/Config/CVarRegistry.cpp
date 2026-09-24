#include <Arcane/Config/CVarRegistry.hpp>

#include <charconv>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace Arcane
{
    namespace
    {
        const char* TypeName(CVarType t)
        {
            switch (t)
            {
            case CVarType::Bool: return "bool";
            case CVarType::Int32: return "int32";
            case CVarType::UInt32: return "uint32";
            case CVarType::Int64: return "int64";
            case CVarType::UInt64: return "uint64";
            case CVarType::Float32: return "float";
            case CVarType::Float64: return "double";
            case CVarType::String: return "string";
            default: return "reserved";
            }
        }

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

        std::string Format(const CVarValue& v)
        {
            switch (v.type)
            {
            case CVarType::Bool: return v.AsBool() ? "true" : "false";
            case CVarType::Int32: return std::to_string(v.AsInt32());
            case CVarType::UInt32: return std::to_string(v.AsUInt32());
            case CVarType::Int64: return std::to_string(v.AsInt64());
            case CVarType::UInt64: return std::to_string(v.AsUInt64());
            case CVarType::Float32: return std::to_string(v.AsFloat32());
            case CVarType::Float64: return std::to_string(v.AsFloat64());
            case CVarType::String: return v.AsString();
            default: return {};
            }
        }

        bool Implemented(CVarType t)
        {
            return t <= CVarType::String;
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
    }

    CVarRegistry::~CVarRegistry() { delete m; }

    const std::string& CVarRegistry::LastError() const { return m->lastError; }

    CVarRegistry& CVarRegistry::Get()
    {
#if defined(ARCANE_DIST)
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
                out += TypeName(e.type);
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
            out += explained->name;
            out += " = ";
            out += Format(explained->published);
            out += " [";
            out += SetByName(explained->setBy);
            out += "]\n";
            for (const CVarHistoryRecord& h : explained->history)
            {
                out += "  ";
                out += SetByName(h.by);
                if (!h.module.empty()) { out += " ("; out += h.module; out += ")"; }
                out += " = ";
                out += Format(h.value);
                out += '\n';
            }
        }
    }

    CVarHandle CVarRegistry::Register(const CVarDesc& desc)
    {
        m->lastError.clear();
        if (desc.name.empty() || !Implemented(desc.type))
        {
            m->lastError = "cvar '" + std::string(desc.name) + "' has no v1 accessor";
            return {};
        }
        if (Any(desc.flags, CVarFlags::Dev) && !m->devCvars)
        {
            m->lastError = "cvar '" + std::string(desc.name) + "' is Dev and this build compiled it out";
            return {};
        }
        if (const auto existing = m->byName.find(std::string(desc.name)); existing != m->byName.end())
        {
            m->lastError = "cvar '" + std::string(desc.name) + "' already registered by module '"
                         + m->slots[existing->second].declaredBy + "', refused from '" + std::string(desc.module) + "'";
            return {};
        }
        if (m->commandByName.contains(std::string(desc.name)))
        {
            m->lastError = "cvar '" + std::string(desc.name) + "' collides with a command";
            return {};
        }

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
        if (Any(slot.flags, CVarFlags::Archive)) slot.flags = slot.flags | CVarFlags::UserSettable;
        slot.name = desc.name;
        slot.help = desc.help;
        slot.declaredBy = desc.module;
        slot.min = desc.min;
        slot.max = desc.max;
        slot.published = Clamp(desc.defaultValue, slot.min, slot.max);
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
        if (Any(flags, CVarFlags::Dev) && !m->devCvars) return false;
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

    SetResult CVarRegistry::Set(CVarHandle handle, CVarValue value, SetBy by,
                                std::string_view sourceModule, Permission permission)
    {
        if (handle.index >= m->slots.size()) return SetResult::Stale;
        Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation) return SetResult::Stale;
        if (value.type != slot.type) return SetResult::TypeMismatch;

        const bool editor = permission == Permission::Editor;
        if (!editor)
        {
            if (!Any(slot.flags, CVarFlags::UserSettable)) return SetResult::Denied;
            if (Any(slot.flags, CVarFlags::Cheat) && !CheatsEnabled()) return SetResult::Denied;
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
            if (!slot.alive || !Any(slot.flags, CVarFlags::Cheat)) continue;
            const auto before = slot.history.size();
            std::erase_if(slot.history, [](const CVarHistoryRecord& h) {
                return h.by == SetBy::Console || h.by == SetBy::Code;
            });
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
            if (Any(slot.flags, CVarFlags::Hidden)) continue;
            if (Any(slot.flags, CVarFlags::Dev) && !m->devCvars) continue;
            out.push_back(CVarListEntry{ slot.name, slot.help, slot.type, slot.flags });
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
        if (args.empty())
        {
            const auto value = Get(handle);
            return { true, name + " = " + (value ? Format(value.value()) : std::string{}) };
        }
        // Assignment is Task 7's --set path and the console. Parse the eight types.
        const Slot& slot = m->slots[handle.index];
        std::string token{ args };
        while (!token.empty() && token.back() == ' ') token.pop_back();
        CVarValue parsed = slot.published;
        switch (slot.type)
        {
        case CVarType::Bool:
        {
            if (token == "1" || token == "true") parsed = CVarValue::Bool(true);
            else if (token == "0" || token == "false") parsed = CVarValue::Bool(false);
            else return { false, "expected true or false" };
            break;
        }
        case CVarType::Int32:
        {
            std::int32_t v = 0;
            const auto r = std::from_chars(token.data(), token.data() + token.size(), v);
            if (r.ec != std::errc{}) return { false, "expected int32" };
            parsed = CVarValue::Int32(v);
            break;
        }
        case CVarType::UInt32:
        {
            std::uint32_t v = 0;
            const auto r = std::from_chars(token.data(), token.data() + token.size(), v);
            if (r.ec != std::errc{}) return { false, "expected uint32" };
            parsed = CVarValue::UInt32(v);
            break;
        }
        case CVarType::Int64:
        {
            std::int64_t v = 0;
            const auto r = std::from_chars(token.data(), token.data() + token.size(), v);
            if (r.ec != std::errc{}) return { false, "expected int64" };
            parsed = CVarValue::Int64(v);
            break;
        }
        case CVarType::UInt64:
        {
            std::uint64_t v = 0;
            const auto r = std::from_chars(token.data(), token.data() + token.size(), v);
            if (r.ec != std::errc{}) return { false, "expected uint64" };
            parsed = CVarValue::UInt64(v);
            break;
        }
        case CVarType::Float32:
        {
            try { parsed = CVarValue::Float32(std::stof(token)); }
            catch (...) { return { false, "expected float" }; }
            break;
        }
        case CVarType::Float64:
        {
            try { parsed = CVarValue::Float64(std::stod(token)); }
            catch (...) { return { false, "expected double" }; }
            break;
        }
        case CVarType::String:
            parsed = CVarValue::String(token);
            break;
        default:
            return { false, "type has no accessor" };
        }
        const SetResult result = Set(handle, std::move(parsed), by, {}, permission);
        if (result == SetResult::Denied) return { false, "denied" };
        if (result == SetResult::RefusedWeaker) return { false, "refused: a stronger source holds " + name };
        if (result != SetResult::Applied) return { false, "rejected" };
        return { true, name + " set (pending publish)" };
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
}

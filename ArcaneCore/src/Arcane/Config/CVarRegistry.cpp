#include <Arcane/Config/CVarRegistry.hpp>

#include <Arcane/Config/CVarFormat.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarRef.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
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

        // Spec s3.2's default table, before any game policy. `absent`: the
        // setting does not exist for this caller (an Editor setting, or a
        // Hidden one, outside the editor), so it is reported as unknown rather
        // than denied.
        struct AccessRule
        {
            bool absent = false;
            bool allowed = false;
        };

        AccessRule DefaultAccess(Audience audience, CVarFlags flags, CVarContext ctx, bool write,
                                 bool cheatsOn, bool clientMaySetServer)
        {
            if (ctx == CVarContext::Editor) return { false, true };
            if (audience == Audience::Editor || HasFlag(flags, CVarFlags::Hidden)) return { true, false };
            if (!write)
                return { false, !HasFlag(flags, CVarFlags::Protected) || ctx == CVarContext::ServerAdmin };
            const bool cheat = HasFlag(flags, CVarFlags::Cheat);
            switch (audience)
            {
            case Audience::Game:       return { false, cheat && cheatsOn };
            case Audience::PlayerSafe: return { false, !cheat || cheatsOn };
            case Audience::Server:
                if (ctx == CVarContext::Client) return { false, clientMaySetServer && (!cheat || cheatsOn) };
                return { false, !cheat || cheatsOn };
            case Audience::Editor:     break;
            }
            return { true, false };
        }

        bool PublishedBool(const CVarRegistry& registry, std::string_view name, bool fallback)
        {
            const auto v = registry.Get(registry.Find(name));
            return v && v->type == CVarType::Bool ? v->AsBool() : fallback;
        }

        // The context and caller of the Execute that is dispatching the current
        // command. CommandFn carries neither, and the registry's own cvarlist /
        // cvar_explain must list and print only what that context, for that
        // caller, may read (s3.2: the table, then the game's policy).
        thread_local CVarContext tExecutingContext = CVarContext::Editor;
        thread_local const CVarCaller* tExecutingCaller = nullptr;

        struct ExecutingContextScope
        {
            CVarContext outerContext;
            const CVarCaller* outerCaller;
            ExecutingContextScope(CVarContext ctx, const CVarCaller* caller)
                : outerContext(tExecutingContext), outerCaller(tExecutingCaller)
            {
                tExecutingContext = ctx;
                tExecutingCaller = caller;
            }
            ~ExecutingContextScope()
            {
                tExecutingContext = outerContext;
                tExecutingCaller = outerCaller;
            }
        };

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
        std::map<std::string, std::string> aliases;     // old -> new
        std::unordered_set<std::string> warnedAliases;  // old names already warned about
        bool publishing = false;
        std::string lastError;

        // The table plus server.cheatsAllowed: outside the editor, the cheats
        // gate can be turned ON only where the host allows cheats at all.
        AccessRule Access(const CVarRegistry& self, const Slot& slot, CVarContext ctx, bool write,
                          const CVarValue* value) const
        {
            AccessRule rule = DefaultAccess(slot.audience, slot.flags, ctx, write, self.CheatsEnabled(),
                                            PublishedBool(self, "server.allowClientSetServer", false));
            if (rule.allowed && write && ctx != CVarContext::Editor && slot.name == "server.cheats"
                && value && value->type == CVarType::Bool && value->AsBool()
                && !PublishedBool(self, "server.cheatsAllowed", true))
                rule.allowed = false;
            return rule;
        }

        CVarPolicyFn policy = nullptr;
        void*        policyUser = nullptr;
        std::string  policyModule;
        CVarAuditFn  audit = nullptr;
        void*        auditUser = nullptr;

        struct Decision
        {
            bool          absent = false;
            bool          allowed = false;
            PolicyVerdict verdict = PolicyVerdict::Default;
        };

        // The table, then the game's policy (settings spec s3.2). The policy
        // is never asked in the Editor context, about an absent setting (it
        // cannot reach Editor or Hidden), or about a Protected read outside
        // ServerAdmin (it cannot make one readable).
        Decision Decide(const CVarRegistry& self, const Slot& slot, CVarContext ctx, const CVarCaller* caller,
                        bool write, const CVarValue* value) const
        {
            if (ctx == CVarContext::Editor) return { false, true, PolicyVerdict::Default };
            const AccessRule rule = Access(self, slot, ctx, write, value);
            if (rule.absent) return { true, false, PolicyVerdict::Default };   // the policy cannot reach it
            const bool protectedRead = !write && HasFlag(slot.flags, CVarFlags::Protected) && ctx != CVarContext::ServerAdmin;
            if (!policy || protectedRead) return { false, rule.allowed, PolicyVerdict::Default };
            const CVarInfo info{ slot.name, slot.type, slot.flags, slot.audience };
            const CVarRequest request{ ctx, caller, write };
            const PolicyVerdict verdict = policy(info, request, policyUser);
            if (verdict == PolicyVerdict::Allow) return { false, true, verdict };
            if (verdict == PolicyVerdict::Deny) return { false, false, verdict };
            return { false, rule.allowed, PolicyVerdict::Default };
        }

        void Audit(const std::string& name, const CVarCaller* caller, CVarContext ctx, PolicyVerdict verdict,
                   std::string oldValue, std::string newValue) const
        {
            if (!audit) return;
            CVarAuditRecord record;
            record.name = name;
            record.callerId = caller ? std::string(caller->id) : std::string();
            record.oldValue = std::move(oldValue);
            record.newValue = std::move(newValue);
            record.context = ctx;
            record.verdict = verdict;
            record.unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            audit(record, auditUser);
        }
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
        // Game / Pref-P per the inventory's R1: it serves both consoles.
        const CVarHandle history = Register(CVarDesc{
            .name = "console.historySize", .type = CVarType::Int32, .defaultValue = CVarValue::Int32(64),
            .min = CVarValue::Int32(1), .max = CVarValue::Int32(1024), .flags = CVarFlags::Archive,
            .help = "Command-line history depth.", .module = "engine",
            .audience = Audience::Game, .scope = SettingScope::PreferencesProject });
        (void)history;
        // Settings spec s3.2: the cheats gate and the two engine knobs, on EVERY
        // registry (test registries included), like console.historySize. They
        // are Server audience: a LocalHost or ServerAdmin console sets them, and
        // a Client only reads them.
        const auto serverBool = [this](std::string_view name, bool def, CVarFlags flags, std::string_view help)
        {
            CVarDesc desc;
            desc.name = name;
            desc.type = CVarType::Bool;
            desc.defaultValue = CVarValue::Bool(def);
            desc.flags = flags;
            desc.help = help;
            desc.module = "engine";
            desc.audience = Audience::Server;
            desc.scope = SettingScope::Project;
            desc.apply = ApplyMode::Live;
            (void)Register(desc);
        };
        serverBool("server.cheats", false, CVarFlags::Replicated,
                   "Allow Cheat settings to change (Source's sv_cheats). Turning it off reverts every Cheat setting.");
        serverBool("server.allowClientSetServer", false, CVarFlags::None,
                   "Let a connected client change Server settings when the game's policy leaves the decision to the engine.");
        serverBool("server.cheatsAllowed", true, CVarFlags::None,
                   "Whether server.cheats may be turned on outside the editor. A dedicated server sets its Project default to false.");
        (void)RegisterAlias("cheats", "server.cheats");
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
            for (const CVarListEntry& e : self->List(tExecutingContext))
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
            const CVarHandle handle = explained ? self->Find(explained->name) : CVarHandle{};
            const auto meta = self->Metadata(handle);
            // The same read rule as a plain `name` (s3.2): absent outside the
            // editor reads as unknown; a Protected value never prints where the
            // plain read is refused; the game's policy may refuse the caller of
            // the Execute that dispatched this command.
            const AccessRule table = meta ? DefaultAccess(meta->audience, meta->flags, tExecutingContext, false, false, false)
                                          : AccessRule{ true, false };
            if (table.absent)
            {
                out = "unknown cvar '";
                out += args;
                out += "'";
                return;
            }
            if (!self->CanRead(handle, tExecutingContext, tExecutingCaller))
            {
                // The table refuses a read for one reason only (Protected outside
                // ServerAdmin), and the policy is never asked about that one; any
                // other refusal is the policy's Deny.
                out = "denied: '";
                out += explained->name;
                out += table.allowed ? "' by the game's policy" : "' is protected";
                return;
            }
            const std::vector<std::string>& enumNames = meta->enumNames;
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
        if (const auto alias = m->aliases.find(name); alias != m->aliases.end())
            return refuse("collides with an alias of '" + alias->second + "'");

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
        slot.audience = desc.audience;
        // UserSettable is DERIVED from the audience (settings contract; spec
        // s3.2): a declaration's own bit is ignored, and Archive never implies it.
        slot.flags = static_cast<CVarFlags>(static_cast<std::uint32_t>(slot.flags)
                                            & ~static_cast<std::uint32_t>(CVarFlags::UserSettable));
        if (slot.audience == Audience::PlayerSafe || slot.audience == Audience::Server)
            slot.flags = slot.flags | CVarFlags::UserSettable;
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
        if (m->byName.contains(name) || m->commandByName.contains(name) || m->aliases.contains(name)) return false;
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

    bool CVarRegistry::CanRead(CVarHandle handle, CVarContext ctx, const CVarCaller* caller) const
    {
        if (handle.index >= m->slots.size()) return false;
        const Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation) return false;
        if (ctx == CVarContext::Editor) return true;
        // Decide runs the policy (game code), which may move the slot array:
        // `slot` is not read after it.
        const Impl::Decision read = m->Decide(*this, slot, ctx, caller, false, nullptr);
        return !read.absent && read.allowed;
    }

    SetResult CVarRegistry::Set(CVarHandle handle, CVarValue value, SetBy by,
                                std::string_view sourceModule, CVarContext ctx, const CVarCaller* caller)
    {
        if (handle.index >= m->slots.size()) return SetResult::Stale;
        if (!m->slots[handle.index].alive || m->slots[handle.index].generation != handle.generation) return SetResult::Stale;
        if (value.type != m->slots[handle.index].type) return SetResult::TypeMismatch;
        if (m->slots[handle.index].type == CVarType::Enum &&
            (value.AsEnum() < 0 || static_cast<std::size_t>(value.AsEnum()) >= m->slots[handle.index].enumNames.size()))
            return SetResult::TypeMismatch;   // an ordinal outside the declared names

        const Impl::Decision decision = m->Decide(*this, m->slots[handle.index], ctx, caller, true, &value);
        // The policy is game code. It may register a cvar and move the slot
        // array, so the slot is looked up again only after it has run.
        Slot& slot = m->slots[handle.index];
        // Every non-Editor change of a Server setting, and every policy denial
        // of one, reaches the audit sink (s3.2, s9). A table denial is neither.
        const bool audited = ctx != CVarContext::Editor && slot.audience == Audience::Server;
        if (!decision.allowed)
        {
            if (audited && decision.verdict == PolicyVerdict::Deny)
                m->Audit(slot.name, caller, ctx, decision.verdict,
                         FormatCVarValue(slot.history.back().value, slot.enumNames), FormatCVarValue(value, slot.enumNames));
            return SetResult::Denied;
        }

        value = Clamp(std::move(value), slot.min, slot.max);
        const std::string before = audited ? FormatCVarValue(slot.history.back().value, slot.enumNames) : std::string();
        const std::string after = audited ? FormatCVarValue(value, slot.enumNames) : std::string();
        // ONE record per (rung, source) (settings spec s4.5, O3): a repeat
        // replaces its own record, so re-opening a project or re-applying a
        // layer never grows the history. Records stay ordered weakest rung
        // first (newest last within a rung), so the back is always the winner.
        // A set beneath a stronger rung is RECORDED there: it is what that rung
        // holds once the stronger one is reverted. It reports RefusedWeaker
        // because it does not win now.
        std::erase_if(slot.history, [&](const CVarHistoryRecord& h) { return h.by == by && h.module == sourceModule; });
        const auto at = std::find_if(slot.history.begin(), slot.history.end(),
                                     [by](const CVarHistoryRecord& h) { return h.by > by; });
        const bool wins = at == slot.history.end();
        slot.history.insert(at, CVarHistoryRecord{ by, std::move(value), std::string(sourceModule) });
        slot.dirty = true;
        if (audited)
            m->Audit(slot.name, caller, ctx, decision.verdict, before, after);
        return wins ? SetResult::Applied : SetResult::RefusedWeaker;
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
        // The module's policy leaves with it: never call into an unloaded image.
        if (m->policy && m->policyModule == module)
            SetPolicy(nullptr, nullptr, {});
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

    void CVarRegistry::SetPolicy(CVarPolicyFn fn, void* user, std::string_view module)
    {
        m->policy = fn;
        m->policyUser = fn ? user : nullptr;
        m->policyModule = fn ? std::string(module) : std::string();
    }

    void CVarRegistry::SetAuditSink(CVarAuditFn fn, void* user)
    {
        m->audit = fn;
        m->auditUser = fn ? user : nullptr;
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
        CVarHandle handle = Find(name);
        if (handle.IsStale())
            if (const auto alias = m->aliases.find(std::string(name)); alias != m->aliases.end())
                handle = Find(alias->second);   // a read: no rename warning
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

    std::vector<CVarListEntry> CVarRegistry::List(CVarContext ctx) const
    {
        std::vector<CVarListEntry> out;
        for (const Slot& slot : m->slots)
        {
            if (!slot.alive) continue;
            if (HasFlag(slot.flags, CVarFlags::Hidden)) continue;
            if (HasFlag(slot.flags, CVarFlags::Dev) && !m->devCvars) continue;
            // A read, so the cheats gate and the client knob play no part.
            const AccessRule read = DefaultAccess(slot.audience, slot.flags, ctx, false, false, false);
            if (read.absent || !read.allowed) continue;
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

    ExecResult CVarRegistry::Execute(std::string_view line, CVarContext ctx, SetBy by, const CVarCaller* caller)
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
            if (ctx != CVarContext::Editor && HasFlag(command.flags, CVarFlags::Cheat) && !CheatsEnabled())
                return { false, "denied: '" + name + "' needs server.cheats" };
            std::string text;
            {
                const ExecutingContextScope scope{ ctx, caller };
                command.fn(args, text, command.user);
            }
            const bool ok = text.rfind("unknown", 0) != 0 && text.rfind("denied", 0) != 0;
            return { ok, std::move(text) };
        }

        const CVarHandle handle = Resolve(name);
        if (handle.IsStale()) return { false, "unknown '" + name + "'" };
        if (ctx != CVarContext::Editor)
        {
            if (m->Access(*this, m->slots[handle.index], ctx, false, nullptr).absent)
                return { false, "unknown '" + name + "'" };
            // A plain read asks the policy here; a write line is ONE write,
            // asked once, by Set. A read is refused for exactly two reasons:
            // the table (Protected outside ServerAdmin) or the policy's Deny.
            if (args.empty())
            {
                const Impl::Decision read = m->Decide(*this, m->slots[handle.index], ctx, caller, false, nullptr);
                if (!read.allowed)
                    return { false, read.verdict == PolicyVerdict::Deny ? "denied: '" + name + "' by the game's policy"
                                                                        : "denied: '" + name + "' is protected" };
            }
        }
        // The policy is game code: it may Register, and Register may grow the
        // slot vector. So a Slot reference is taken only AFTER the read-path
        // policy above has run, and lives in a block that ends BEFORE Set runs
        // the write-path policy. The echo after Set uses a copy of the name.
        // (handle.index itself is stable: Register appends or reuses a DEAD
        // slot, never moves a live one.)
        std::optional<CVarValue> parsed;
        std::string canonical;
        {
            const Slot& slot = m->slots[handle.index];
            if (args.empty())
                return { true, slot.name + " = " + FormatCVarValue(slot.published, slot.enumNames) };
            // The console and --set (ApplyCVarCommandLine) parse every type the same way.
            std::string token{ args };
            while (!token.empty() && token.back() == ' ') token.pop_back();
            std::string error;
            parsed = ParseCVarText(token, slot.type, slot.enumNames, error);
            if (!parsed) return { false, error };
            canonical = slot.name;
        }
        const SetResult result = Set(handle, std::move(*parsed), by, {}, ctx, caller);
        if (result == SetResult::Denied) return { false, "denied" };
        if (result == SetResult::RefusedWeaker) return { false, "refused: a stronger source holds " + canonical };
        if (result != SetResult::Applied) return { false, "rejected" };
        return { true, canonical + " set (pending publish)" };
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
        const CVarHandle handle = Find("server.cheats");
        const auto value = Get(handle);
        return value && value->type == CVarType::Bool && value->AsBool();
    }

    bool CVarRegistry::RegisterAlias(std::string_view oldName, std::string_view newName)
    {
        if (oldName.empty() || newName.empty() || oldName == newName) return false;
        const std::string from{ oldName };
        const std::string to{ newName };
        if (m->byName.contains(from) || m->commandByName.contains(from)) return false;
        if (m->aliases.contains(to)) return false;                     // the target is itself an old name
        if (const auto it = m->aliases.find(from); it != m->aliases.end()) return it->second == to;
        for (auto& [old, target] : m->aliases)
            if (target == from) target = to;                           // a -> b, then b -> c: a -> c
        m->aliases.emplace(from, to);
        return true;
    }

    CVarHandle CVarRegistry::Resolve(std::string_view name)
    {
        if (const CVarHandle direct = Find(name); !direct.IsStale()) return direct;
        const auto it = m->aliases.find(std::string(name));
        if (it == m->aliases.end()) return {};
        if (m->warnedAliases.insert(it->first).second)
            ARC_WARN("cvar: '{}' is renamed '{}' -- update the file, --set or script that names it (warned once)",
                     it->first, it->second);
        return Find(it->second);
    }

    std::string CVarRegistry::AliasTarget(std::string_view oldName) const
    {
        const auto it = m->aliases.find(std::string(oldName));
        return it == m->aliases.end() ? std::string{} : it->second;
    }

    std::vector<std::pair<std::string, std::string>> CVarRegistry::Aliases() const
    {
        return std::vector<std::pair<std::string, std::string>>(m->aliases.begin(), m->aliases.end());
    }

    bool Detail::RegisterDeclaredAlias(std::string_view oldName, std::string_view newName)
    {
        const bool ok = CVarRegistry::Get().RegisterAlias(oldName, newName);
        if (!ok)
            ARC_ERROR("cvar: the alias '{}' -> '{}' was refused (an empty or equal name, a live cvar or command "
                      "under the old name, or a chain)", oldName, newName);
        return ok;
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

#include <Arcane/Remote/Commands/ConsoleCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/RemoteCVarService.hpp>
#include <Arcane/Reflection.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace Arcane::Remote
{

// The wire args of console.exec, reflected for its schema (spec s5.3). The
// command reads its args JSON itself and refuses unknown keys.
struct ConsoleExecArgs {
    std::string line;   // one console line: "<command> [args]", "<cvar>" or "<cvar> <value>"
};
ARC_REFLECT_TYPE(ConsoleExecArgs)
    ARC_REFLECT_FIELD(ConsoleExecArgs, line)
ARC_REFLECT_TYPE_END()

namespace
{

constexpr std::string_view kUsage = "pass line: one console line, such as cvarlist or render.vsync false";
constexpr std::string_view kExplain = "cvar_explain";   // the history command the service refuses remotely

// The stored user (ruling F11). Main thread only: the command runs inside Drain.
CvarCommandUser*& StoredUser()
{
    static CvarCommandUser* user = nullptr;
    return user;
}

const char* ContextName(CVarContext context)
{
    switch (context) {
        case CVarContext::Editor: return "Editor";
        case CVarContext::LocalHost: return "LocalHost";
        case CVarContext::ServerAdmin: return "ServerAdmin";
        case CVarContext::Client: return "Client";
    }
    return "?";
}

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

// A listed (live, not Hidden) console command of the user's registry. The
// service resolves names the same way, so this only picks the op.
bool IsListedCommand(const CVarRegistry& registry, std::string_view name)
{
    for (const CVarListEntry& c : registry.ListCommands()) {
        if (c.name == name)
            return true;
    }
    return false;
}

// A refused console command (the service's or Execute's text), mapped to its code.
Envelope CommandRefusal(const std::string& command, const std::string& name, const std::string& text)
{
    if (text.find("is not remotely executable") != std::string::npos)   // RemoteCVarService::RunCommand
        return MakeError(command, "permission.denied", text,
                         "only console commands registered with ServerCanExecute run remotely");
    if (text.starts_with("denied"))   // Execute's gate: a Cheat command without server.cheats
        return MakeError(command, "permission.denied", text, "this host's context or cheat setting refuses '" + name + "'");
    // The command ran and reported failure; its text says why.
    return MakeError(command, "cvar.refused", text, "the console command '" + name + "' ran and reported failure");
}

// A refused cvar line, mapped by Task 20's table (CvarRefusalCode).
Envelope CvarLineRefusal(const std::string& command, const std::string& name, const std::string& text)
{
    const std::string_view code = CvarRefusalCode(text);
    if (code == "cvar.unknown")
        return MakeError(command, "cvar.unknown",
                         "'" + name + "' is not a console command or cvar this host serves remotely",
                         "servitor cvar list shows the cvars; a console command runs remotely only with ServerCanExecute");
    if (code == "cvar.protected")
        return MakeError(command, "cvar.protected", text, "a Protected cvar is never readable remotely");
    if (code == "cvar.type-mismatch")
        return MakeError(command, "cvar.type-mismatch", text,
                         "pass a value of the cvar's type (servitor cvar explain " + name + " shows it)");
    if (code == "permission.denied")
        return MakeError(command, "permission.denied", text, "this host's context or policy refuses it");
    if (code == "cvar.refused-weaker")
        return MakeError(command, "cvar.refused-weaker", text,
                         "servitor cvar explain " + name + " shows the stronger source that holds it");
    if (code == "arg.missing")
        return MakeError(command, "arg.missing", text, std::string(kUsage));
    return MakeError(command, "cvar.refused", text, "servitor cvar explain " + name + " shows its state");
}

CommandResult ConsoleExec(const Call& call, void* /*user*/)
{
    CommandResult result;
    if (!call.args.is_null() && !call.args.is_object()) {
        result.envelope = MakeError(call.command, "arg.invalid", call.command + " args must be a JSON object",
                                    std::string(kUsage));
        return result;
    }
    if (call.args.is_object()) {
        for (const auto& [key, value] : call.args.items()) {
            (void)value;
            if (key != "line") {
                result.envelope = MakeError(call.command, "arg.unknown",
                                            call.command + " has no argument '" + key + "'", std::string(kUsage));
                return result;
            }
        }
    }
    if (!call.args.is_object() || !call.args.contains("line")) {
        result.envelope = MakeError(call.command, "arg.missing", call.command + " needs line", std::string(kUsage));
        return result;
    }
    const nlohmann::json& lineJson = call.args.at("line");
    const std::string lineText = lineJson.is_string() ? lineJson.get<std::string>() : std::string{};
    const std::string_view line = Trim(lineText);
    if (line.empty()) {
        result.envelope = MakeError(call.command, "arg.invalid", "line must be a non-empty string", std::string(kUsage));
        return result;
    }
    const CvarCommandUser* user = StoredUser();
    if (user == nullptr || user->registry == nullptr) {
        result.envelope = MakeError(call.command, "host.unavailable", "no armed host session serves " + call.command,
                                    "relaunch the host with --remote");
        return result;
    }

    const std::size_t gap = line.find_first_of(" \t");
    const std::string name(line.substr(0, gap));
    const std::string rest(gap == std::string_view::npos ? std::string_view{} : Trim(line.substr(gap + 1)));
    // The service refuses cvar_explain (its history would bypass the Protected
    // mask); refuse it here before anything resolves or runs.
    if (name == kExplain) {
        result.envelope = MakeError(call.command, "command.unavailable",
                                    "cvar_explain is not remotely executable",
                                    "use servitor cvar explain <name> (its Protected rule masks history)");
        return result;
    }

    // One RemoteCVarService::Handle in the user's context (ruling E-1): a
    // command is the service's command path (op set: the ServerCanExecute
    // check, then Execute(line, context, SetBy::Console, &caller)); a cvar
    // alone is its get and a cvar with a value its set, so the Protected mask
    // and Hidden-as-unknown rules are the remote cvar service's own.
    const bool command = IsListedCommand(*user->registry, name);
    RemoteCVarService service(*user->registry);
    const RemoteCVarResponse res = service.Handle(
        {command || !rest.empty() ? "set" : "get", name, rest,
         call.callerId.empty() ? std::string("remote") : call.callerId, user->context});

    if (!res.ok) {
        result.envelope = command ? CommandRefusal(call.command, name, res.text)
                                  : CvarLineRefusal(call.command, name, res.text);
        result.envelope.data = {{"line", std::string(line)}, {"text", res.text}, {"context", ContextName(user->context)}};
        return result;
    }
    result.envelope = MakeOk(call.command,
                             {{"line", std::string(line)}, {"text", res.text}, {"context", ContextName(user->context)}},
                             {});
    UndoBlock undo;
    undo.label = nullptr;
    undo.undoable = false;
    undo.reason = "not-undoable-command";
    result.envelope.undo = std::move(undo);
    return result;
}

} // namespace

void SetConsoleCommandUser(CvarCommandUser* user)
{
    StoredUser() = user;
}

void ReleaseConsoleCommandUser(const CvarCommandUser* user)
{
    if (user != nullptr && StoredUser() == user)
        StoredUser() = nullptr;
}

void RegisterConsoleCommand()
{
    static bool registered = false;   // main thread only (HostSession::Start)
    if (registered)
        return;
    registered = true;

    Desc d;
    d.name = "console.exec";
    d.summary = "Run one console line: a ServerCanExecute command, or a cvar read or set, and return its text";
    d.access = Access::Drive;
    // Preferences, as cvar.set: a console line may set a cvar the user owns,
    // and None is refused for Drive.
    d.mutates = Mutates::Preferences;
    d.play = PlayPolicy::Any;
    d.thread = ExecThread::Main;
    d.needsFrame = false;
    d.argsMeta = Astra::GetMeta<ConsoleExecArgs>();
    d.fn = &ConsoleExec;
    std::string error;
    if (!CommandRegistry::Get().Register(std::move(d), &error))
        ARC_ERROR("remote: console command refused: {}", error);
}

} // namespace Arcane::Remote

#endif

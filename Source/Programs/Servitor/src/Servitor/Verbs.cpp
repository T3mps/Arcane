#include <Servitor/Verbs.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Servitor/Discover.hpp>
#include <Servitor/Launch.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>

namespace Servitor {

namespace {

bool Fail(std::string* error, std::string text)
{
    if (error != nullptr)
        *error = std::move(text);
    return false;
}

// quit [--force] [--discard-changes] -> host.quit {force, discardChanges} (Task 16).
constexpr VerbFlag kQuitFlags[] = {
    {"--force", false},             // quit even with unsaved changes
    {"--discard-changes", false},   // the editor's guard, answered "drop them" (a drive command, audited)
};

bool QuitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool WaitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);

// Tasks 15-29 add their rows here (ruling F17).
const std::array<VerbRow, 5> kVerbs = {{
    {"status", nullptr, nullptr, &StatusVerb},   // client-side: lists live hosts, no connect (Task 15)
    {"stats frame", "stats.frame", nullptr, nullptr},
    // Task 16. launch is client-side: it spawns the host and waits for its descriptor.
    {"launch", nullptr, &LaunchArgs, &LaunchVerb, LaunchFlags(), true, false,
     "editor|runtime|server [--headless] [--backend dx12|vulkan] [--remote-descriptor <path>] [-- <host flags>]"},
    {"quit", "host.quit", &QuitArgs, nullptr, kQuitFlags, false, false, "[--force] [--discard-changes]"},
    // `frames N` counts host.frame, the host's sim advances (AM1): one per fixed
    // tick on a server, a headless runtime or a step; a windowed runtime in Run
    // mode advances it once per loop pass, whatever its sub-steps.
    {"wait", "host.wait", &WaitArgs, nullptr, {}, false, true,
     "ready|settled|frames N|reload|state <s>   (frames counts host.frame: sim advances, not rendered frames)"},
}};

bool QuitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("quit"), rest, &tokens))
        return Fail(error, *problem);
    if (!tokens.positional.empty())
        return Fail(error, "quit takes no arguments, got " + tokens.positional.front());
    bool force = false;
    bool discard = false;
    for (const auto& [flag, value] : tokens.flags) {
        (void)value;
        force = force || flag == "--force";
        discard = discard || flag == "--discard-changes";
    }
    out = {{"force", force}, {"discardChanges", discard}};
    return true;
}

bool WaitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    const std::string usage = "wait needs ready, settled, frames N, reload or state <s>";
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("wait"), rest, &tokens))
        return Fail(error, *problem);
    const auto& p = tokens.positional;
    if (p.empty())
        return Fail(error, usage);
    const std::string& what = p.front();
    if (what == "ready" || what == "settled" || what == "reload") {
        if (p.size() != 1)
            return Fail(error, "wait " + what + " takes nothing more, got " + p[1]);
        out = {{"what", what}};
        return true;
    }
    if (what == "frames") {
        if (p.size() != 2)
            return Fail(error, "wait frames needs one count N");
        const std::string& n = p[1];
        errno = 0;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(n.c_str(), &end, 10);
        if (n.empty() || n[0] < '0' || n[0] > '9' || end != n.c_str() + n.size() || errno == ERANGE ||
            v > std::numeric_limits<std::uint32_t>::max())
            return Fail(error, "wait frames needs a non-negative whole number, got '" + n + "'");
        out = {{"what", "frames"}, {"frames", static_cast<std::uint32_t>(v)}};
        return true;
    }
    if (what == "state") {
        if (p.size() != 2)
            return Fail(error, "wait state needs one state (ready, playing, paused or stopping)");
        out = {{"what", "state"}, {"state", p[1]}};
        return true;
    }
    return Fail(error, usage + ", got " + what);
}

// How many leading tokens spell `words` (space-separated); 0 when they do not.
std::size_t WordsMatched(std::string_view words, std::span<const std::string> tokens)
{
    std::size_t used = 0;
    while (!words.empty()) {
        const std::size_t space = words.find(' ');
        const std::string_view word = words.substr(0, space);
        if (used >= tokens.size() || tokens[used] != word)
            return 0;
        ++used;
        words = space == std::string_view::npos ? std::string_view{} : words.substr(space + 1);
    }
    return used;
}

} // namespace

std::span<const VerbRow> VerbTable()
{
    return kVerbs;
}

const VerbRow* MatchVerb(std::span<const std::string> tokens, std::size_t* consumed)
{
    const VerbRow* best = nullptr;
    std::size_t bestUsed = 0;
    for (const VerbRow& row : kVerbs) {
        const std::size_t used = WordsMatched(row.words, tokens);
        if (used > bestUsed) {
            best = &row;
            bestUsed = used;
        }
    }
    if (consumed != nullptr)
        *consumed = bestUsed;
    return best;
}

const VerbRow* FindVerb(std::string_view words)
{
    for (const VerbRow& row : kVerbs) {
        if (words == row.words)
            return &row;
    }
    return nullptr;
}

std::optional<std::string> SplitVerbTokens(const VerbRow& row, std::span<const std::string> rest, VerbTokens* out)
{
    VerbTokens local;
    VerbTokens& tokens = out != nullptr ? *out : local;
    tokens = VerbTokens{};
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string& token = rest[i];
        if (token == "--" && row.passThrough) {
            tokens.passThrough.assign(rest.begin() + static_cast<std::ptrdiff_t>(i) + 1, rest.end());
            return std::nullopt;
        }
        if (!token.starts_with("--")) {   // a single '-' stays a value (negative numbers)
            tokens.positional.push_back(token);
            continue;
        }
        const VerbFlag* flag = nullptr;
        for (const VerbFlag& f : row.flags) {
            if (token == f.name)
                flag = &f;
        }
        if (flag == nullptr)
            return "unknown flag " + token;
        if (!flag->takesValue) {
            tokens.flags.emplace_back(token, std::string{});
            continue;
        }
        if (i + 1 >= rest.size() || rest[i + 1].starts_with("--"))
            return token + " needs a value";
        tokens.flags.emplace_back(token, rest[++i]);
    }
    return std::nullopt;
}

} // namespace Servitor

#endif

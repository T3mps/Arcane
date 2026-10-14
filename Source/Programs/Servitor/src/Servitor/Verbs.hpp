#pragma once
// The ONE Servitor verb table (ruling F17). Every task that adds a CLI verb
// adds its row to Verbs.cpp; main.cpp and Run never change for a new verb.
#include <Servitor/Client.hpp>
#include <Json.hpp>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Servitor {

// One --flag a verb accepts beyond the global flags (Task 16). Run refuses
// every other "--" token after the verb with usage exit 2 (Rule 3).
struct VerbFlag {
    const char* name;     // spelled with its dashes: "--force"
    bool takesValue;      // the next token is its value; it must exist and must not start with "--"
};

// One row of the Servitor verb table.
struct VerbRow {
    const char* words;    // the verb as typed, space-separated: "stats frame", "cvar get"
    const char* command;  // the wire command it posts ("stats.frame"); nullptr for a client-side verb
    // Turns the tokens after the verb into `args`; false is a usage error
    // (exit 2) with *error set. nullptr: the verb takes no arguments (Run
    // refuses any). A client-side row may have one too: Run then hands
    // `local` the tokens unparsed, and `local` calls the parser itself.
    bool (*args)(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
    // Client-side verbs (status, launch, commands, list, describe, schema,
    // call) run here and return the exit code instead of posting.
    // nullptr for a row that posts `command`.
    int (*local)(std::span<const std::string> rest, const GlobalOptions& opt);
    // The verb's own flags (Task 16); empty = none.
    std::span<const VerbFlag> flags = {};
    // A bare "--" ends the verb's own tokens; everything after it is handed
    // over verbatim (launch's host flags). Without it a bare "--" is refused.
    bool passThrough = false;
    // Discovery waits up to --timeout for a host to appear instead of
    // answering target.none at once (wait: a host that is still booting).
    bool awaitsHost = false;
    // The argument synopsis the usage text prints after `words`; null = none.
    const char* usage = nullptr;
};

std::span<const VerbRow> VerbTable(); // Servitor/Verbs.cpp: one static array
// Longest `words` prefix of `tokens` (globals already removed); null = unknown verb (Run exits 2).
const VerbRow* MatchVerb(std::span<const std::string> tokens, std::size_t* consumed);
// The row whose words are exactly `words`; null when none.
const VerbRow* FindVerb(std::string_view words);

// The tokens after a verb, split by the row's declared flags.
struct VerbTokens {
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> flags;   // {"--backend", "vulkan"}; a switch has ""
    std::vector<std::string> passThrough;                     // after a bare "--" (passThrough rows)
};
// nullopt when every "--" token is one of row.flags (a value flag followed by
// its value) or, for a passThrough row, the bare "--" separator. Otherwise the
// usage problem ("unknown flag --x", "--x needs a value"). `out` may be null.
std::optional<std::string> SplitVerbTokens(const VerbRow& row, std::span<const std::string> rest, VerbTokens* out);

} // namespace Servitor

#endif

#pragma once
// The ONE Servitor verb table (ruling F17). Every task that adds a CLI verb
// adds its row to Verbs.cpp; main.cpp and Run never change for a new verb.
#include <Servitor/Client.hpp>
#include <Json.hpp>
#include <cstddef>
#include <span>
#include <string>

#if !defined(ARC_BUILD_DIST)

namespace Servitor {

// One row of the Servitor verb table.
struct VerbRow {
    const char* words;    // the verb as typed, space-separated: "stats frame", "cvar get"
    const char* command;  // the wire command it posts ("stats.frame"); nullptr for a client-side verb
    // Turns the tokens after the verb into `args`; false is a usage error
    // (exit 2) with *error set. nullptr: the verb takes no arguments.
    bool (*args)(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
    // Client-side verbs (status, launch, quit, wait, commands, list, describe,
    // schema, call) run here and return the exit code instead of posting.
    // nullptr for a row that posts `command`.
    int (*local)(std::span<const std::string> rest, const GlobalOptions& opt);
};

std::span<const VerbRow> VerbTable(); // Servitor/Verbs.cpp: one static array
// Longest `words` prefix of `tokens` (globals already removed); null = unknown verb (Run exits 2).
const VerbRow* MatchVerb(std::span<const std::string> tokens, std::size_t* consumed);

} // namespace Servitor

#endif

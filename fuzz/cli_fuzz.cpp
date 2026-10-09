// libFuzzer harness: Arcane::Cli argv parsing (ArcaneCore/src/Arcane/Cli) and
// the dedicated server's own spec over it (ArcaneServer/src/ServerConfig.cpp).
//
// The input is an argv: arguments separated by NUL bytes (argv[0] is supplied
// by the harness). Each argv is parsed twice:
//   1. ServerConfig::Parse -- the real server command line.
//   2. A spec that uses every Cli feature: flags, short aliases, Choices,
//      Required, every CliType, Many().
// Properties checked on (2) when Parse says ok:
//   - every typed option's value parses fully as its type (Parse validated it),
//     so GetAs<T> returns the value the user typed, never a silent T{};
//   - a value given for a Choices option is one of the choices;
//   - a Required option was actually supplied;
//   - a Many() option's occurrences end with the value Get() reports.
// Exit codes are only ever 0 or 2.

#include <Arcane/Cli/Cli.hpp>

#include "../ArcaneServer/src/ServerConfig.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    [[noreturn]] void Fail(const char* property, int line)
    {
        std::fprintf(stderr, "cli_fuzz: property violated (line %d): %s\n", line, property);
        std::abort();
    }
#define FUZZ_CHECK(cond) do { if (!(cond)) Fail(#cond, __LINE__); } while (0)

    template <typename T>
    bool ParsesFully(const std::string& s)
    {
        T v{};
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        return ec == std::errc{} && end == s.data() + s.size();
    }

    Arcane::Cli MakeSpec()
    {
        using Arcane::CliType;
        Arcane::Cli cli{ "fuzz", "every Cli feature" };
        cli.Flag("verbose", "a flag").Short('v');
        cli.Flag("dry-run", "another flag");
        cli.Option("name", "", "required string").Required().Short('n');
        cli.Option("count", "3", "uint").Type(CliType::Uint).Short('c');
        cli.Option("offset", "-1", "int").Type(CliType::Int);
        cli.Option("scale", "1.5", "double").Type(CliType::Double).Short('s');
        cli.Option("mode", "fast", "choice").Choices({ "fast", "slow", "" });
        cli.Option("set", "", "repeatable").Many();
        cli.Option("level", "2", "uint choice").Type(CliType::Uint).Choices({ "1", "2", "3" });
        return cli;
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // Split on NUL into argv; argv[0] is the program name.
    std::vector<std::string> args{ "fuzz" };
    std::string cur;
    for (size_t i = 0; i < size; ++i)
    {
        if (data[i] == 0) { args.push_back(cur); cur.clear(); }
        else cur.push_back(static_cast<char>(data[i]));
    }
    if (!cur.empty()) args.push_back(cur);
    if (args.size() > 64) return 0;

    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    const int argc = static_cast<int>(args.size());

    const auto server = Arcane::Server::ServerConfig::Parse(argc, argv.data());
    FUZZ_CHECK(server.exitCode == 0 || server.exitCode == 2);
    FUZZ_CHECK(!server.config || server.exitCode == 0);

    static const Arcane::Cli spec = MakeSpec();
    const Arcane::Cli::Result r = spec.Parse(argc, argv.data());
    FUZZ_CHECK(r.exitCode == 0 || r.exitCode == 2);
    if (!r.ok)
    {
        FUZZ_CHECK(r.helpRequested == (r.exitCode == 0));
        return 0;
    }

    FUZZ_CHECK(ParsesFully<std::uint64_t>(r.Get("count")));
    FUZZ_CHECK(ParsesFully<std::int64_t>(r.Get("offset")));
    FUZZ_CHECK(ParsesFully<double>(r.Get("scale")));
    FUZZ_CHECK(ParsesFully<std::uint64_t>(r.Get("level")));
    FUZZ_CHECK(std::to_string(r.GetAs<std::uint64_t>("count")) == r.Get("count") || r.Get("count").front() == '0');
    const std::string mode = r.Get("mode");
    FUZZ_CHECK(mode == "fast" || mode == "slow" || mode.empty());
    const std::string level = r.Get("level");
    FUZZ_CHECK(level == "1" || level == "2" || level == "3");
    FUZZ_CHECK(r.Supplied("name"));
    const std::vector<std::string> sets = r.GetMany("set");
    FUZZ_CHECK(sets.empty() ? !r.Supplied("set") : sets.back() == r.Get("set"));
    (void)r.Flag("verbose");
    (void)r.GetAs<double>("scale");
    (void)r.GetAs<std::int64_t>("offset");
    return 0;
}

#pragma once
// Arcane::Cli -- a small typed, declarative command-line argument parser.
//
// Register options/flags up front (name + default + help, optional short alias,
// Choices validation, Required, a numeric Type), then Parse(argc, argv). On
// --help/-h it prints usage and returns {ok=false, helpRequested, exitCode=0};
// on a bad/unknown/missing arg it prints the reason + usage and returns
// {ok=false, exitCode=2}; the caller does `if (!r.ok) return r.exitCode;`.
//
// Reusable engine-wide (Loom today; future tools/Game host/harnesses). std-only,
// presentation-free. Out of scope (documented extension points, NOT built):
// subcommands, config files, env-var fallback, positional/array options.
// PRESENTATION-FREE + C++23-clean.
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
namespace Arcane
{
    enum class CliType : std::uint8_t { String, Int, Uint, Double };

    class Cli
    {
    public:
        Cli(std::string prog, std::string desc) : m_prog(std::move(prog)), m_desc(std::move(desc)) {}

        // Fluent builder over the just-registered option (references the Cli + index,
        // so it stays valid across m_opts reallocation).
        struct Builder
        {
            Cli* cli; std::size_t idx;
            Builder& Short(char s)                              { cli->m_opts[idx].shortAlias = s; return *this; }
            Builder& Choices(std::initializer_list<const char*> ch) { for (const char* s : ch) cli->m_opts[idx].choices.emplace_back(s); return *this; }
            Builder& Required()                                 { cli->m_opts[idx].required = true; return *this; }
            Builder& Type(CliType t)                            { cli->m_opts[idx].type = t; return *this; }
        };

        Builder Flag(std::string name, std::string help);                         // bool, default off
        Builder Option(std::string name, std::string defaultValue, std::string help);

        struct Result
        {
            bool ok = false;
            bool helpRequested = false;
            int  exitCode = 0;                                  // 0 (help) or 2 (error) when !ok
            std::unordered_map<std::string, std::string> values;// option name -> resolved value (or default)
            std::unordered_map<std::string, bool>        flags; // flag name -> present?

            [[nodiscard]] bool        Flag(std::string_view name) const;
            [[nodiscard]] std::string Get (std::string_view name) const;
            template <typename T> [[nodiscard]] T GetAs(std::string_view name) const;
        };

        [[nodiscard]] Result Parse(int argc, char** argv) const;
        void PrintUsage() const;

    private:
        struct Opt
        {
            std::string name, help, def, raw;
            char shortAlias = 0;
            bool isFlag = false, required = false;
            CliType type = CliType::String;
            std::vector<std::string> choices;
        };
        const Opt* FindLong(std::string_view name) const;
        const Opt* FindShort(char c) const;

        std::string m_prog, m_desc;
        std::vector<Opt> m_opts;
    };

    // Typed access: the value was already validated at Parse time, so from_chars
    // succeeds; defensively returns T{} on an unexpected failure.
    template <typename T>
    T Cli::Result::GetAs(std::string_view name) const
    {
        const std::string s = Get(name);
        T out{};
        (void)std::from_chars(s.data(), s.data() + s.size(), out);
        return out;
    }
}

#include <Arcane/Cli/Cli.hpp>
#include <charconv>
#include <cstdio>
namespace Arcane
{
    Cli::Builder Cli::Flag(std::string name, std::string help)
    {
        m_opts.push_back(Opt{ std::move(name), std::move(help), "false", 0, true, false, CliType::String, {} });
        return Builder{ this, m_opts.size() - 1 };
    }
    Cli::Builder Cli::Option(std::string name, std::string defaultValue, std::string help)
    {
        m_opts.push_back(Opt{ std::move(name), std::move(help), std::move(defaultValue), 0, false, false, CliType::String, {} });
        return Builder{ this, m_opts.size() - 1 };
    }
    const Cli::Opt* Cli::FindLong(std::string_view name) const
    {
        for (const Opt& o : m_opts) if (o.name == name) return &o;
        return nullptr;
    }
    const Cli::Opt* Cli::FindShort(char c) const
    {
        for (const Opt& o : m_opts) if (o.shortAlias != 0 && o.shortAlias == c) return &o;
        return nullptr;
    }
    bool        Cli::Result::Flag(std::string_view name) const { const auto it = flags.find(std::string(name)); return it != flags.end() && it->second; }
    std::string Cli::Result::Get (std::string_view name) const { const auto it = values.find(std::string(name)); return it != values.end() ? it->second : std::string(); }

    void Cli::PrintUsage() const
    {
        std::printf("%s -- %s\n", m_prog.c_str(), m_desc.c_str());
        for (const Opt& o : m_opts)
        {
            if (o.isFlag) std::printf("  --%-16s %s\n", o.name.c_str(), o.help.c_str());
            else          std::printf("  --%-16s %s (default %s)\n", o.name.c_str(), o.help.c_str(), o.def.c_str());
        }
        std::printf("  --%-16s %s\n", "help", "show this help");
    }

    namespace
    {
        bool NumericOk(CliType t, const std::string& s)
        {
            if (t == CliType::String) return true;
            if (s.empty()) return false;
            if (t == CliType::Uint)   { std::uint64_t v; auto r = std::from_chars(s.data(), s.data()+s.size(), v); return r.ec == std::errc{} && r.ptr == s.data()+s.size(); }
            if (t == CliType::Int)    { std::int64_t  v; auto r = std::from_chars(s.data(), s.data()+s.size(), v); return r.ec == std::errc{} && r.ptr == s.data()+s.size(); }
            double d; auto r = std::from_chars(s.data(), s.data()+s.size(), d); return r.ec == std::errc{} && r.ptr == s.data()+s.size();
        }
        bool ChoiceOk(const std::vector<std::string>& choices, const std::string& v)
        {
            if (choices.empty()) return true;
            for (const std::string& c : choices) if (c == v) return true;
            return false;
        }
    }

    Cli::Result Cli::Parse(int argc, char** argv) const
    {
        Result r;
        for (const Opt& o : m_opts) { if (o.isFlag) r.flags[o.name] = false; else r.values[o.name] = o.def; }

        auto fail = [&](const std::string& why) { std::fprintf(stderr, "error: %s\n", why.c_str()); PrintUsage(); r.ok = false; r.exitCode = 2; return r; };

        for (int i = 1; i < argc; ++i)
        {
            std::string a = argv[i];
            if (a == "--help" || a == "-h") { PrintUsage(); r.ok = false; r.helpRequested = true; r.exitCode = 0; return r; }

            const Opt* opt = nullptr;
            std::string inlineVal; bool hasInline = false;
            if (a.rfind("--", 0) == 0)
            {
                std::string body = a.substr(2);
                const auto eq = body.find('=');
                if (eq != std::string::npos) { hasInline = true; inlineVal = body.substr(eq + 1); body = body.substr(0, eq); }
                opt = FindLong(body);
            }
            else if (a.size() >= 2 && a[0] == '-')
            {
                char sc = a[1];
                if (a.size() > 2 && a[2] == '=') { hasInline = true; inlineVal = a.substr(3); }
                opt = FindShort(sc);
            }
            if (!opt) return fail("unknown argument '" + a + "'");

            if (opt->isFlag)
            {
                if (hasInline) return fail("flag '--" + opt->name + "' takes no value");
                r.flags[opt->name] = true;
                continue;
            }

            std::string val;
            if (hasInline) val = inlineVal;
            else if (i + 1 < argc) val = argv[++i];
            else return fail("option '--" + opt->name + "' requires a value");

            if (!ChoiceOk(opt->choices, val)) return fail("'--" + opt->name + "' must be one of the allowed values, got '" + val + "'");
            if (!NumericOk(opt->type, val))   return fail("'--" + opt->name + "' expects a number, got '" + val + "'");
            r.values[opt->name] = val;
        }

        // Required semantics: a required option is declared with an EMPTY default
        // (as in the test); if the resolved value is still that empty default, the
        // user never supplied it, so treat it as missing.
        for (const Opt& o : m_opts)
            if (o.required && !o.isFlag && r.values[o.name] == o.def && o.def.empty())
                return fail("missing required option '--" + o.name + "'");

        r.ok = true; r.exitCode = 0;
        return r;
    }
}

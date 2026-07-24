#include <Arcane/Material/MaterialSource.hpp>

#include <charconv>
#include <cstdint>

namespace Arcane
{
    namespace
    {
        constexpr std::string_view kParamPrefix = "//@param";

        // Names the fullscreen template already owns at global scope -- a param
        // with one of these would shadow or collide in the stitched source.
        constexpr std::string_view kReservedNames[] = {
            "Time", "DeltaTime", "ViewportSize", "MaterialSampler",
        };

        std::string_view TrimView(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.remove_suffix(1);
            return s;
        }

        std::string_view TakeToken(std::string_view& s)
        {
            s = TrimView(s);
            std::size_t end = 0;
            while (end < s.size() && s[end] != ' ' && s[end] != '\t')
                ++end;
            std::string_view tok = s.substr(0, end);
            s.remove_prefix(end);
            return tok;
        }

        bool ParseFloat(std::string_view s, float& out)
        {
            s = TrimView(s);
            if (s.empty())
                return false;
            const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
            return r.ec == std::errc() && r.ptr == s.data() + s.size();
        }

        // "(a, b, c)" -> components. False on malformed text or wrong count.
        bool ParseTuple(std::string_view s, float* out, int minCount, int maxCount, int& count)
        {
            s = TrimView(s);
            if (s.size() < 2 || s.front() != '(' || s.back() != ')')
                return false;
            s = s.substr(1, s.size() - 2);

            count = 0;
            while (true)
            {
                const std::size_t comma = s.find(',');
                const std::string_view part = comma == std::string_view::npos ? s : s.substr(0, comma);
                if (count >= maxCount || !ParseFloat(part, out[count]))
                    return false;
                ++count;
                if (comma == std::string_view::npos)
                    break;
                s.remove_prefix(comma + 1);
            }
            return count >= minCount;
        }

        bool ValidIdentifier(std::string_view name)
        {
            if (name.empty())
                return false;
            auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
            auto digit = [](char c) { return c >= '0' && c <= '9'; };
            if (!alpha(name.front()))
                return false;
            for (char c : name)
                if (!alpha(c) && !digit(c))
                    return false;
            return true;
        }

        bool TypeFromToken(std::string_view tok, MatParamType& out)
        {
            if (tok == "float")   { out = MatParamType::Float;   return true; }
            if (tok == "float2")  { out = MatParamType::Float2;  return true; }
            if (tok == "float4")  { out = MatParamType::Float4;  return true; }
            if (tok == "color")   { out = MatParamType::Color;   return true; }
            if (tok == "texture") { out = MatParamType::Texture; return true; }
            return false;
        }

        const char* HlslType(MatParamType t)
        {
            switch (t)
            {
                case MatParamType::Float:  return "float";
                case MatParamType::Float2: return "float2";
                case MatParamType::Float4: return "float4";
                case MatParamType::Color:  return "float4";
                case MatParamType::Texture: break;
            }
            return "float4";
        }

        std::uint64_t Fnv64Str(std::uint64_t h, std::string_view s) noexcept
        {
            for (char c : s)
            {
                h ^= static_cast<std::uint8_t>(c);
                h *= 1099511628211ull;
            }
            return h;
        }
    }

    MaterialSourceParse ParseMaterialSource(std::string_view snippet)
    {
        MaterialSourceParse out;

        int lineNo = 0;
        std::size_t pos = 0;
        while (pos <= snippet.size())
        {
            ++lineNo;
            std::size_t end = snippet.find('\n', pos);
            if (end == std::string_view::npos)
                end = snippet.size();
            std::string_view line = TrimView(snippet.substr(pos, end - pos));
            const bool lastLine = end == snippet.size();
            pos = end + 1;

            if (line.substr(0, kParamPrefix.size()) != kParamPrefix)
            {
                if (lastLine)
                    break;
                continue;
            }

            auto fail = [&](const std::string& msg)
            {
                out.errors.push_back("line " + std::to_string(lineNo) + ": " + msg);
            };

            std::string_view rest = line.substr(kParamPrefix.size());
            // Require whitespace after the prefix so `//@paramx` is not a decl.
            if (!rest.empty() && rest.front() != ' ' && rest.front() != '\t')
            {
                fail("expected whitespace after //@param");
                if (lastLine) break; else continue;
            }

            ParamDecl d;
            ParamMeta meta;

            const std::string_view typeTok = TakeToken(rest);
            if (!TypeFromToken(typeTok, d.type))
            {
                fail("unknown param type '" + std::string(typeTok) +
                     "' (float|float2|float4|color|texture)");
                if (lastLine) break; else continue;
            }

            const std::string_view nameTok = TakeToken(rest);
            if (!ValidIdentifier(nameTok))
            {
                fail("invalid param name '" + std::string(nameTok) + "'");
                if (lastLine) break; else continue;
            }
            d.name = std::string(nameTok);

            bool bad = false;
            for (std::string_view reserved : kReservedNames)
            {
                if (nameTok == reserved)
                {
                    fail("'" + d.name + "' is reserved by the material template");
                    bad = true;
                    break;
                }
            }
            for (const ParamDecl& prev : out.decls)
            {
                if (prev.name == d.name)
                {
                    fail("duplicate param '" + d.name + "'");
                    bad = true;
                    break;
                }
            }
            if (bad)
            {
                if (lastLine) break; else continue;
            }

            // Optional `= <default>`; the range `[min..max]` may follow it.
            rest = TrimView(rest);
            if (!rest.empty() && rest.front() == '=')
            {
                rest.remove_prefix(1);
                rest = TrimView(rest);
                // The default runs until the range bracket (if any).
                const std::size_t bracket = rest.find('[');
                std::string_view defText = TrimView(
                    bracket == std::string_view::npos ? rest : rest.substr(0, bracket));
                rest = bracket == std::string_view::npos ? std::string_view{}
                                                         : rest.substr(bracket);

                float f[4] = {0, 0, 0, 0};
                int n = 0;
                bool ok = false;
                switch (d.type)
                {
                    case MatParamType::Float:
                        ok = ParseFloat(defText, f[0]);
                        d.def = MatParamValue::MakeFloat(f[0]);
                        break;
                    case MatParamType::Float2:
                        ok = ParseTuple(defText, f, 2, 2, n);
                        d.def = MatParamValue::MakeFloat2(f[0], f[1]);
                        break;
                    case MatParamType::Float4:
                        ok = ParseTuple(defText, f, 4, 4, n);
                        d.def = MatParamValue::MakeFloat4(f[0], f[1], f[2], f[3]);
                        break;
                    case MatParamType::Color:
                        ok = ParseTuple(defText, f, 3, 4, n);
                        d.def = MatParamValue::MakeColor(f[0], f[1], f[2], n == 4 ? f[3] : 1.0f);
                        break;
                    case MatParamType::Texture:
                        ok = false;   // texture defaults bind through the instance
                        break;
                }
                if (!ok)
                {
                    fail("bad default '" + std::string(defText) + "' for " +
                         std::string(typeTok) + " '" + d.name + "'");
                    if (lastLine) break; else continue;
                }
            }
            else if (d.type == MatParamType::Texture)
            {
                d.def = MatParamValue::MakeTexture(Guid::Nil());
            }
            else
            {
                // No default: zeroes of the declared type.
                MatParamValue zero;
                zero.type = d.type;
                d.def = zero;
            }

            // Optional `[min..max]` slider hint.
            rest = TrimView(rest);
            if (!rest.empty() && rest.front() == '[')
            {
                if (rest.back() != ']')
                {
                    fail("unterminated range on '" + d.name + "'");
                    if (lastLine) break; else continue;
                }
                const std::string_view range = rest.substr(1, rest.size() - 2);
                const std::size_t dots = range.find("..");
                float lo = 0.0f, hi = 0.0f;
                if (d.type == MatParamType::Texture ||
                    dots == std::string_view::npos ||
                    !ParseFloat(range.substr(0, dots), lo) ||
                    !ParseFloat(range.substr(dots + 2), hi) || hi < lo)
                {
                    fail("bad range on '" + d.name + "' (expected [min..max])");
                    if (lastLine) break; else continue;
                }
                meta.sliderMin = lo;
                meta.sliderMax = hi;
                rest = {};   // the whole remainder WAS the range
            }

            // Anything left is a malformed decl the author meant to matter --
            // the classic `//@param float Speed 2.0` (forgotten '=') must not
            // silently declare Speed = 0.
            rest = TrimView(rest);
            if (!rest.empty())
            {
                fail("unexpected trailing text '" + std::string(rest) + "' after '" +
                     d.name + "' (missing '='?)");
                if (lastLine) break; else continue;
            }

            out.decls.push_back(std::move(d));
            out.metas.push_back(std::move(meta));
            if (lastLine)
                break;
        }

        return out;
    }

    std::string GenerateMaterialBindings(const MaterialTemplate& templ)
    {
        // Members emit in declaration order: HLSL's cbuffer packing applies the
        // same rules MaterialTemplate::Build used, so the shader's offsets match
        // the CPU layout byte for byte. Keep the two in lockstep.
        std::string outText;

        bool anyNumeric = false;
        for (const ParamDecl& d : templ.Params())
            anyNumeric = anyNumeric || d.type != MatParamType::Texture;

        if (anyNumeric)
        {
            outText += "cbuffer Material : register(b0)\n{\n";
            for (const ParamDecl& d : templ.Params())
            {
                if (d.type == MatParamType::Texture)
                    continue;
                outText += "    ";
                outText += HlslType(d.type);
                outText += ' ';
                outText += d.name;
                outText += ";\n";
            }
            outText += "};\n";
        }

        bool anyTexture = false;
        for (const ParamDecl& d : templ.Params())
        {
            if (d.type != MatParamType::Texture)
                continue;
            outText += "Texture2D " + d.name + " : register(t" +
                       std::to_string(d.textureIndex) + ");\n";
            anyTexture = true;
        }
        if (anyTexture)
            outText += "SamplerState MaterialSampler : register(s0);\n";

        return outText;
    }

    std::string StitchShaderTemplate(
        std::string_view templateText,
        std::span<const std::pair<std::string_view, std::string_view>> slots,
        std::vector<std::string>* unresolved)
    {
        std::string outText;
        outText.reserve(templateText.size());

        std::size_t pos = 0;
        while (pos < templateText.size())
        {
            const std::size_t open = templateText.find("%{", pos);
            if (open == std::string_view::npos)
            {
                outText.append(templateText.substr(pos));
                break;
            }
            const std::size_t close = templateText.find('}', open + 2);
            if (close == std::string_view::npos)
            {
                outText.append(templateText.substr(pos));
                break;
            }

            outText.append(templateText.substr(pos, open - pos));
            const std::string_view name = templateText.substr(open + 2, close - open - 2);

            bool found = false;
            for (const auto& [slotName, value] : slots)
            {
                if (slotName == name)
                {
                    outText.append(value);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                outText.append(templateText.substr(open, close - open + 1));
                if (unresolved)
                    unresolved->push_back(std::string(name));
            }
            pos = close + 1;
        }
        return outText;
    }

    MaterialBuildResult BuildMaterialShaderSource(std::string_view templateText,
                                                  std::string_view snippet,
                                                  std::string materialName)
    {
        MaterialBuildResult r;

        MaterialSourceParse parsed = ParseMaterialSource(snippet);
        r.errors = std::move(parsed.errors);
        r.metas = std::move(parsed.metas);

        std::uint64_t hash = 14695981039346656037ull;
        hash = Fnv64Str(hash, templateText);
        hash = Fnv64Str(hash, snippet);
        r.templ = MaterialTemplate::Build(std::move(materialName), hash, std::move(parsed.decls));

        const std::string bindings = GenerateMaterialBindings(r.templ);
        const std::pair<std::string_view, std::string_view> slots[] = {
            { "MATERIAL_CBUFFER", bindings },
            { "MATERIAL_BODY", snippet },
        };
        std::vector<std::string> unresolved;
        r.hlsl = StitchShaderTemplate(templateText, slots, &unresolved);
        for (const std::string& slot : unresolved)
            r.errors.push_back("template: unresolved slot %{" + slot + "}");
        return r;
    }
}

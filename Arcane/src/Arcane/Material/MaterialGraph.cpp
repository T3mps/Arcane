#include <Arcane/Material/MaterialGraph.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/MaterialAsset.hpp>   // MatParamValueToJson/FromJson

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Arcane
{
    // ------------------------------------------------------------------ table
    namespace
    {
        // Pin arrays live at namespace scope so the info table's spans stay
        // valid for the process lifetime. PIN ORDER IS APPEND-ONLY (header
        // contract): GraphLink pins index into these arrays.
        constexpr GraphPinDesc kNoPins[]      = { { "", 0 } };   // span'd with count 0
        constexpr GraphPinDesc kOutputIn[]    = { { "color", 4 } };
        constexpr GraphPinDesc kOut1[]        = { { "out", 1 } };
        constexpr GraphPinDesc kOut2[]        = { { "out", 2 } };
        constexpr GraphPinDesc kOut4[]        = { { "out", 4 } };
        constexpr GraphPinDesc kOutDyn[]      = { { "out", 0 } };
        constexpr GraphPinDesc kUvIn[]        = { { "uv", 2 } };
        constexpr GraphPinDesc kSampleOut[]   = { { "rgba", 4 }, { "a", 1 } };
        constexpr GraphPinDesc kBinaryIn[]    = { { "a", 0 }, { "b", 0 } };
        constexpr GraphPinDesc kLerpIn[]      = { { "a", 0 }, { "b", 0 }, { "t", 0 } };
        constexpr GraphPinDesc kUnaryIn[]     = { { "x", 0 } };
        constexpr GraphPinDesc kSplitOut[]    = { { "r", 1 }, { "g", 1 }, { "b", 1 }, { "a", 1 } };

        template <std::size_t N>
        constexpr std::span<const GraphPinDesc> Pins(const GraphPinDesc (&a)[N]) { return { a, N }; }
        constexpr std::span<const GraphPinDesc> NoPins() { return { kNoPins, std::size_t(0) }; }

        const GraphNodeTypeInfo kNodeInfos[] = {
            { GraphNodeType::Output,        "output",         "Output",         Pins(kOutputIn), NoPins()          },
            { GraphNodeType::ConstFloat,    "const_float",    "Float",          NoPins(),        Pins(kOut1)       },
            { GraphNodeType::ConstFloat2,   "const_float2",   "Float2",         NoPins(),        Pins(kOut2)       },
            { GraphNodeType::ConstFloat4,   "const_float4",   "Float4",         NoPins(),        Pins(kOut4)       },
            { GraphNodeType::ConstColor,    "const_color",    "Color",          NoPins(),        Pins(kOut4)       },
            { GraphNodeType::Param,         "param",          "Param",          NoPins(),        Pins(kOutDyn)     },
            { GraphNodeType::TextureSample, "texture_sample", "Texture Sample", Pins(kUvIn),     Pins(kSampleOut)  },
            { GraphNodeType::SpriteTexture, "sprite_texture", "Sprite Texture", Pins(kUvIn),     Pins(kSampleOut)  },
            { GraphNodeType::UV,            "uv",             "UV",             NoPins(),        Pins(kOut2)       },
            { GraphNodeType::Time,          "time",           "Time",           NoPins(),        Pins(kOut1)       },
            { GraphNodeType::VertexColor,   "vertex_color",   "Vertex Color",   NoPins(),        Pins(kOut4)       },
            { GraphNodeType::Add,           "add",            "Add",            Pins(kBinaryIn), Pins(kOutDyn)     },
            { GraphNodeType::Sub,           "sub",            "Subtract",       Pins(kBinaryIn), Pins(kOutDyn)     },
            { GraphNodeType::Mul,           "mul",            "Multiply",       Pins(kBinaryIn), Pins(kOutDyn)     },
            { GraphNodeType::Lerp,          "lerp",           "Lerp",           Pins(kLerpIn),   Pins(kOutDyn)     },
            { GraphNodeType::Sin,           "sin",            "Sine",           Pins(kUnaryIn),  Pins(kOutDyn)     },
            { GraphNodeType::Fraction,      "fraction",       "Fraction",       Pins(kUnaryIn),  Pins(kOutDyn)     },
            { GraphNodeType::Saturate,      "saturate",       "Saturate",       Pins(kUnaryIn),  Pins(kOutDyn)     },
            { GraphNodeType::OneMinus,      "one_minus",      "One Minus",      Pins(kUnaryIn),  Pins(kOutDyn)     },
            { GraphNodeType::Split,         "split",          "Split",          Pins(kUnaryIn),  Pins(kSplitOut)   },
        };
    }

    const GraphNodeTypeInfo& GraphNodeInfo(GraphNodeType t) noexcept
    {
        const auto i = static_cast<std::size_t>(t);
        static_assert(std::size(kNodeInfos) == static_cast<std::size_t>(GraphNodeType::Split) + 1,
                      "kNodeInfos must cover every GraphNodeType");
        return kNodeInfos[i < std::size(kNodeInfos) ? i : 0];
    }

    std::span<const GraphNodeTypeInfo> AllGraphNodeInfos() noexcept
    {
        return { kNodeInfos, std::size(kNodeInfos) };
    }

    bool GraphNodeTypeFromToken(std::string_view token, GraphNodeType& out) noexcept
    {
        for (const GraphNodeTypeInfo& info : kNodeInfos)
            if (token == info.token)
            {
                out = info.type;
                return true;
            }
        return false;
    }

    // ---------------------------------------------------------------- codegen
    namespace
    {
        // Keep in lockstep with MaterialSource.cpp kReservedNames (the parse of
        // the generated snippet is the enforcement backstop; this check exists
        // for node-accurate messages).
        constexpr std::string_view kGraphReservedNames[] = {
            "Time", "DeltaTime", "ViewportSize", "MaterialSampler", "SpriteTexture",
        };

        bool ValidParamName(std::string_view name)
        {
            if (name.empty())
                return false;
            // Leading underscore is reserved for codegen locals (_n<id>).
            const char c0 = name[0];
            if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')))
                return false;
            for (char c : name)
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '_'))
                    return false;
            return true;
        }

        std::string FormatF(float v)
        {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%g", v);
            return buf;
        }

        const char* HlslTypeForWidth(int w)
        {
            switch (w)
            {
                case 1: return "float";
                case 2: return "float2";
                default: return "float4";
            }
        }

        // SG's wire-adaptation table verbatim (restricted to our 1/2/4 widths):
        // equal as-is; scalar splats; float2->float4 appends 0,1; wider->
        // narrower takes the leading swizzle.
        std::string Adapt(const std::string& expr, int from, int to)
        {
            if (from == to)
                return expr;
            if (from == 1)
                return "(" + expr + (to == 2 ? ").xx" : ").xxxx");
            if (from == 2 && to == 4)
                return "float4(" + expr + ", 0.0, 1.0)";
            if (to == 2)
                return "(" + expr + ").xy";
            return "(" + expr + ").x";
        }

        struct DeclInfo
        {
            MatParamType  type = MatParamType::Float;
            MatParamValue def;
            bool          hasRange = false;
            float         rangeMin = 0.0f, rangeMax = 1.0f;
            std::uint32_t firstNode = 0;
        };
    }

    GraphCodegenResult GenerateGraphSnippet(const MaterialGraph& graph, MaterialSurface surface)
    {
        GraphCodegenResult res;
        auto fail = [&](std::uint32_t node, std::string msg)
        { res.errors.push_back({ node, std::move(msg) }); };

        // --- index nodes; ids must be unique and non-zero
        std::unordered_map<std::uint32_t, const GraphNode*> byId;
        byId.reserve(graph.nodes.size());
        for (const GraphNode& n : graph.nodes)
        {
            if (n.id == 0)
            {
                fail(0, "a node has id 0 (ids must be positive)");
                continue;
            }
            if (!byId.emplace(n.id, &n).second)
                fail(n.id, "duplicate node id");
        }

        // Deterministic node-id order for decl collection and output lookup.
        std::vector<const GraphNode*> ordered;
        ordered.reserve(byId.size());
        for (const GraphNode& n : graph.nodes)
            if (byId.count(n.id) && byId.at(n.id) == &n)
                ordered.push_back(&n);
        std::sort(ordered.begin(), ordered.end(),
                  [](const GraphNode* a, const GraphNode* b) { return a->id < b->id; });

        // --- validate links; build the input map (one edge per input; last wins
        // -- the canvas enforces silent-replace, codegen just mirrors it)
        std::map<std::pair<std::uint32_t, std::uint32_t>, const GraphLink*> inputLink;
        for (const GraphLink& l : graph.links)
        {
            const auto fromIt = byId.find(l.fromNode);
            const auto toIt = byId.find(l.toNode);
            if (fromIt == byId.end() || toIt == byId.end())
            {
                fail(0, "link references a missing node (" + std::to_string(l.fromNode) +
                        " -> " + std::to_string(l.toNode) + ")");
                continue;
            }
            if (l.fromPin >= GraphNodeInfo(fromIt->second->type).outputs.size())
            {
                fail(l.fromNode, "link uses an unknown output pin");
                continue;
            }
            if (l.toPin >= GraphNodeInfo(toIt->second->type).inputs.size())
            {
                fail(l.toNode, "link uses an unknown input pin");
                continue;
            }
            inputLink[{ l.toNode, l.toPin }] = &l;
        }

        // --- exactly one Output
        const GraphNode* output = nullptr;
        for (const GraphNode* n : ordered)
            if (n->type == GraphNodeType::Output)
            {
                if (output)
                    fail(n->id, "graph has more than one Output node");
                else
                    output = n;
            }
        if (!output)
            fail(0, "graph has no Output node");

        // --- param declarations over ALL nodes (reachable or not -- the params
        // panel must match the canvas). First-declaring node id wins the
        // default value (template dup rule); conflicting TYPES are an error on
        // both nodes.
        std::vector<std::pair<std::string, DeclInfo>> decls;
        auto findDecl = [&](std::string_view name) -> DeclInfo*
        {
            for (auto& [n, d] : decls)
                if (n == name)
                    return &d;
            return nullptr;
        };
        for (const GraphNode* n : ordered)
        {
            if (n->type != GraphNodeType::Param && n->type != GraphNodeType::TextureSample)
                continue;
            if (!ValidParamName(n->paramName))
            {
                fail(n->id, "param name '" + n->paramName +
                            "' is not a valid identifier (letters first, no leading underscore)");
                continue;
            }
            bool reserved = false;
            for (std::string_view r : kGraphReservedNames)
                if (n->paramName == r)
                    reserved = true;
            if (reserved)
            {
                fail(n->id, "param name '" + n->paramName + "' is reserved by the template");
                continue;
            }
            const MatParamType type =
                n->type == GraphNodeType::TextureSample ? MatParamType::Texture : n->paramType;
            if (n->type == GraphNodeType::Param && n->paramType == MatParamType::Texture)
            {
                fail(n->id, "Param nodes cannot declare textures -- use a Texture Sample node");
                continue;
            }
            if (DeclInfo* existing = findDecl(n->paramName))
            {
                if (existing->type != type)
                {
                    fail(n->id, "param '" + n->paramName + "' redeclared with a different type");
                    fail(existing->firstNode, "param '" + n->paramName + "' redeclared with a different type");
                }
                continue;   // first-wins on default/range
            }
            DeclInfo d;
            d.type = type;
            d.def = n->paramDefault;
            d.hasRange = n->hasRange;
            d.rangeMin = n->rangeMin;
            d.rangeMax = n->rangeMax;
            d.firstNode = n->id;
            decls.emplace_back(n->paramName, d);
        }

        // --- surface gating
        if (surface != MaterialSurface::Sprite)
            for (const GraphNode* n : ordered)
                if (n->type == GraphNodeType::VertexColor || n->type == GraphNodeType::SpriteTexture)
                    fail(n->id, std::string(GraphNodeInfo(n->type).display) +
                                " requires the sprite surface");

        if (!res.errors.empty())
            return res;

        // --- which output pins are consumed anywhere (multi-output nodes emit
        // secondary-pin locals only when used; a consumer being unreachable
        // just leaves a dead local for HLSL to prune)
        std::unordered_set<std::uint64_t> consumed;
        for (const auto& [key, link] : inputLink)
            consumed.insert((std::uint64_t(link->fromNode) << 32) | link->fromPin);
        auto pinConsumed = [&](std::uint32_t node, std::uint32_t pin)
        { return consumed.count((std::uint64_t(node) << 32) | pin) != 0; };

        // --- DFS from Output: resolve widths, emit SSA statements post-order
        std::unordered_map<std::uint32_t, int> state;    // 0 fresh / 1 on-stack / 2 done
        std::unordered_map<std::uint32_t, int> widthOf;  // resolved primary width
        std::vector<std::pair<std::string, std::uint32_t>> body;   // statement, nodeId

        // Expression for a node's output pin AFTER the node was visited.
        auto pinExpr = [&](const GraphNode* n, std::uint32_t pin, int& outWidth) -> std::string
        {
            const auto& outs = GraphNodeInfo(n->type).outputs;
            const int declared = outs[pin].width;
            outWidth = declared == 0 ? widthOf[n->id] : declared;
            if (outs.size() == 1)
                return "_n" + std::to_string(n->id);
            return "_n" + std::to_string(n->id) + "_" + outs[pin].name;
        };

        std::function<bool(const GraphNode*)> visit = [&](const GraphNode* n) -> bool
        {
            int& st = state[n->id];
            if (st == 2)
                return true;
            if (st == 1)
            {
                fail(n->id, "the graph has a cycle through this node");
                return false;
            }
            st = 1;

            const GraphNodeTypeInfo& info = GraphNodeInfo(n->type);

            // Visit children; collect per-input (expression, width) with
            // unconnected defaults.
            struct In { std::string expr; int width = 1; bool connected = false; };
            std::vector<In> in(info.inputs.size());
            for (std::uint32_t pin = 0; pin < info.inputs.size(); ++pin)
            {
                const auto it = inputLink.find({ n->id, pin });
                if (it == inputLink.end())
                    continue;
                const GraphNode* src = byId.at(it->second->fromNode);
                if (!visit(src))
                    return false;
                in[pin].connected = true;
                in[pin].expr = pinExpr(src, it->second->fromPin, in[pin].width);
            }

            // Resolved width: SG rule -- minimum connected non-scalar dynamic
            // input, else 1 (scalars splat, never pinning the width).
            bool dynamic = false;
            for (const GraphPinDesc& p : info.inputs)
                dynamic = dynamic || p.width == 0;
            int w = 0;
            if (dynamic)
            {
                for (std::uint32_t pin = 0; pin < info.inputs.size(); ++pin)
                    if (info.inputs[pin].width == 0 && in[pin].connected && in[pin].width > 1)
                        w = w == 0 ? in[pin].width : std::min(w, in[pin].width);
                if (w == 0)
                    w = 1;
            }
            widthOf[n->id] = w;

            // Adapted expression for input `pin` at target width `t` (0 = the
            // node's resolved dynamic width). Unconnected numeric inputs read 0.
            auto arg = [&](std::uint32_t pin, int t) -> std::string
            {
                if (t == 0)
                    t = w;
                if (!in[pin].connected)
                    return Adapt("0.0", 1, t);
                return Adapt(in[pin].expr, in[pin].width, t);
            };

            const std::string id = std::to_string(n->id);
            auto stmt = [&](std::string s) { body.emplace_back(std::move(s), n->id); };
            auto local = [&](int lw, const std::string& expr)
            { stmt(std::string(HlslTypeForWidth(lw)) + " _n" + id + " = " + expr + ";"); };

            switch (n->type)
            {
                case GraphNodeType::Output:
                    stmt("return " + (in[0].connected
                                          ? Adapt(in[0].expr, in[0].width, 4)
                                          : std::string("float4(0.0, 0.0, 0.0, 1.0)")) + ";");
                    break;
                case GraphNodeType::ConstFloat:
                    local(1, FormatF(n->value[0]));
                    break;
                case GraphNodeType::ConstFloat2:
                    local(2, "float2(" + FormatF(n->value[0]) + ", " + FormatF(n->value[1]) + ")");
                    break;
                case GraphNodeType::ConstFloat4:
                case GraphNodeType::ConstColor:
                    local(4, "float4(" + FormatF(n->value[0]) + ", " + FormatF(n->value[1]) + ", " +
                             FormatF(n->value[2]) + ", " + FormatF(n->value[3]) + ")");
                    break;
                case GraphNodeType::Param:
                {
                    const int pw = static_cast<int>(ComponentCount(n->paramType));
                    widthOf[n->id] = pw;
                    local(pw, n->paramName);
                    break;
                }
                case GraphNodeType::TextureSample:
                case GraphNodeType::SpriteTexture:
                {
                    const std::string tex =
                        n->type == GraphNodeType::SpriteTexture ? "SpriteTexture" : n->paramName;
                    const std::string uv = in[0].connected ? Adapt(in[0].expr, in[0].width, 2)
                                                           : std::string("v.uv");
                    stmt("float4 _n" + id + "_rgba = " + tex + ".Sample(MaterialSampler, " + uv + ");");
                    if (pinConsumed(n->id, 1))
                        stmt("float _n" + id + "_a = _n" + id + "_rgba.a;");
                    break;
                }
                case GraphNodeType::UV:
                    local(2, "v.uv");
                    break;
                case GraphNodeType::Time:
                    local(1, "Time");
                    break;
                case GraphNodeType::VertexColor:
                    local(4, "v.color");
                    break;
                case GraphNodeType::Add:
                    local(w, arg(0, 0) + " + " + arg(1, 0));
                    break;
                case GraphNodeType::Sub:
                    local(w, arg(0, 0) + " - " + arg(1, 0));
                    break;
                case GraphNodeType::Mul:
                    local(w, arg(0, 0) + " * " + arg(1, 0));
                    break;
                case GraphNodeType::Lerp:
                    local(w, "lerp(" + arg(0, 0) + ", " + arg(1, 0) + ", " + arg(2, 0) + ")");
                    break;
                case GraphNodeType::Sin:
                    local(w, "sin(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::Fraction:
                    local(w, "frac(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::Saturate:
                    local(w, "saturate(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::OneMinus:
                    local(w, "1.0 - " + arg(0, 0));
                    break;
                case GraphNodeType::Split:
                {
                    // SG Split rule: lanes beyond the source width read 0. The
                    // input is read at its NATIVE width (no adaptation).
                    const int sw = in[0].connected ? in[0].width : 1;
                    const std::string src = in[0].connected ? in[0].expr : std::string("0.0");
                    const char* lane[4] = { ".x", ".y", ".z", ".w" };
                    const auto& outs = GraphNodeInfo(n->type).outputs;
                    for (std::uint32_t pin = 0; pin < outs.size(); ++pin)
                    {
                        if (!pinConsumed(n->id, pin))
                            continue;
                        const bool present = static_cast<int>(pin) < sw;
                        // A scalar source has only .x; lane 0 of a scalar is the
                        // value itself.
                        std::string expr = !present ? std::string("0.0")
                                          : sw == 1 ? "(" + src + ")"
                                                     : "(" + src + ")" + lane[pin];
                        stmt("float _n" + id + "_" + outs[pin].name + " = " + expr + ";");
                    }
                    break;
                }
            }

            st = 2;
            return true;
        };

        if (!visit(output) || !res.errors.empty())
        {
            res.snippet.clear();
            res.lineNodeIds.clear();
            return res;
        }

        // --- assemble: //@param block + blank + shade() wrapper, with the
        // per-line node attribution the compiler-diag badge mapping needs.
        std::vector<std::pair<std::string, std::uint32_t>> lines;
        for (const auto& [name, d] : decls)
        {
            std::string line = "//@param ";
            switch (d.type)
            {
                case MatParamType::Float:
                    line += "float " + name + " = " + FormatF(d.def.f[0]);
                    break;
                case MatParamType::Float2:
                    line += "float2 " + name + " = (" + FormatF(d.def.f[0]) + ", " +
                            FormatF(d.def.f[1]) + ")";
                    break;
                case MatParamType::Float4:
                case MatParamType::Color:
                    line += std::string(d.type == MatParamType::Color ? "color " : "float4 ") +
                            name + " = (" + FormatF(d.def.f[0]) + ", " + FormatF(d.def.f[1]) +
                            ", " + FormatF(d.def.f[2]) + ", " + FormatF(d.def.f[3]) + ")";
                    break;
                case MatParamType::Texture:
                    line += "texture " + name;
                    break;
            }
            if (d.hasRange && d.type != MatParamType::Texture)
                line += " [" + FormatF(d.rangeMin) + ".." + FormatF(d.rangeMax) + "]";
            lines.emplace_back(std::move(line), d.firstNode);
        }
        if (!lines.empty())
            lines.emplace_back("", 0);
        lines.emplace_back("float4 shade(Varyings v)", 0);
        lines.emplace_back("{", 0);
        for (auto& [text, nodeId] : body)
            lines.emplace_back("    " + text, nodeId);
        lines.emplace_back("}", 0);

        res.snippet.reserve(lines.size() * 40);
        res.lineNodeIds.reserve(lines.size());
        for (auto& [text, nodeId] : lines)
        {
            res.snippet += text;
            res.snippet += '\n';
            res.lineNodeIds.push_back(nodeId);
        }
        return res;
    }

    // ------------------------------------------------------------------- json
    nlohmann::json GraphToJson(const MaterialGraph& graph)
    {
        nlohmann::json j;
        j["nextId"] = graph.nextId;

        std::vector<const GraphNode*> ordered;
        ordered.reserve(graph.nodes.size());
        for (const GraphNode& n : graph.nodes)
            ordered.push_back(&n);
        std::sort(ordered.begin(), ordered.end(),
                  [](const GraphNode* a, const GraphNode* b) { return a->id < b->id; });

        nlohmann::json nodes = nlohmann::json::array();
        for (const GraphNode* n : ordered)
        {
            nlohmann::json e;
            e["id"] = n->id;
            e["type"] = GraphNodeInfo(n->type).token;
            e["pos"] = nlohmann::json::array({ n->posX, n->posY });
            switch (n->type)
            {
                case GraphNodeType::ConstFloat:
                    e["value"] = n->value[0];
                    break;
                case GraphNodeType::ConstFloat2:
                    e["value"] = nlohmann::json::array({ n->value[0], n->value[1] });
                    break;
                case GraphNodeType::ConstFloat4:
                case GraphNodeType::ConstColor:
                    e["value"] = nlohmann::json::array(
                        { n->value[0], n->value[1], n->value[2], n->value[3] });
                    break;
                case GraphNodeType::Param:
                {
                    nlohmann::json p;
                    p["name"] = n->paramName;
                    MatParamValue def = n->paramDefault;
                    def.type = n->paramType;   // decl type is authoritative on disk
                    p["decl"] = MatParamValueToJson(def);
                    if (n->hasRange)
                        p["range"] = nlohmann::json::array({ n->rangeMin, n->rangeMax });
                    e["param"] = std::move(p);
                    break;
                }
                case GraphNodeType::TextureSample:
                    e["param"] = nlohmann::json{ { "name", n->paramName } };
                    break;
                default:
                    break;
            }
            nodes.push_back(std::move(e));
        }
        j["nodes"] = std::move(nodes);

        std::vector<const GraphLink*> links;
        links.reserve(graph.links.size());
        for (const GraphLink& l : graph.links)
            links.push_back(&l);
        std::sort(links.begin(), links.end(), [](const GraphLink* a, const GraphLink* b)
        {
            if (a->toNode != b->toNode) return a->toNode < b->toNode;
            if (a->toPin != b->toPin) return a->toPin < b->toPin;
            if (a->fromNode != b->fromNode) return a->fromNode < b->fromNode;
            return a->fromPin < b->fromPin;
        });
        nlohmann::json jlinks = nlohmann::json::array();
        for (const GraphLink* l : links)
            jlinks.push_back(nlohmann::json{ { "from", l->fromNode },
                                             { "fromPin", l->fromPin },
                                             { "to", l->toNode },
                                             { "toPin", l->toPin } });
        j["links"] = std::move(jlinks);
        return j;
    }

    std::optional<MaterialGraph> GraphFromJson(const nlohmann::json& j)
    {
        if (!j.is_object() || !j.contains("nodes") || !j["nodes"].is_array())
            return std::nullopt;

        MaterialGraph g;
        std::unordered_set<std::uint32_t> ids;
        for (const nlohmann::json& e : j["nodes"])
        {
            if (!e.is_object() || !e.contains("id") || !e["id"].is_number_unsigned() ||
                !e.contains("type") || !e["type"].is_string())
                return std::nullopt;
            GraphNode n;
            n.id = e["id"].get<std::uint32_t>();
            if (n.id == 0 || !ids.insert(n.id).second)
                return std::nullopt;
            if (!GraphNodeTypeFromToken(e["type"].get<std::string>(), n.type))
                return std::nullopt;   // no unknown-node stand-ins yet: refuse, keep the file intact
            if (e.contains("pos") && e["pos"].is_array() && e["pos"].size() == 2 &&
                e["pos"][0].is_number() && e["pos"][1].is_number())
            {
                n.posX = e["pos"][0].get<float>();
                n.posY = e["pos"][1].get<float>();
            }
            if (e.contains("value"))
            {
                const nlohmann::json& v = e["value"];
                if (v.is_number())
                    n.value[0] = v.get<float>();
                else if (v.is_array())
                    for (std::size_t i = 0; i < std::min<std::size_t>(4, v.size()); ++i)
                        if (v[i].is_number())
                            n.value[i] = v[i].get<float>();
            }
            if (e.contains("param") && e["param"].is_object())
            {
                const nlohmann::json& p = e["param"];
                if (p.contains("name") && p["name"].is_string())
                    n.paramName = p["name"].get<std::string>();
                if (p.contains("decl"))
                    if (auto def = MatParamValueFromJson(p["decl"]))
                    {
                        n.paramDefault = *def;
                        n.paramType = def->type;
                    }
                if (p.contains("range") && p["range"].is_array() && p["range"].size() == 2 &&
                    p["range"][0].is_number() && p["range"][1].is_number())
                {
                    n.hasRange = true;
                    n.rangeMin = p["range"][0].get<float>();
                    n.rangeMax = p["range"][1].get<float>();
                }
            }
            if (n.type == GraphNodeType::TextureSample)
                n.paramType = MatParamType::Texture;
            g.nodes.push_back(std::move(n));
        }

        if (j.contains("links") && j["links"].is_array())
        {
            for (const nlohmann::json& e : j["links"])
            {
                if (!e.is_object())
                    continue;
                auto u32 = [&](const char* key, std::uint32_t& out)
                {
                    if (!e.contains(key) || !e[key].is_number_unsigned())
                        return false;
                    out = e[key].get<std::uint32_t>();
                    return true;
                };
                GraphLink l;
                if (!u32("from", l.fromNode) || !u32("to", l.toNode))
                    continue;
                u32("fromPin", l.fromPin);
                u32("toPin", l.toPin);
                // SG-parity dangling-edge cleanup: links to missing nodes or
                // out-of-range pins are dropped silently at load.
                if (!ids.count(l.fromNode) || !ids.count(l.toNode))
                    continue;
                const GraphNode* from = g.FindNode(l.fromNode);
                const GraphNode* to = g.FindNode(l.toNode);
                if (l.fromPin >= GraphNodeInfo(from->type).outputs.size() ||
                    l.toPin >= GraphNodeInfo(to->type).inputs.size())
                    continue;
                g.links.push_back(l);
            }
        }

        std::uint32_t maxId = 0;
        for (const GraphNode& n : g.nodes)
            maxId = std::max(maxId, n.id);
        g.nextId = std::max<std::uint32_t>(maxId + 1,
            j.contains("nextId") && j["nextId"].is_number_unsigned()
                ? j["nextId"].get<std::uint32_t>() : 1);
        return g;
    }
}

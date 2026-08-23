#include <Arcane/Material/MaterialGraph.hpp>

#include <Arcane/Base/Assert.hpp>   // ARC_ENSURE: GenerateGraphSnippet's Mesh-surface guard
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
        constexpr GraphPinDesc kCombineIn[]   = { { "r", 1 }, { "g", 1 }, { "b", 1 }, { "a", 1 } };
        constexpr GraphPinDesc kClampIn[]     = { { "x", 0 }, { "min", 0 }, { "max", 0 } };
        constexpr GraphPinDesc kSmoothIn[]    = { { "edge0", 0 }, { "edge1", 0 }, { "x", 0 } };
        constexpr GraphPinDesc kStepIn[]      = { { "edge", 0 }, { "x", 0 } };
        constexpr GraphPinDesc kRemapIn[]     = { { "x", 0 }, { "inRange", 2 }, { "outRange", 2 } };
        constexpr GraphPinDesc kTileIn[]      = { { "uv", 2 }, { "tiling", 2 }, { "offset", 2 } };
        constexpr GraphPinDesc kNoiseIn[]     = { { "uv", 2 }, { "scale", 1 } };
        constexpr GraphPinDesc kPannerIn[]    = { { "uv", 2 }, { "speed", 2 } };
        // bias/scale are FIXED width 1 beside a DYNAMIC x -- the SimpleNoise
        // row's { uv, scale } shape (fixed operand next to the value pin), so
        // an affine pair collapses into one node carrying two pin literals.
        constexpr GraphPinDesc kBiasScaleIn[] = { { "x", 0 }, { "bias", 1 }, { "scale", 1 } };
        constexpr GraphPinDesc kVertexOutIn[] = { { "posOffset", 2 }, { "uvOffset", 2 },
                                                  { "color", 4 } };

        template <std::size_t N>
        constexpr std::span<const GraphPinDesc> Pins(const GraphPinDesc (&a)[N]) { return { a, N }; }
        constexpr std::span<const GraphPinDesc> NoPins() { return { kNoPins, std::size_t(0) }; }

        // Row-local sugar for the last column only -- spelling
        // GraphNodeCategory in 49 rows buys nothing that Cat:: does not, while
        // GraphNodeType stays written out so the enum remains greppable from
        // the table. Each row wraps onto two lines because the sixth column
        // does not fit in one (the file's next-longest line is ~100 chars).
        using Cat = GraphNodeCategory;

        const GraphNodeTypeInfo kNodeInfos[] = {
            { GraphNodeType::Output,        "output",         "Output",
              Pins(kOutputIn),    NoPins(),         Cat::Output },
            { GraphNodeType::ConstFloat,    "const_float",    "Float",
              NoPins(),           Pins(kOut1),      Cat::Input },
            { GraphNodeType::ConstFloat2,   "const_float2",   "Float2",
              NoPins(),           Pins(kOut2),      Cat::Input },
            { GraphNodeType::ConstFloat4,   "const_float4",   "Float4",
              NoPins(),           Pins(kOut4),      Cat::Input },
            { GraphNodeType::ConstColor,    "const_color",    "Color",
              NoPins(),           Pins(kOut4),      Cat::Input },
            { GraphNodeType::Param,         "param",          "Param",
              NoPins(),           Pins(kOutDyn),    Cat::Input },
            { GraphNodeType::TextureSample, "texture_sample", "Texture Sample",
              Pins(kUvIn),        Pins(kSampleOut), Cat::Input },
            { GraphNodeType::SpriteTexture, "sprite_texture", "Sprite Texture",
              Pins(kUvIn),        Pins(kSampleOut), Cat::Input },
            { GraphNodeType::UV,            "uv",             "UV",
              NoPins(),           Pins(kOut2),      Cat::Input },
            { GraphNodeType::Time,          "time",           "Time",
              NoPins(),           Pins(kOut1),      Cat::Input },
            { GraphNodeType::VertexColor,   "vertex_color",   "Vertex Color",
              NoPins(),           Pins(kOut4),      Cat::Input },
            { GraphNodeType::Add,           "add",            "Add",
              Pins(kBinaryIn),    Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Sub,           "sub",            "Subtract",
              Pins(kBinaryIn),    Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Mul,           "mul",            "Multiply",
              Pins(kBinaryIn),    Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Lerp,          "lerp",           "Lerp",
              Pins(kLerpIn),      Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Sin,           "sin",            "Sine",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Fraction,      "fraction",       "Fraction",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Saturate,      "saturate",       "Saturate",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::OneMinus,      "one_minus",      "One Minus",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Split,         "split",          "Split",
              Pins(kUnaryIn),     Pins(kSplitOut),  Cat::Vector },
            // Custom's pins are PER-NODE data -- the table spans stay empty and
            // every pin query routes through GraphNodeInput/OutputPin below.
            { GraphNodeType::Custom,        "custom",         "Custom (HLSL)",
              NoPins(),           NoPins(),         Cat::Utility },
            { GraphNodeType::Combine,       "combine",        "Combine",
              Pins(kCombineIn),   Pins(kOut4),      Cat::Vector },
            { GraphNodeType::Clamp,         "clamp",          "Clamp",
              Pins(kClampIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Smoothstep,    "smoothstep",     "Smoothstep",
              Pins(kSmoothIn),    Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Step,          "step",           "Step",
              Pins(kStepIn),      Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Power,         "power",          "Power",
              Pins(kBinaryIn),    Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Remap,         "remap",          "Remap",
              Pins(kRemapIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::TilingOffset,  "tiling_offset",  "Tiling & Offset",
              Pins(kTileIn),      Pins(kOut2),      Cat::Procedural },
            { GraphNodeType::Cos,           "cos",            "Cosine",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Abs,           "abs",            "Absolute",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Min,           "min",            "Minimum",
              Pins(kBinaryIn),    Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Max,           "max",            "Maximum",
              Pins(kBinaryIn),    Pins(kOutDyn),    Cat::Math },
            // Swizzle's OUTPUT width is per-node data (the mask length) -- the
            // dynamic 0 here resolves through widthOf, which the emission case
            // pins to the mask (the Param pattern).
            { GraphNodeType::Swizzle,       "swizzle",        "Swizzle",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Vector },
            { GraphNodeType::SimpleNoise,   "simple_noise",   "Simple Noise",
              Pins(kNoiseIn),     Pins(kOut1),      Cat::Procedural },
            { GraphNodeType::PassInput,     "pass_input",     "Pass Input",
              Pins(kUvIn),        Pins(kSampleOut), Cat::Input },
            { GraphNodeType::VertexOutput,  "vertex_output",  "Vertex Output",
              Pins(kVertexOutIn), NoPins(),         Cat::Output },
            { GraphNodeType::Comment,       "comment",        "Comment",
              NoPins(),           NoPins(),         Cat::Utility },
            // Library growth batch 2 -- appended in the header's enum order.
            { GraphNodeType::Exp,           "exp",            "Exponential",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Negate,        "negate",         "Negate",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Floor,         "floor",          "Floor",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Ceil,          "ceil",           "Ceiling",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Round,         "round",          "Round",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Sign,          "sign",           "Sign",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Math },
            { GraphNodeType::Normalize,     "normalize",      "Normalize",
              Pins(kUnaryIn),     Pins(kOutDyn),    Cat::Vector },
            // Fixed width-1 OUTPUT -- the SimpleNoise row above is the
            // fixed-out-1 precedent (its INPUTS are fixed-width, unlike
            // these): the emission case adapts the operands to the node's
            // resolved width, then the intrinsic collapses to one float.
            { GraphNodeType::Length,        "length",         "Length",
              Pins(kUnaryIn),     Pins(kOut1),      Cat::Vector },
            { GraphNodeType::Distance,      "distance",       "Distance",
              Pins(kBinaryIn),    Pins(kOut1),      Cat::Vector },
            { GraphNodeType::Dot,           "dot",            "Dot Product",
              Pins(kBinaryIn),    Pins(kOut1),      Cat::Vector },
            { GraphNodeType::Panner,        "panner",         "Panner",
              Pins(kPannerIn),    Pins(kOut2),      Cat::Procedural },
            // Gap-close -- appended in the header's enum order.
            { GraphNodeType::ScaleOffset,   "scale_offset",   "Scale & Offset",
              Pins(kBiasScaleIn), Pins(kOutDyn),    Cat::Math },
        };
    }

    const GraphNodeTypeInfo& GraphNodeInfo(GraphNodeType t) noexcept
    {
        const auto i = static_cast<std::size_t>(t);
        static_assert(std::size(kNodeInfos) == static_cast<std::size_t>(GraphNodeType::ScaleOffset) + 1,
                      "kNodeInfos must cover every GraphNodeType");
        return kNodeInfos[i < std::size(kNodeInfos) ? i : 0];
    }

    std::uint32_t GraphNodeInputCount(const GraphNode& n) noexcept
    {
        if (n.type == GraphNodeType::Custom)
            return static_cast<std::uint32_t>(n.customPins.size());
        return static_cast<std::uint32_t>(GraphNodeInfo(n.type).inputs.size());
    }

    std::uint32_t GraphNodeOutputCount(const GraphNode& n) noexcept
    {
        if (n.type == GraphNodeType::Custom)
            return 1;
        return static_cast<std::uint32_t>(GraphNodeInfo(n.type).outputs.size());
    }

    GraphPinDesc GraphNodeInputPin(const GraphNode& n, std::uint32_t pin) noexcept
    {
        if (n.type == GraphNodeType::Custom)
        {
            if (pin < n.customPins.size())
                return { n.customPins[pin].name.c_str(), n.customPins[pin].width };
            return { "", 1 };
        }
        const auto& pins = GraphNodeInfo(n.type).inputs;
        return pin < pins.size() ? pins[pin] : GraphPinDesc{ "", 0 };
    }

    GraphPinDesc GraphNodeOutputPin(const GraphNode& n, std::uint32_t pin) noexcept
    {
        if (n.type == GraphNodeType::Custom)
            return { "out", n.customOutWidth };
        const auto& pins = GraphNodeInfo(n.type).outputs;
        return pin < pins.size() ? pins[pin] : GraphPinDesc{ "", 0 };
    }

    std::span<const GraphNodeTypeInfo> AllGraphNodeInfos() noexcept
    {
        return { kNodeInfos, std::size(kNodeInfos) };
    }

    const char* GraphNodeCategoryName(GraphNodeCategory c) noexcept
    {
        // Menu headings. Lives here, beside the column it names, so a new
        // category is one enum value + one row here + the rows that use it.
        switch (c)
        {
            case GraphNodeCategory::Uncategorized: return "Uncategorized";
            case GraphNodeCategory::Input:         return "Input";
            case GraphNodeCategory::Math:          return "Math";
            case GraphNodeCategory::Vector:        return "Vector";
            case GraphNodeCategory::Procedural:    return "Procedural";
            case GraphNodeCategory::Output:        return "Output";
            case GraphNodeCategory::Utility:       return "Utility";
        }
        return "Uncategorized";   // a cast-in value, not an enumerator: name it
                                  // for what it is rather than guessing a bucket
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
            // Pass chains: upstream pass outputs (slot k of the pass's inputs).
            "InputTexture", "InputTexture1", "InputTexture2", "InputTexture3",
            "displace",   // the vertex-stage hook function
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

        // A pin literal as an HLSL constant, formatted exactly like the Const*
        // node cases emit theirs, so an inline literal and a Const node wired
        // into the same pin produce identical text. `lanes` comes from
        // GraphPinLiteralLanes.
        std::string PinLiteralExpr(const GraphPinLiteral& lit, int lanes)
        {
            if (lanes == 2)
                return "float2(" + FormatF(lit.v[0]) + ", " + FormatF(lit.v[1]) + ")";
            if (lanes == 4)
                return "float4(" + FormatF(lit.v[0]) + ", " + FormatF(lit.v[1]) + ", " +
                       FormatF(lit.v[2]) + ", " + FormatF(lit.v[3]) + ")";
            return FormatF(lit.v[0]);
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

        // Shared helper functions, emitted ONCE above shade() when any node
        // needs them (the emit-once registry). `_g_` prefix: reserved
        // leading-underscore space, distinct from `_n` locals and `_cf`
        // Custom-node functions.
        constexpr const char* kSimpleNoiseHelper[] = {
            "float _g_hash21(float2 p)",
            "{",
            "    p = frac(p * float2(123.34, 456.21));",
            "    p += dot(p, p + 45.32);",
            "    return frac(p.x * p.y);",
            "}",
            "float _g_simple_noise(float2 uv)",
            "{",
            "    float2 i = floor(uv);",
            "    float2 f = frac(uv);",
            "    float2 u = f * f * (3.0 - 2.0 * f);",
            "    float a = _g_hash21(i);",
            "    float b = _g_hash21(i + float2(1.0, 0.0));",
            "    float c = _g_hash21(i + float2(0.0, 1.0));",
            "    float d = _g_hash21(i + float2(1.0, 1.0));",
            "    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);",
            "}",
        };

        int SwizzleLane(char c)
        {
            return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : -1;
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

    GraphCodegenResult GenerateGraphSnippet(const MaterialGraph& graph, MaterialSurface surface,
                                            std::uint32_t availableInputs, bool passGraph)
    {
        GraphCodegenResult res;
        auto fail = [&](std::uint32_t node, std::string msg)
        { res.errors.push_back({ node, std::move(msg) }); };

        // THE THIRD SILENT-FALLBACK SITE, guarded the same way as the other
        // two (MaterialTemplateFile / GenerateMaterialBindings, Material/
        // MaterialSource.cpp): every `surface` read in this function -- the
        // vertex-colour pin gate at "the color pin requires the sprite
        // surface" and the VertexColor/SpriteTexture gate under "surface
        // gating", the only two -- tests `!= MaterialSurface::Sprite`, so
        // MaterialSurface::Mesh silently takes the FULLSCREEN path and this
        // function emits a fullscreen snippet for a surface that has no
        // template to stitch it into.
        //
        // REACHABLE, not theoretical: LoadMaterialAsset's graph self-heal
        // calls GenerateGraphSnippet(*data.graph, MaterialSurfaceForKind(
        // data.kind)) (MaterialAsset.cpp), so a hand-authored graph-only
        // "mesh"-kind .arcmat reaches here today. One guard at the top rather
        // than one per site: ARC_ENSURE dedups per CALL SITE, so two would
        // mean two log lines for one call, and the whole function's output is
        // surface-dependent -- there is no partial answer that is right.
        // Closes when F2b/Task 8 gives Mesh a real template and a register map.
        ARC_ENSURE(surface != MaterialSurface::Mesh,
                   "GenerateGraphSnippet: MaterialSurface::Mesh has no graph codegen yet "
                   "(F2b/Task 8) -- generating against the fullscreen surface");

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
            if (l.fromPin >= GraphNodeOutputCount(*fromIt->second))
            {
                fail(l.fromNode, "link uses an unknown output pin");
                continue;
            }
            if (l.toPin >= GraphNodeInputCount(*toIt->second))
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

        // --- at most one Vertex Output (the vertex context; base graphs only)
        const GraphNode* vertexOut = nullptr;
        for (const GraphNode* n : ordered)
            if (n->type == GraphNodeType::VertexOutput)
            {
                if (vertexOut)
                    fail(n->id, "graph has more than one Vertex Output node");
                else
                    vertexOut = n;
            }
        // Explicitly a PASS-graph gate, not availableInputs > 0: a BASE graph
        // gains wired inputs the moment it reads the scene (post materials),
        // and it still owns the vertex stage.
        if (vertexOut && passGraph)
            fail(vertexOut->id, "the vertex stage belongs to the BASE material's "
                                "graph, not a pass graph");
        // `!= Sprite`, so Mesh reads as Fullscreen here -- one of the two sites
        // the entry guard above covers; see it before adding a third surface.
        if (vertexOut && surface != MaterialSurface::Sprite &&
            inputLink.count({ vertexOut->id, 2 }))
            fail(vertexOut->id,
                 "the color pin requires the sprite surface (fullscreen "
                 "Varyings carry no vertex color)");

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
        // The SECOND `!= Sprite` site the entry guard above covers: Mesh takes
        // the Fullscreen branch here too.
        if (surface != MaterialSurface::Sprite)
            for (const GraphNode* n : ordered)
                if (n->type == GraphNodeType::VertexColor || n->type == GraphNodeType::SpriteTexture)
                    fail(n->id, std::string(GraphNodeInfo(n->type).display) +
                                " requires the sprite surface");

        // --- pass-context gating: a PassInput samples a slot the owning pass
        // wired on the pass canvas; anything else is authoring error.
        for (const GraphNode* n : ordered)
            if (n->type == GraphNodeType::PassInput && n->passInputSlot >= availableInputs)
                fail(n->id, availableInputs == 0
                                ? std::string("Pass Input needs a pass with wired inputs "
                                              "(base graphs have none)")
                                : "Pass Input slot " + std::to_string(n->passInputSlot) +
                                      " is not wired (the pass has " +
                                      std::to_string(availableInputs) + " input(s))");

        // --- Custom nodes: pins name real function parameters; the body is a
        // real function body. Validate the parts the compiler would report as
        // gibberish otherwise.
        for (const GraphNode* n : ordered)
        {
            if (n->type != GraphNodeType::Custom)
                continue;
            if (n->customBody.empty())
                fail(n->id, "Custom node has no HLSL body");
            if (n->customOutWidth != 1 && n->customOutWidth != 2 && n->customOutWidth != 4)
                fail(n->id, "Custom node output width must be 1, 2, or 4");
            for (std::size_t i = 0; i < n->customPins.size(); ++i)
            {
                const GraphCustomPin& p = n->customPins[i];
                if (!ValidParamName(p.name))
                    fail(n->id, "Custom pin '" + p.name + "' is not a valid identifier");
                if (p.width != 1 && p.width != 2 && p.width != 4)
                    fail(n->id, "Custom pin '" + p.name + "' width must be 1, 2, or 4");
                for (std::size_t j = i + 1; j < n->customPins.size(); ++j)
                    if (n->customPins[j].name == p.name)
                        fail(n->id, "Custom pin name '" + p.name + "' is duplicated");
            }
        }

        // --- Swizzle masks: 1/2/4 lanes from xyzw (no float3 in the value set)
        for (const GraphNode* n : ordered)
        {
            if (n->type != GraphNodeType::Swizzle)
                continue;
            const std::string& m = n->swizzleMask;
            bool ok = m.size() == 1 || m.size() == 2 || m.size() == 4;
            for (char c : m)
                ok = ok && SwizzleLane(c) >= 0;
            if (!ok)
                fail(n->id, "Swizzle mask '" + m + "' must be 1, 2, or 4 chars from xyzw");
        }

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
        // Custom-node functions, emitted above shade() (line-mapped to their
        // node so compile errors INSIDE a body badge the Custom node).
        std::vector<std::pair<std::string, std::uint32_t>> funcs;
        // The emit-once registry: shared helpers land in `funcs` the first
        // time any node needs them (line-mapped 0 = engine-owned).
        std::unordered_set<std::string_view> emittedHelpers;
        // Custom nodes reachable from BOTH walks (the visit state resets for
        // the vertex walk) must still define their _cf function exactly once.
        std::unordered_set<std::uint32_t> emittedCustomFuncs;
        auto emitHelperOnce = [&](std::string_view key, std::span<const char* const> helper)
        {
            if (!emittedHelpers.insert(key).second)
                return;
            for (const char* line : helper)
                funcs.emplace_back(line, 0);
            funcs.emplace_back("", 0);
        };

        // Expression for a node's output pin AFTER the node was visited.
        auto pinExpr = [&](const GraphNode* n, std::uint32_t pin, int& outWidth) -> std::string
        {
            const GraphPinDesc desc = GraphNodeOutputPin(*n, pin);
            outWidth = desc.width == 0 ? widthOf[n->id] : desc.width;
            if (GraphNodeOutputCount(*n) == 1)
                return "_n" + std::to_string(n->id);
            return "_n" + std::to_string(n->id) + "_" + desc.name;
        };

        // The vertex walk (UE's WPO model): helpers ARE available -- the
        // templates stitch %{MATERIAL_BODY} BEFORE %{VERTEX_BODY}, so
        // displace() can call the snippet's functions -- and texture reads
        // emit SampleLevel (VS has no implicit derivatives). Only Pass Input
        // stays barred (the vertex context lives on the base graph, which has
        // no upstream passes).
        bool vertexWalk = false;
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

            if (vertexWalk && n->type == GraphNodeType::PassInput)
            {
                fail(n->id, "Pass Input cannot drive the vertex stage");
                return false;
            }

            const std::uint32_t inputCount = GraphNodeInputCount(*n);

            // Visit children; collect per-input (expression, width) with
            // unconnected defaults.
            struct In { std::string expr; int width = 1; bool connected = false; };
            std::vector<In> in(inputCount);
            for (std::uint32_t pin = 0; pin < inputCount; ++pin)
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
            for (std::uint32_t pin = 0; pin < inputCount; ++pin)
                dynamic = dynamic || GraphNodeInputPin(*n, pin).width == 0;
            int w = 0;
            if (dynamic)
            {
                for (std::uint32_t pin = 0; pin < inputCount; ++pin)
                    if (GraphNodeInputPin(*n, pin).width == 0 && in[pin].connected &&
                        in[pin].width > 1)
                        w = w == 0 ? in[pin].width : std::min(w, in[pin].width);
                if (w == 0)
                    w = 1;
            }
            widthOf[n->id] = w;

            // Adapted expression for input `pin` at target width `t` (0 = the
            // node's resolved dynamic width). Precedence is WIRE > user
            // literal > neutral default: a wire hides the literal without
            // destroying it (unwiring restores the value -- SG behavior), and
            // a literal outranks `def`, which is why it also overrides every
            // NON-ZERO neutral -- Combine alpha, Clamp max, Smoothstep edge1,
            // Power exponent, TilingOffset tiling, SimpleNoise scale, Panner
            // uv, ScaleOffset scale: the eight argOr call sites that pass
            // something other than "0.0". Unconnected and literal-free inputs
            // read `def` exactly as before, so a graph with no literals emits
            // byte-identical text.
            //
            // `defWidth` is the width of `def` ITSELF. Every constant neutral
            // is a width-1 string that splats (the default), but Panner's
            // `v.uv` is already a float2 and must be handed to Adapt as one --
            // Adapt(.., 1, 2) would turn it into "(v.uv).xx" (Adapt's scalar
            // rule, this file's Adapt above), duplicating u into both lanes.
            auto argOr = [&](std::uint32_t pin, int t, const char* def,
                             int defWidth = 1) -> std::string
            {
                if (t == 0)
                    t = w;
                if (in[pin].connected)
                    return Adapt(in[pin].expr, in[pin].width, t);
                if (const GraphPinLiteral* lit = n->FindPinLiteral(pin))
                {
                    // The literal adapts FROM its own lane count the same way
                    // the width-1 `def` string does below -- one adaptation
                    // table, no special case.
                    const int lanes = GraphPinLiteralLanes(GraphNodeInputPin(*n, pin).width);
                    return Adapt(PinLiteralExpr(*lit, lanes), lanes, t);
                }
                return Adapt(def, defWidth, t);
            };
            auto arg = [&](std::uint32_t pin, int t) { return argOr(pin, t, "0.0"); };

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
                    // The vertex stage samples at mip 0 explicitly -- Sample's
                    // implicit derivatives only exist in the pixel stage.
                    const std::string call = vertexWalk
                        ? tex + ".SampleLevel(MaterialSampler, " + uv + ", 0.0)"
                        : tex + ".Sample(MaterialSampler, " + uv + ")";
                    stmt("float4 _n" + id + "_rgba = " + call + ";");
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
                case GraphNodeType::Custom:
                {
                    // One function per node instance above shade() + one call
                    // here. Args adapt to the declared pin widths; unconnected
                    // pins read zero.
                    std::string sig = std::string(HlslTypeForWidth(n->customOutWidth)) +
                                      " _cf" + id + "(";
                    std::string call = "_cf" + id + "(";
                    for (std::size_t p = 0; p < n->customPins.size(); ++p)
                    {
                        if (p != 0)
                        {
                            sig += ", ";
                            call += ", ";
                        }
                        sig += std::string(HlslTypeForWidth(n->customPins[p].width)) + " " +
                               n->customPins[p].name;
                        call += arg(static_cast<std::uint32_t>(p), n->customPins[p].width);
                    }
                    sig += ")";
                    call += ")";

                    if (emittedCustomFuncs.insert(n->id).second)
                    {
                        funcs.emplace_back(sig, n->id);
                        funcs.emplace_back("{", n->id);
                        std::string_view bodyText = n->customBody;
                        while (!bodyText.empty())
                        {
                            const std::size_t nl = bodyText.find('\n');
                            std::string_view lineText = bodyText.substr(0, nl);
                            if (!lineText.empty() && lineText.back() == '\r')
                                lineText.remove_suffix(1);
                            funcs.emplace_back("    " + std::string(lineText), n->id);
                            if (nl == std::string_view::npos)
                                break;
                            bodyText.remove_prefix(nl + 1);
                        }
                        funcs.emplace_back("}", n->id);
                        funcs.emplace_back("", 0);
                    }

                    local(n->customOutWidth, call);
                    break;
                }
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
                case GraphNodeType::Combine:
                    local(4, "float4(" + arg(0, 1) + ", " + arg(1, 1) + ", " + arg(2, 1) +
                             ", " + argOr(3, 1, "1.0") + ")");
                    break;
                case GraphNodeType::Clamp:
                    local(w, "clamp(" + arg(0, 0) + ", " + arg(1, 0) + ", " +
                             argOr(2, 0, "1.0") + ")");
                    break;
                case GraphNodeType::Smoothstep:
                    local(w, "smoothstep(" + arg(0, 0) + ", " + argOr(1, 0, "1.0") + ", " +
                             arg(2, 0) + ")");
                    break;
                case GraphNodeType::Step:
                    local(w, "step(" + arg(0, 0) + ", " + arg(1, 0) + ")");
                    break;
                case GraphNodeType::Power:
                    local(w, "pow(" + arg(0, 0) + ", " + argOr(1, 0, "1.0") + ")");
                    break;
                case GraphNodeType::Remap:
                {
                    // SG Remap: out.x + (x - in.x) * (out.y - out.x) / (in.y - in.x).
                    // Ranges are float2 pins defaulting to (0, 1) -- the zero
                    // default would divide by zero.
                    const std::string x = arg(0, 0);
                    const std::string ir = in[1].connected
                                               ? Adapt(in[1].expr, in[1].width, 2)
                                               : std::string("float2(0.0, 1.0)");
                    const std::string outr = in[2].connected
                                                 ? Adapt(in[2].expr, in[2].width, 2)
                                                 : std::string("float2(0.0, 1.0)");
                    local(w, "(" + outr + ").x + (" + x + " - (" + ir + ").x) * ((" + outr +
                             ").y - (" + outr + ").x) / ((" + ir + ").y - (" + ir + ").x)");
                    break;
                }
                case GraphNodeType::TilingOffset:
                {
                    const std::string uv = in[0].connected
                                               ? Adapt(in[0].expr, in[0].width, 2)
                                               : std::string("v.uv");
                    local(2, uv + " * " + argOr(1, 2, "1.0") + " + " + arg(2, 2));
                    break;
                }
                case GraphNodeType::Cos:
                    local(w, "cos(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::Abs:
                    local(w, "abs(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::Min:
                    local(w, "min(" + arg(0, 0) + ", " + arg(1, 0) + ")");
                    break;
                case GraphNodeType::Max:
                    local(w, "max(" + arg(0, 0) + ", " + arg(1, 0) + ")");
                    break;
                case GraphNodeType::Swizzle:
                {
                    // Source reads at its NATIVE width; absent lanes read 0
                    // (the Split rule). All-present masks emit a real HLSL
                    // swizzle; mixed masks build a constructor.
                    const int sw = in[0].connected ? in[0].width : 1;
                    const std::string src = in[0].connected ? in[0].expr : std::string("0.0");
                    const std::string& mask = n->swizzleMask;
                    const int mlen = static_cast<int>(mask.size());
                    widthOf[n->id] = mlen;   // the output pin's dynamic width
                    bool allPresent = true;
                    for (char c : mask)
                        allPresent = allPresent && SwizzleLane(c) < sw;
                    std::string expr;
                    if (allPresent)
                        expr = "(" + src + ")." + mask;
                    else
                    {
                        const char* lane[4] = { ".x", ".y", ".z", ".w" };
                        auto laneExpr = [&](char c) -> std::string
                        {
                            const int idx = SwizzleLane(c);
                            if (idx >= sw)
                                return "0.0";
                            return sw == 1 ? "(" + src + ")"
                                           : "(" + src + ")" + lane[idx];
                        };
                        if (mlen == 1)
                            expr = laneExpr(mask[0]);
                        else
                        {
                            expr = std::string(HlslTypeForWidth(mlen)) + "(";
                            for (int i = 0; i < mlen; ++i)
                            {
                                if (i)
                                    expr += ", ";
                                expr += laneExpr(mask[static_cast<std::size_t>(i)]);
                            }
                            expr += ")";
                        }
                    }
                    local(mlen, expr);
                    break;
                }
                case GraphNodeType::SimpleNoise:
                    emitHelperOnce("simple_noise", kSimpleNoiseHelper);
                    local(1, "_g_simple_noise((" +
                             (in[0].connected ? Adapt(in[0].expr, in[0].width, 2)
                                              : std::string("v.uv")) +
                             ") * " + argOr(1, 1, "10.0") + ")");
                    break;
                case GraphNodeType::VertexOutput:
                    // Only connected pins emit -- an untouched pin is a true
                    // passthrough for that member.
                    if (in[0].connected)
                        stmt("v.pos.xy += " + Adapt(in[0].expr, in[0].width, 2) + ";");
                    if (in[1].connected)
                        stmt("v.uv += " + Adapt(in[1].expr, in[1].width, 2) + ";");
                    if (in[2].connected)
                        stmt("v.color *= " + Adapt(in[2].expr, in[2].width, 4) + ";");
                    break;
                case GraphNodeType::PassInput:
                {
                    // Mirrors TextureSample: rgba primary + consumed-only alpha.
                    const std::string tex =
                        n->passInputSlot == 0
                            ? std::string("InputTexture")
                            : "InputTexture" + std::to_string(n->passInputSlot);
                    const std::string uv = in[0].connected
                                               ? Adapt(in[0].expr, in[0].width, 2)
                                               : std::string("v.uv");
                    stmt("float4 _n" + id + "_rgba = " + tex +
                         ".Sample(MaterialSampler, " + uv + ");");
                    if (pinConsumed(n->id, 1))
                        stmt("float _n" + id + "_a = _n" + id + "_rgba.a;");
                    break;
                }
                // --- library growth batch 2 -------------------------------
                // Dynamic unaries: one operand read through arg(), so the
                // pin composes with an inline literal for free.
                case GraphNodeType::Exp:
                    local(w, "exp(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::Negate:
                    // The spec's shape is parenthesized, so the minus reads
                    // unambiguously whichever adaptation form the operand took
                    // ("_n3", "(_n3).xx", "float2(...)").
                    local(w, "-(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::Floor:
                    local(w, "floor(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::Ceil:
                    local(w, "ceil(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::Round:
                    local(w, "round(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::Sign:
                    // HLSL's sign() returns an INT vector -- `dxc -ast-dump`
                    // types the call `vector<int, 2> (vector<float, 2>)` --
                    // so this float local leans on the implicit int -> float
                    // conversion. The [shadercompile] batch-2 section is the
                    // proof DXC accepts it on DXIL and SPIR-V both.
                    local(w, "sign(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::Normalize:
                    local(w, "normalize(" + arg(0, 0) + ")");
                    break;
                // Scalar-out kernels: operands adapt to the node's resolved
                // dynamic width `w` FIRST, then the intrinsic collapses to one
                // float. HLSL would splat a mismatched scalar operand itself
                // (verified: `dot(float, float2)` compiles clean under dxc),
                // but routing through arg() keeps the emitted text explicit
                // and identical to every other dynamic node's. The local is
                // width 1 and so is the table's output pin (the SimpleNoise
                // pattern), so pinExpr hands consumers width 1 and the
                // adaptation table splats it downstream.
                case GraphNodeType::Length:
                    local(1, "length(" + arg(0, 0) + ")");
                    break;
                case GraphNodeType::Distance:
                    local(1, "distance(" + arg(0, 0) + ", " + arg(1, 0) + ")");
                    break;
                case GraphNodeType::Dot:
                    local(1, "dot(" + arg(0, 0) + ", " + arg(1, 0) + ")");
                    break;
                case GraphNodeType::Panner:
                {
                    // Both pins are fixed float2 and BOTH read through the
                    // argOr seam -- unlike TilingOffset above, whose uv reads
                    // its v.uv default directly and therefore ignores
                    // literals. speed going through arg() is the point: a pin
                    // literal alone can drive the scroll, no Const node.
                    // `Time` is a Globals cbuffer member declared ABOVE both
                    // stitched seams (data/shaders/materials/fullscreen_material.hlsl:
                    // cbuffer at :26-31, %{MATERIAL_BODY} :39, %{VERTEX_BODY}
                    // :41), so a Panner may also drive the vertex stage.
                    //
                    // pannerFractional is UE's bFractionalPart, and the frac()
                    // goes around the PRODUCT ONLY. Read at the vendored
                    // implementation rather than guessed -- UMaterialExpression
                    // Panner::Compile builds Arg1/Arg2 = Frac(Mul(Time, Speed))
                    // per component and adds the coordinate AFTERWARDS
                    // (Arcane/.example/UnrealEngine-release/Engine/Source/
                    // Runtime/Engine/Private/Materials/MaterialExpressions.cpp:
                    // 5469-5489). Wrapping the whole sum instead would tile the
                    // coordinate itself and put a seam in the middle of the uv
                    // space; this form only bounds the offset. HLSL frac() is
                    // per-component, so one call covers UE's two.
                    const std::string scroll = "Time * " + arg(1, 2);
                    local(2, argOr(0, 2, "v.uv", 2) + " + " +
                             (n->pannerFractional ? "frac(" + scroll + ")" : scroll));
                    break;
                }
                case GraphNodeType::ScaleOffset:
                    // UE's ConstantBiasScale: Mul(Add(Bias, Input), Scale)
                    // (Arcane/.example/UnrealEngine-release/Engine/Source/
                    // Runtime/Engine/Private/Materials/MaterialExpressions.cpp:
                    // 12702). Operands commute on the add, so this writes x
                    // first for readability -- same value, same rounding.
                    // bias/scale target width 1 (their declared pin width, the
                    // SimpleNoise.scale precedent): HLSL splats a scalar
                    // operand itself, which keeps the text readable when x is
                    // a float2/float4, and a wider wire narrows through the
                    // adaptation table exactly as a width-1 pin promises.
                    local(w, "(" + arg(0, 0) + " + " + argOr(1, 1, "0.0") + ") * " +
                             argOr(2, 1, "1.0"));
                    break;
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

        // The vertex walk: independent DFS from Vertex Output into a fresh
        // statement list (locals shared with the pixel walk duplicate safely
        // -- separate function scopes).
        std::vector<std::pair<std::string, std::uint32_t>> pixelBody = std::move(body);
        body.clear();
        if (vertexOut)
        {
            state.clear();
            widthOf.clear();
            vertexWalk = true;
            if (!visit(vertexOut) || !res.errors.empty())
            {
                res.snippet.clear();
                res.lineNodeIds.clear();
                return res;
            }
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
        for (auto& fl : funcs)
            lines.push_back(std::move(fl));
        lines.emplace_back("float4 shade(Varyings v)", 0);
        lines.emplace_back("{", 0);
        for (auto& [text, nodeId] : pixelBody)
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

        // The vertex body is its OWN artifact (it feeds %{VERTEX_BODY}, not
        // the pixel snippet). `body` holds the vertex walk's statements.
        if (vertexOut)
        {
            res.vertexSnippet = "Varyings displace(Varyings v)\n{\n";
            for (auto& [text, nodeId] : body)
                res.vertexSnippet += "    " + text + "\n";
            res.vertexSnippet += "    return v;\n}\n";
        }
        return res;
    }

    GraphCodegenResult GenerateNodePreviewSnippet(const MaterialGraph& graph,
                                                  std::uint32_t nodeId,
                                                  std::uint32_t availableInputs)
    {
        GraphCodegenResult res;
        const GraphNode* target = graph.FindNode(nodeId);
        if (!target)
        {
            res.errors.push_back({ nodeId, "preview: node does not exist" });
            return res;
        }
        if (GraphNodeOutputCount(*target) == 0)
        {
            res.errors.push_back({ nodeId, "preview: node has no output pin" });
            return res;
        }

        MaterialGraph pg = graph;

        // Strip Vertex Output nodes (previews are pixel values) and every link
        // touching them or feeding the Output -- the preview owns the Output.
        std::uint32_t outId = 0;
        for (const GraphNode& n : pg.nodes)
            if (n.type == GraphNodeType::Output)
                outId = n.id;
        if (outId == 0)
        {
            res.errors.push_back({ 0, "preview: graph has no Output node" });
            return res;
        }
        std::vector<std::uint32_t> dropped;
        std::erase_if(pg.nodes, [&](const GraphNode& n)
        {
            if (n.type != GraphNodeType::VertexOutput)
                return false;
            dropped.push_back(n.id);
            return true;
        });
        std::erase_if(pg.links, [&](const GraphLink& l)
        {
            if (l.toNode == outId)
                return true;
            for (std::uint32_t id : dropped)
                if (l.fromNode == id || l.toNode == id)
                    return true;
            return false;
        });

        // target.pin0 -> Custom(float4 value){ return float4(value.rgb, 1) }
        // -> Output. The custom CALL SITE adapts the wire to the declared
        // width-4 pin -- scalars splat (grayscale), float2 appends 0,1 -- and
        // the body forces alpha opaque: exactly the SG preview semantics.
        // (Split would NOT work here: it reads its source at native width, so
        // a scalar target would land in the red lane alone.)
        GraphNode wrap;
        wrap.id = pg.MintId();
        wrap.type = GraphNodeType::Custom;
        wrap.customPins = { { "value", 4 } };
        wrap.customOutWidth = 4;
        wrap.customBody = "return float4(value.rgb, 1.0);";
        pg.links.push_back({ nodeId, 0, wrap.id, 0 });
        pg.links.push_back({ wrap.id, 0, outId, 0 });
        pg.nodes.push_back(std::move(wrap));

        return GenerateGraphSnippet(pg, MaterialSurface::Fullscreen, availableInputs);
    }

    // ------------------------------------------------- pin-literal predicates
    // The two rules the emission switch above encodes, exported so this file
    // is the ONE authority: the editor asks them to decide whether a pin gets
    // a literal widget at all and how many lanes that widget edits. They live
    // here, beside the switch they mirror, precisely because a duplicate on
    // the editor side would drift silently.

    int GraphPinLiteralLanes(int declaredWidth) noexcept
    {
        // Fixed 2/4 keep their lanes; everything else -- INCLUDING dynamic
        // (width-0) pins -- is a scalar. Width resolution (:661-664) reads
        // only CONNECTED inputs, so a literal never pins a node's width
        // regardless of lane count; the scalar choice instead splats it to
        // whatever width the node resolves to.
        return declaredWidth == 2 ? 2 : declaredWidth == 4 ? 4 : 1;
    }

    bool GraphPinAcceptsLiteral(const GraphNode& n, std::uint32_t pin) noexcept
    {
        // The SEAM SCOPE exclusion switch. Every case below names an emission
        // case ABOVE that reads its unconnected input DIRECTLY instead of
        // through argOr (:687-703, whose literal branch is :694-701), so a
        // literal stored on that pin is dead data. Cites are into this file.
        //
        // MAINTENANCE CONTRACT: a node type added to the emission switch that
        // bypasses argOr on any pin must be added here in the SAME commit.
        // MaterialGraphTest.cpp's explicit truth table fails otherwise, which
        // is the whole point of it existing -- the failure mode this replaces
        // was a dead editor widget nobody could see was dead.
        switch (n.type)
        {
            case GraphNodeType::Output:          // :714-716
            case GraphNodeType::TextureSample:   // :741-742, shared with
            case GraphNodeType::SpriteTexture:   //   SpriteTexture
            case GraphNodeType::PassInput:       // :975-977
            case GraphNodeType::Split:           // :835-836
            case GraphNodeType::Swizzle:         // :912-913
            case GraphNodeType::VertexOutput:    // :961-966 (all three pins)
                return false;                    // every input pin bypasses argOr
            case GraphNodeType::TilingOffset:    // uv reads v.uv direct (:889-891)
            case GraphNodeType::SimpleNoise:     // uv likewise (:954-955)
                return pin != 0;
            case GraphNodeType::Remap:           // ranges read float2(0,1) (:877-882)
                return pin == 0;
            default:
                // Everything else routes every operand pin through argOr,
                // INCLUDING Custom nodes' per-node pins (:803) and Panner's
                // uv, which -- unlike TilingOffset's -- takes its v.uv
                // neutral THROUGH the seam as a width-2 default (:1058).
                return true;
        }
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
                case GraphNodeType::Swizzle:
                    e["mask"] = n->swizzleMask;
                    break;
                case GraphNodeType::PassInput:
                    e["slot"] = n->passInputSlot;
                    break;
                case GraphNodeType::Panner:
                    // Written ONLY when set, like pinDefaults below: a Panner
                    // authored before this option gains no key, so its file
                    // rewrites byte-identical and an older engine (which
                    // ignores unknown keys) still loads a newer file.
                    if (n->pannerFractional)
                        e["frac"] = true;
                    break;
                case GraphNodeType::Comment:
                    e["comment"] = nlohmann::json{
                        { "text", n->paramName },
                        { "size", nlohmann::json::array({ n->value[0], n->value[1] }) },
                    };
                    break;
                case GraphNodeType::Custom:
                {
                    nlohmann::json c;
                    c["out"] = n->customOutWidth;
                    c["body"] = n->customBody;
                    nlohmann::json pins = nlohmann::json::array();
                    for (const GraphCustomPin& p : n->customPins)
                        pins.push_back(nlohmann::json{ { "name", p.name },
                                                       { "width", p.width } });
                    c["pins"] = std::move(pins);
                    e["custom"] = std::move(c);
                    break;
                }
                default:
                    break;
            }
            // Inline pin literals ride OUTSIDE the per-type switch (any node
            // with input pins can carry them) and are written only when the
            // user set one -- an untouched graph gains no key at all, which is
            // what keeps old files byte-identical on rewrite. Sorted by pin
            // for the same diff-stability reason nodes and links are sorted.
            if (!n->pinLiterals.empty())
            {
                std::vector<const GraphPinLiteral*> lits;
                lits.reserve(n->pinLiterals.size());
                for (const GraphPinLiteral& l : n->pinLiterals)
                    lits.push_back(&l);
                std::sort(lits.begin(), lits.end(),
                          [](const GraphPinLiteral* a, const GraphPinLiteral* b)
                          { return a->pin < b->pin; });
                nlohmann::json defs = nlohmann::json::array();
                for (const GraphPinLiteral* l : lits)
                {
                    const int lanes = GraphPinLiteralLanes(GraphNodeInputPin(*n, l->pin).width);
                    nlohmann::json v;
                    if (lanes == 1)
                        v = l->v[0];   // scalar pins stay bare numbers on disk
                    else
                    {
                        v = nlohmann::json::array();
                        for (int i = 0; i < lanes; ++i)
                            v.push_back(l->v[i]);
                    }
                    defs.push_back(nlohmann::json{ { "pin", l->pin },
                                                   { "value", std::move(v) } });
                }
                e["pinDefaults"] = std::move(defs);
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
            if (e.contains("mask") && e["mask"].is_string())
                n.swizzleMask = e["mask"].get<std::string>();
            if (e.contains("slot") && e["slot"].is_number_unsigned())
                n.passInputSlot = e["slot"].get<std::uint32_t>();
            // Absent = false is the whole compatibility story for this key; a
            // non-boolean "frac" is ignored rather than refused, matching how
            // every other optional key above type-checks before reading.
            if (e.contains("frac") && e["frac"].is_boolean())
                n.pannerFractional = e["frac"].get<bool>();
            if (e.contains("comment") && e["comment"].is_object())
            {
                const nlohmann::json& c = e["comment"];
                if (c.contains("text") && c["text"].is_string())
                    n.paramName = c["text"].get<std::string>();
                if (c.contains("size") && c["size"].is_array() && c["size"].size() == 2 &&
                    c["size"][0].is_number() && c["size"][1].is_number())
                {
                    n.value[0] = c["size"][0].get<float>();
                    n.value[1] = c["size"][1].get<float>();
                }
            }
            if (e.contains("custom") && e["custom"].is_object())
            {
                const nlohmann::json& c = e["custom"];
                if (c.contains("out") && c["out"].is_number_integer())
                {
                    const int w = c["out"].get<int>();
                    if (w == 1 || w == 2 || w == 4)
                        n.customOutWidth = w;
                }
                if (c.contains("body") && c["body"].is_string())
                    n.customBody = c["body"].get<std::string>();
                if (c.contains("pins") && c["pins"].is_array())
                {
                    for (const nlohmann::json& jp : c["pins"])
                    {
                        if (!jp.is_object() || !jp.contains("name") || !jp["name"].is_string())
                            continue;
                        GraphCustomPin p;
                        p.name = jp["name"].get<std::string>();
                        if (jp.contains("width") && jp["width"].is_number_integer())
                        {
                            const int w = jp["width"].get<int>();
                            if (w == 1 || w == 2 || w == 4)
                                p.width = w;
                        }
                        n.customPins.push_back(std::move(p));
                    }
                }
            }
            // "pinDefaults" is read LAST on purpose: a Custom node's input
            // count is its customPins list, parsed just above, so the
            // unknown-pin check needs those pins already in place.
            if (e.contains("pinDefaults") && e["pinDefaults"].is_array())
            {
                const std::uint32_t inputs = GraphNodeInputCount(n);
                for (const nlohmann::json& jl : e["pinDefaults"])
                {
                    // is_number_integer() accepts nlohmann's SIGNED and
                    // unsigned integer types both: a hand-built `{"pin": 1}`
                    // types as signed, and dropping those as malformed would
                    // make the tolerance promise a lie.
                    if (!jl.is_object() || !jl.contains("pin") || !jl.contains("value") ||
                        !jl["pin"].is_number_integer() || jl["pin"].get<std::int64_t>() < 0)
                        continue;
                    GraphPinLiteral lit;
                    lit.pin = static_cast<std::uint32_t>(jl["pin"].get<std::int64_t>());
                    if (lit.pin >= inputs)
                    {
                        // Content damage, not structural: warn and drop the
                        // one entry, keep the file. GraphFromJson's nullopt
                        // refusal stays reserved for shapes we cannot
                        // represent at all (unknown node types, bad ids).
                        ARC_WARN("material graph: node {} carries a pin literal on "
                                 "unknown pin {} ({} input(s)) -- dropped",
                                 n.id, lit.pin, inputs);
                        continue;
                    }
                    if (n.FindPinLiteral(lit.pin))
                        continue;   // one literal per pin; first wins
                    const nlohmann::json& v = jl["value"];
                    // Tolerant of BOTH shapes regardless of the pin's width:
                    // hand-authored files legitimately write a bare number
                    // where the writer would emit an array, and the unread
                    // lanes simply stay 0.
                    if (v.is_number())
                        lit.v[0] = v.get<float>();
                    else if (v.is_array())
                    {
                        for (std::size_t i = 0; i < std::min<std::size_t>(4, v.size()); ++i)
                            if (v[i].is_number())
                                lit.v[i] = v[i].get<float>();
                    }
                    else
                        continue;
                    n.pinLiterals.push_back(lit);
                }
            }
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
                if (l.fromPin >= GraphNodeOutputCount(*from) ||
                    l.toPin >= GraphNodeInputCount(*to))
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

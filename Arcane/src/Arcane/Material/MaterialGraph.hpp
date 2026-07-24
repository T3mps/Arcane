#pragma once

// Material node graph (Slice 9 of the shader-editor arc): the SECOND authoring
// front-end over the same material artifact. A graph is a small dataflow DAG
// that GenerateGraphSnippet compiles into a text SNIPPET -- `//@param`
// declarations + a `float4 shade(Varyings v)` body -- targeting the exact same
// %{MATERIAL_BODY} seam the text editor feeds. Everything downstream
// (ParseMaterialSource, template stitch, compile service, preview, params
// panel, instances, sprite cache) is untouched: a graph-owned .armat still
// SAVES the generated snippet, so every existing loader keeps working and the
// graph is purely the authoring truth on top.
//
// Reference model: Unity Shader Graph 17.6.0 -- the deep dive at
// docs/superpowers/2026-07-24-unity-shadergraph-1760-deep-dive.md drives the
// contracts here (edge identity, adaptation table, deterministic SSA naming,
// structured per-node errors, monotonic ids). Where this header says
// "SG rule", that document is the citation.
//
// Ownership rule (spec 5.8): a material is graph-owned (the .armat carries a
// "graph" object; the text panel shows generated HLSL read-only) or text-owned.
// Convert-to-text severs the graph one way; there is no text->graph decompile.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Material/MaterialSource.hpp>   // MaterialSurface (surface-gated nodes)
#include <Arcane/Material/MaterialTypes.hpp>

#include <Json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on dll-exported types: benign under /MD
#endif

    enum class GraphNodeType : std::uint8_t
    {
        Output = 0,     // the single float4 result -> shade()'s return; exactly one per graph
        ConstFloat,
        ConstFloat2,
        ConstFloat4,
        ConstColor,     // float4; stores LINEAR floats (Arcane is linear-only end to
                        // end -- any sRGB conversion is the color picker's job, never
                        // codegen's; SG's IsGammaSpace machinery is deliberately skipped)
        Param,          // declares/references a numeric //@param (name+type+default)
        TextureSample,  // declares a texture //@param + samples it (outputs rgba + a)
        SpriteTexture,  // samples the sprite's OWN texture (template t0); sprite surface only
        UV,             // v.uv
        Time,           // Globals Time
        VertexColor,    // v.color (the sprite tint); sprite surface only
        Add,            // a + b   (dynamic width)
        Sub,            // a - b   (dynamic width)
        Mul,            // a * b   (dynamic width)
        Lerp,           // lerp(a, b, t)   (dynamic width)
        Sin,            // sin(x)  (dynamic width)
        Fraction,       // frac(x) (dynamic width)
        Saturate,       // saturate(x) (dynamic width)
        OneMinus,       // 1 - x   (dynamic width)
        Split,          // x -> r/g/b/a scalars (missing lanes read 0; SG Split rule)
    };

    // One pin on a node type. `width` = component count of the value flowing
    // through it: 1/2/4 fixed, 0 = DYNAMIC (resolved per node instance -- SG
    // rule: scalars never pin a dynamic node's width; the resolved width is the
    // MINIMUM of the connected non-scalar dynamic inputs, else 1).
    //
    // Wire adaptation at every edge follows SG's table verbatim:
    //   equal width      -> as-is
    //   scalar -> N      -> splat (`x.xx` / `x.xxxx`)
    //   float2 -> float4 -> append 0.0, 1.0
    //   wider -> narrower-> leading swizzle (`.xy`, `.x`)
    // (float3 does not exist in this graph's value set.)
    struct GraphPinDesc
    {
        const char* name;    // canvas label AND codegen suffix for output pins
        int         width;   // 1/2/4, or 0 = dynamic
    };

    // Static per-type description -- THE node table (SG lesson: ~90% of a node
    // library is data, not code). Codegen, the canvas (labels, pin colors,
    // create menu), and serialization all read this one table; adding a node
    // type = one row + tests.
    //
    // PIN ORDER IS A SERIALIZED CONTRACT: GraphLink.toPin/fromPin index into
    // these spans. Evolution rule is APPEND-ONLY -- new pins go at the end;
    // never reorder or remove a pin from a shipped node type (SG survives this
    // only because its slot ids are explicit constants; ours are indices).
    struct GraphNodeTypeInfo
    {
        GraphNodeType type;
        const char*   token;     // serialization token ("const_float", "texture_sample", ...)
        const char*   display;   // canvas title / create-menu label
        std::span<const GraphPinDesc> inputs;
        std::span<const GraphPinDesc> outputs;
    };

    [[nodiscard]] ARCANE_API const GraphNodeTypeInfo& GraphNodeInfo(GraphNodeType t) noexcept;
    [[nodiscard]] ARCANE_API std::span<const GraphNodeTypeInfo> AllGraphNodeInfos() noexcept;
    [[nodiscard]] ARCANE_API bool GraphNodeTypeFromToken(std::string_view token,
                                                         GraphNodeType& out) noexcept;

    struct GraphNode
    {
        std::uint32_t id = 0;                        // unique within the graph, > 0, NEVER reused
        GraphNodeType type = GraphNodeType::Output;
        float posX = 0.0f, posY = 0.0f;              // canvas layout (persisted)

        float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // Const* payload

        // Param / TextureSample: the //@param this node declares. All nodes with
        // the same name share ONE declaration; codegen rejects conflicting types.
        // RENAME WART (documented, SG-parity): instance overrides key on the
        // param NAME -- renaming a param orphans existing .armat instance
        // overrides exactly like Unity's reference-name rename. MVP accepts
        // this; an editor-assisted rename that rewrites instances is a
        // follow-up (we have GUID-identified assets to find them; Unity
        // doesn't).
        std::string   paramName;
        MatParamType  paramType = MatParamType::Float;   // Param only (TextureSample is Texture)
        MatParamValue paramDefault;                      // Param only: the decl default
        bool          hasRange = false;                  // Param only: [min..max] slider hint
        float         rangeMin = 0.0f, rangeMax = 1.0f;
    };

    // Edge identity is (node, pin) on BOTH ends (SG rule -- multi-output nodes
    // are load-bearing even in this small set: TextureSample exposes rgba AND
    // a; Split exposes four scalars).
    struct GraphLink
    {
        std::uint32_t fromNode = 0;
        std::uint32_t fromPin = 0;    // output pin index on fromNode
        std::uint32_t toNode = 0;
        std::uint32_t toPin = 0;      // input pin index on toNode
    };

    struct MaterialGraph
    {
        std::vector<GraphNode> nodes;
        std::vector<GraphLink> links;

        // Monotonic id allocator, SERIALIZED with the graph. Never max+1: a
        // deleted id must stay dead, or a delete->create->undo sequence under
        // incremental CommandStack undo aliases a stale link onto a new node.
        std::uint32_t nextId = 1;

        [[nodiscard]] bool Empty() const noexcept { return nodes.empty(); }
        // (Inline: MaterialGraph is a plain data struct -- only the free
        // functions below are dll-exported, matching MaterialAsset's style.)
        [[nodiscard]] const GraphNode* FindNode(std::uint32_t id) const noexcept
        {
            for (const GraphNode& n : nodes)
                if (n.id == id)
                    return &n;
            return nullptr;
        }
        [[nodiscard]] GraphNode* FindNode(std::uint32_t id) noexcept
        {
            for (GraphNode& n : nodes)
                if (n.id == id)
                    return &n;
            return nullptr;
        }
        // Mint a fresh node id (bumps nextId; self-heals if a hand-edited file
        // carries nextId <= an existing id).
        [[nodiscard]] std::uint32_t MintId() noexcept
        {
            std::uint32_t maxId = 0;
            for (const GraphNode& n : nodes)
                maxId = maxId < n.id ? n.id : maxId;
            if (nextId <= maxId)
                nextId = maxId + 1;   // never reuse a dead id
            return nextId++;
        }
    };

    // Structured codegen diagnostics: the canvas badges the offending node (SG:
    // one badge per node, message on hover). nodeId 0 = graph-level message.
    struct GraphError
    {
        std::uint32_t nodeId = 0;
        std::string   message;
    };

    struct GraphCodegenResult
    {
        std::string snippet;               // complete //@param block + shade() body; empty on error
        std::vector<GraphError> errors;
        // Per-snippet-line node attribution: lineNodeIds[i] = the node whose
        // statement produced 0-based line i (0 = engine-owned line). This is
        // what maps DXC diagnostics back onto canvas nodes -- node-accurate
        // error badges without SG's per-node preview-shader farm.
        std::vector<std::uint32_t> lineNodeIds;
        [[nodiscard]] bool Ok() const noexcept { return errors.empty(); }
    };

    // Topological codegen: validate (single Output, unique ids, known link
    // endpoints, no cycles, param name/type coherence, surface-gated nodes),
    // then walk the DAG from Output (post-order DFS, visited-set dedup) and
    // emit straight-line SSA statements wrapped as `float4 shade(Varyings v)`,
    // preceded by one //@param line per distinct Param/TextureSample name (in
    // first-declaring-node-id order -- the Blackboard-order analogue).
    //
    // Locals are `_n<id>` (single-output) / `_n<id>_<pin>` (multi-output,
    // consumed pins only) -- deterministic from serialized ids: same graph
    // file => byte-same snippet (SG rule). Unconnected inputs take neutral
    // defaults: TextureSample/SpriteTexture uv -> v.uv; Output.color -> opaque
    // black; dynamic operands -> 0.0. Unreachable nodes emit no statements but
    // their param decls still register, so the params panel always matches the
    // canvas. `surface` gates VertexColor/SpriteTexture (sprite-only) and is
    // NOT part of the graph: the same graph may compile against either surface
    // if it avoids gated nodes.
    //
    // On any error `snippet` is empty -- callers keep the last good snippet
    // bound, exactly like a failed text compile.
    [[nodiscard]] ARCANE_API GraphCodegenResult GenerateGraphSnippet(
        const MaterialGraph& graph,
        MaterialSurface surface = MaterialSurface::Fullscreen);

    // The "graph" object inside .armat JSON (see MaterialAsset). Output is
    // ordered -- nodes sorted by id, links by (to, toPin) -- purely for diff
    // stability (SG sorts on save for the same reason). FromJson returns
    // nullopt on shape violations (unknown node types, non-numeric ids,
    // duplicate ids) rather than guessing.
    [[nodiscard]] ARCANE_API nlohmann::json GraphToJson(const MaterialGraph& graph);
    [[nodiscard]] ARCANE_API std::optional<MaterialGraph> GraphFromJson(const nlohmann::json& j);

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

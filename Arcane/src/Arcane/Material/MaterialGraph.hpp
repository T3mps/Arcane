#pragma once

// Material node graph (Slice 9 of the shader-editor arc): the SECOND authoring
// front-end over the same material artifact. A graph is a small dataflow DAG
// that GenerateGraphSnippet compiles into a text SNIPPET -- `//@param`
// declarations + a `float4 shade(Varyings v)` body -- targeting the exact same
// %{MATERIAL_BODY} seam the text editor feeds. Everything downstream
// (ParseMaterialSource, template stitch, compile service, preview, params
// panel, instances, sprite cache) is untouched: a graph-owned .arcmat still
// SAVES the generated snippet, so every existing loader keeps working and the
// graph is purely the authoring truth on top.
//
// Reference model: Unity Shader Graph 17.6.0 -- the deep dive at
// docs/superpowers/2026-07-24-unity-shadergraph-1760-deep-dive.md drives the
// contracts here (edge identity, adaptation table, deterministic SSA naming,
// structured per-node errors, monotonic ids). Where this header says
// "SG rule", that document is the citation.
//
// Ownership rule (spec 5.8): a material is graph-owned (the .arcmat carries a
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
        Custom,         // designer HLSL island (the UE Custom node / SG string-mode
                        // Custom Function): user-defined input pins + one output +
                        // a body emitted as its own function above shade(). The
                        // body may read //@param names and Globals (Time...)
                        // directly -- the snippet lands AFTER those declarations.
        // Library growth batch 1 (2026-07-24) -- APPEND-ONLY (enum value indexes
        // kNodeInfos; tokens are the serialized identity).
        Combine,        // float4(r, g, b, a) from scalars (a defaults opaque 1)
        Clamp,          // clamp(x, min, max)          (dynamic width; max defaults 1)
        Smoothstep,     // smoothstep(edge0, edge1, x) (dynamic width; edge1 defaults 1)
        Step,           // step(edge, x)               (dynamic width)
        Power,          // pow(a, b)                   (dynamic width; b defaults 1)
        Remap,          // remap x: inRange -> outRange (ranges float2, default (0,1))
        TilingOffset,   // uv * tiling + offset (float2; uv defaults v.uv, tiling 1)
        Cos,            // cos(x)    (dynamic width)
        Abs,            // abs(x)    (dynamic width)
        Min,            // min(a, b) (dynamic width)
        Max,            // max(a, b) (dynamic width)
        Swizzle,        // lane mask over the input; output width = mask length
        SimpleNoise,    // value noise(uv * scale) -> float; the first node backed
                        // by a SHARED helper function (emit-once registry)
        PassInput,      // samples an upstream pass output (InputTexture(N)) --
                        // PASS GRAPHS only: valid iff the node's slot is wired
                        // on the pass canvas (see GenerateGraphSnippet's
                        // availableInputs)
        VertexOutput,   // the VERTEX context (base graph only, at most one):
                        // connected pins emit into `displace()` -- posOffset
                        // adds to clip-space pos.xy, uvOffset adds to uv,
                        // color multiplies the sprite tint (sprite surface
                        // only). Absent/unconnected = passthrough. Snippet
                        // helpers (Custom, SimpleNoise) ARE available -- the
                        // templates stitch %{MATERIAL_BODY} first -- and
                        // texture reads emit SampleLevel (no implicit
                        // derivatives in VS); only Pass Input stays barred.
        Comment,        // annotation group box (UE comment / SG group): no
                        // pins, emits nothing -- pure canvas furniture that
                        // drags contained nodes. paramName holds the title,
                        // value[0]/value[1] the box size.
        // Library growth batch 2 (2026-07-28) -- APPEND-ONLY, same contract as
        // batch 1 above (enum value indexes kNodeInfos; tokens are the
        // serialized identity). Every operand pin reads through codegen's
        // argOr seam, so all of these compose with inline pin literals.
        Exp,            // exp(x)       (dynamic width)
        Negate,         // -(x)         (dynamic width; parenthesized)
        Floor,          // floor(x)     (dynamic width)
        Ceil,           // ceil(x)      (dynamic width)
        Round,          // round(x)     (dynamic width)
        Sign,           // sign(x)      (dynamic width)
        Normalize,      // normalize(x) (dynamic width)
        // The three scalar-out kernels: operands adapt to the node's resolved
        // dynamic width, then the intrinsic collapses to ONE float -- a fixed
        // width-1 output pin (SimpleNoise is the fixed-out-1 precedent), so
        // consumers splat it like any other scalar.
        Length,         // length(x)
        Distance,       // distance(a, b)
        Dot,            // dot(a, b)
        Panner,         // uv + Time * speed (float2; uv defaults v.uv)
        // Library gap-close (2026-07-28) -- APPEND-ONLY, same contract again.
        ScaleOffset,    // (x + bias) * scale -- UE's ConstantBiasScale, which
                        // compiles Mul(Add(Bias, Input), Scale) (vendored:
                        // Engine/Source/Runtime/Engine/Private/Materials/
                        // MaterialExpressions.cpp:12702). bias and scale are
                        // FIXED width-1 pins routed through argOr, so both take
                        // an inline literal -- that is the whole point of the
                        // node: an affine mul+add pair collapses to ONE node
                        // with two numbers on it. Neutrals are 0 and 1 (the
                        // node is the identity when both are unwired),
                        // deliberately NOT UE's 1.0/0.5: this library's rule is
                        // that an untouched pin changes nothing. Clamp's max =
                        // 1 is the precedent for a non-zero neutral.
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

    // A Custom node's user-authored input pin (name doubles as the function
    // parameter name inside the body).
    struct GraphCustomPin
    {
        std::string name;
        int         width = 1;   // 1/2/4
    };

    // A user-set constant on an UNWIRED input pin (SG/UE parity: a pin can
    // carry a value without a Const node feeding it). Lane count is the pin's
    // DECLARED width -- 1/2/4 fixed, and a SCALAR for dynamic (width-0) pins.
    // A literal never enters dynamic-width resolution regardless of lane
    // count -- that loop reads only CONNECTED inputs (MaterialGraph.cpp:
    // 597-600). The scalar choice instead means a literal on a dynamic pin
    // splats to whatever width the node resolves to, so it never needs
    // re-authoring when wiring changes. Unused lanes stay 0.
    struct GraphPinLiteral
    {
        std::uint32_t pin = 0;
        float         v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    struct GraphNode
    {
        std::uint32_t id = 0;                        // unique within the graph, > 0, NEVER reused
        GraphNodeType type = GraphNodeType::Output;
        float posX = 0.0f, posY = 0.0f;              // canvas layout (persisted)

        float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // Const* payload

        // Param / TextureSample: the //@param this node declares. All nodes with
        // the same name share ONE declaration; codegen rejects conflicting types.
        // (Comment reuses this field as the box TITLE.)
        // RENAME WART (documented, SG-parity): instance overrides key on the
        // param NAME -- renaming a param orphans existing .arcmat instance
        // overrides exactly like Unity's reference-name rename. MVP accepts
        // this; an editor-assisted rename that rewrites instances is a
        // follow-up (we have GUID-identified assets to find them; Unity
        // doesn't).
        std::string   paramName;
        MatParamType  paramType = MatParamType::Float;   // Param only (TextureSample is Texture)
        MatParamValue paramDefault;                      // Param only: the decl default
        bool          hasRange = false;                  // Param only: [min..max] slider hint
        float         rangeMin = 0.0f, rangeMax = 1.0f;

        // Custom only: input pins are PER-NODE DATA (the one type whose pins
        // are not table-static), the body is the function's HLSL (must
        // `return` a value of customOutWidth), and the single output pin's
        // width is customOutWidth. The editor re-indexes links when a pin is
        // removed; codegen/serialization query pins through the per-node
        // helpers below.
        std::vector<GraphCustomPin> customPins;
        std::string customBody;
        int         customOutWidth = 4;

        // Swizzle only: lane mask over the input -- 1/2/4 chars from xyzw
        // (float3 does not exist in this value set). Lanes beyond the source
        // width read 0 (the Split rule).
        std::string swizzleMask = "xyzw";

        // PassInput only: which of the pass's wired input slots to sample
        // (0 = InputTexture, k = InputTexture<k>).
        std::uint32_t passInputSlot = 0;

        // Panner only: UE's bFractionalPart. The frac() wraps the PRODUCT, not
        // the sum -- UE compiles Frac(Mul(Time, Speed)) per component and only
        // THEN adds the coordinate (vendored: Engine/Source/Runtime/Engine/
        // Private/Materials/MaterialExpressions.cpp:5469-5489). UE's stated
        // reason is precision, not looks -- the comment above those lines reads
        // "avoid (delay) divergent accuracy issues as GameTime increases":
        // bounding the offset to [0,1) stops a large Time from eating the
        // mantissa. It is only SEAMLESS for content that repeats with period 1
        // in uv, which is why it is opt-in rather than the default.
        // Serialized as the optional "frac" key, written
        // only when true -- absent means false, so every graph authored before
        // this field emits byte-identical text.
        bool pannerFractional = false;

        // Inline pin literals, SPARSE: only pins the user actually set appear
        // here, so a graph nobody touched carries none and emits exactly the
        // pre-literal snippet. The value SURVIVES wiring -- codegen ignores it
        // while a link exists (MaterialGraph.cpp argOr checks `connected`
        // first), so unwiring restores it, which is SG's behavior. Serialized
        // as the node's "pinDefaults" array.
        //
        // INVARIANT: at most one entry per pin. GraphFromJson's pinDefaults
        // loop keeps the first entry when a pin is duplicated on load
        // (MaterialGraph.cpp:1444-1445), and FindPinLiteral below returns the
        // first match, so callers that mutate this vector must update an
        // existing entry in place rather than append a duplicate.
        //
        // SEAM SCOPE (v1): a literal reaches codegen only on pins whose
        // emission routes through argOr -- which is every numeric operand
        // pin, INCLUDING Custom nodes' per-node pins. Pins that read their
        // unconnected default directly instead ignore literals: Output.color,
        // the v.uv-defaulting pins of TextureSample / SpriteTexture /
        // PassInput / TilingOffset.uv / SimpleNoise.uv, Split/Swizzle's
        // native-width source, Remap's two range pins, and Vertex Output's
        // connected-only pins. Editors must not offer a literal widget on
        // those. Panner.uv is NOT in that set even though it also defaults to
        // v.uv: batch 2 routes it through argOr with a width-2 default.
        std::vector<GraphPinLiteral> pinLiterals;

        // Null when this pin carries no user literal (the common case).
        // (Inline like MaterialGraph::FindNode below: GraphNode is a plain
        // data struct -- only the free functions are dll-exported.)
        [[nodiscard]] const GraphPinLiteral* FindPinLiteral(std::uint32_t pin) const noexcept
        {
            for (const GraphPinLiteral& l : pinLiterals)
                if (l.pin == pin)
                    return &l;
            return nullptr;
        }
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

    // Per-NODE pin queries -- the only correct way to count/inspect pins
    // (Custom nodes carry their own pin list; every other type reads the
    // static table). Returned GraphPinDesc.name points into the node for
    // Custom pins -- transient use only.
    [[nodiscard]] ARCANE_API std::uint32_t GraphNodeInputCount(const GraphNode& n) noexcept;
    [[nodiscard]] ARCANE_API std::uint32_t GraphNodeOutputCount(const GraphNode& n) noexcept;
    [[nodiscard]] ARCANE_API GraphPinDesc GraphNodeInputPin(const GraphNode& n,
                                                            std::uint32_t pin) noexcept;
    [[nodiscard]] ARCANE_API GraphPinDesc GraphNodeOutputPin(const GraphNode& n,
                                                             std::uint32_t pin) noexcept;

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
        // The vertex stage generated from the graph's Vertex Output node --
        // a full `Varyings displace(Varyings v)` body for %{VERTEX_BODY}.
        // Empty when the graph has no Vertex Output (= passthrough).
        std::string vertexSnippet;
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
    //
    // `availableInputs` is the wired-slot context: how many input slots the
    // owning pass has wired on the pass canvas (a BASE graph gains slots too
    // once it reads the scene -- post materials). PassInput nodes with
    // slot >= availableInputs are structured errors (0 -- the default --
    // makes them invalid anywhere without wired inputs). `passGraph` marks an
    // EXTRA pass's graph: Vertex Output is barred there (the vertex stage
    // belongs to the base).
    [[nodiscard]] ARCANE_API GraphCodegenResult GenerateGraphSnippet(
        const MaterialGraph& graph,
        MaterialSurface surface = MaterialSurface::Fullscreen,
        std::uint32_t availableInputs = 0,
        bool passGraph = false);

    // Per-node preview codegen (the editor's SG-style thumbnails): the snippet
    // whose final color VISUALIZES `nodeId`'s first output pin -- scalars splat
    // to grayscale, float2 shows as R/G, and alpha is forced opaque (a preview
    // must never vanish into the canvas). Built entirely from the existing
    // vocabulary: a clone of the graph with the Output re-wired through a
    // synthetic Custom node (`float4(value.rgb, 1)` behind a width-4 pin), so
    // the adaptation table does the width work at the call site and codegen
    // stays untouched. Vertex Output nodes are stripped from
    // the clone (previews are pixel values; no displacement, and pass-context
    // clones stay legal). Always generated against the FULLSCREEN surface --
    // that is the surface thumbnails render on -- so subgraphs using
    // sprite-only nodes refuse with errors (callers show no preview).
    // `availableInputs` follows GenerateGraphSnippet's pass-context contract.
    // Errors: nodeId missing / previewless (no output pins) / whatever the
    // underlying codegen refuses.
    [[nodiscard]] ARCANE_API GraphCodegenResult GenerateNodePreviewSnippet(
        const MaterialGraph& graph,
        std::uint32_t nodeId,
        std::uint32_t availableInputs = 0);

    // The "graph" object inside .arcmat JSON (see MaterialAsset). Output is
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

// Shader-graph canvas backdrop: an infinite, LOD-faded two-octave line grid
// drawn as ONE fullscreen triangle into an editor-owned, canvas-sized RT.
// The editor blits the RT under the node-editor's content and disables the
// vendored editor's own grid (StyleColor_Grid alpha 0), so this is the only
// backdrop the canvas has.
//
// COORDINATE CONTRACT (the whole design in one paragraph):
//   The node editor maps a canvas point c to the screen pixel
//   p = (c - origin) * zoom, where `origin` is the canvas coordinate under the
//   region's top-left corner. Feeding the grid a coordinate q = p + origin*zoom
//   (= c*zoom) makes it PAN 1:1 with the content -- panning changes only
//   `origin`, and q absorbs that change exactly. Zoom is then free to be
//   answered however we like, because it only enters through the PERIOD.
//
//   Screen period = kBaseSpacingPx * pow(zoom, kZoomExponent). At
//   kZoomExponent = 1 the grid is rigidly welded to canvas content (lines pass
//   through fixed canvas coordinates); at 0 it never changes size at all. We
//   want "visibly responds, much less than 1:1", so the exponent is fractional
//   -- see kZoomExponent below.
//
// The cbuffer uses plain register(b0): the SPIR-V build applies
// -fvk-b-shift 256 0 (matching nvrhi::VulkanBindingOffsets), so no #if SPIRV
// guard is needed. Colors are DISPLAY-REFERRED: ImGui draws post-tonemap into
// the backbuffer and samples this RT straight through (imgui.hlsl:1-5), so
// whatever is written here is what the user sees.

// ---------------------------------------------------------------------------
// Tunables. Everything that decides how the backdrop LOOKS lives here.
// ---------------------------------------------------------------------------

// How strongly the grid answers zoom. 1.0 = welded to canvas space (Unity's
// and UE's default feel), 0.0 = a fixed screen-space lattice that only pans.
// 0.4 sits in the requested 0.35-0.5 band and is the value that reads best on
// paper: halving the zoom shrinks the spacing to 0.5^0.4 = 0.76x and doubling
// it grows to 1.32x -- unmistakably alive, but nowhere near the 2x/0.5x lurch
// of a welded grid. It also sets how often the octave LOD steps: an octave is
// 2^(1/0.4) = 5.7x of zoom, so a normal working range crosses at most one
// boundary, and that boundary is faded (below), never snapped.
static const float kZoomExponent = 0.4;

// Grid spacing in canvas units at zoom = 1, before the octave LOD snaps it.
static const float kBaseSpacingPx = 20.0;

// The on-screen spacing the minor octave is driven TOWARD. After the LOD snap
// the minor period always lands in (kMinorTargetPx/2, kMinorTargetPx], so the
// backdrop can never collapse into mush or dissolve into emptiness no matter
// how far the user zooms.
static const float kMinorTargetPx = 22.0;

// Major lines every N minor lines. Powers of two only: the LOD steps by
// doubling, and a power-of-two ratio is what keeps majors landing exactly on
// minor lines across a step (so majors never "slide" through the minor field).
static const float kMajorEvery = 8.0;

// Line geometry, in screen pixels. Half-width plus the anti-alias ramp.
static const float kHalfWidthPx = 0.5;
static const float kFeatherPx   = 1.0;

cbuffer GraphGridCB : register(b0)
{
    float2 gPhasePx;      // origin (canvas space) * zoom -- the pan term
    float  gZoom;         // node-editor view scale (1 = 100%)
    float  gPad0;
    float4 gCanvasColor;  // backdrop tone (a = unused, written opaque)
    float4 gMinorColor;   // a = peak strength of the minor octave
    float4 gMajorColor;   // a = peak strength of the major octave
};

struct VSOutput
{
    float4 pos : SV_Position;
};

VSOutput vs_main(uint vid : SV_VertexID)
{
    VSOutput o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Anti-aliased coverage of the axis-aligned lattice of period `period`
// (screen pixels) at grid coordinate `q` (screen pixels). Distance is exact in
// pixels because q is already a pixel coordinate, so no ddx/ddy is needed.
float LineCoverage(float2 q, float period)
{
    float2 d = abs(frac(q / period + 0.5) - 0.5) * period;   // px to nearest line
    float  f = min(d.x, d.y);
    return 1.0 - smoothstep(kHalfWidthPx, kHalfWidthPx + kFeatherPx, f);
}

float4 ps_main(VSOutput i) : SV_Target0
{
    // Pan-locked grid coordinate (see the coordinate contract above).
    float2 q = i.pos.xy + gPhasePx;

    // Octave LOD. `pm` is the snapped minor period; it always satisfies
    // pm = kMinorTargetPx * 2^-t for t in [0,1), i.e. pm in (T/2, T].
    float basePeriod = kBaseSpacingPx * pow(max(gZoom, 1e-4), kZoomExponent);
    float level      = log2(kMinorTargetPx / max(basePeriod, 1e-4));
    float step0      = floor(level);
    float t          = level - step0;          // 0 -> just stepped, 1 -> about to
    float pm         = basePeriod * exp2(step0);

    // The classic pop-free pair. Lines at 2*pm are a strict SUBSET of the lines
    // at pm, so drawing pm at (1-t) and 2*pm at full strength fades exactly the
    // "odd" lines out as the octave boundary approaches. At the boundary the
    // survivors are the 2*pm lines at full strength -- which is precisely the
    // next octave's pm at t = 0. Nothing appears, nothing jumps.
    float minor = max(LineCoverage(q, pm) * (1.0 - t),
                      LineCoverage(q, pm * 2.0));

    // Majors ride the same fade one power of two apart, so the 8:1 look holds
    // across the boundary for free (an 8x target differs by exactly 3 octaves,
    // which leaves `t` identical).
    float major = max(LineCoverage(q, pm * kMajorEvery) * (1.0 - t),
                      LineCoverage(q, pm * kMajorEvery * 2.0));

    float3 col = gCanvasColor.rgb;
    col = lerp(col, gMinorColor.rgb, minor * gMinorColor.a);
    col = lerp(col, gMajorColor.rgb, major * gMajorColor.a);
    return float4(col, 1.0);
}

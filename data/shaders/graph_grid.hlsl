// Shader-graph canvas backdrop: an infinite, LOD-faded two-octave line grid
// drawn as ONE fullscreen triangle into an editor-owned, canvas-sized RT.
// The editor blits the RT under the node-editor's content and disables the
// vendored editor's own grid (StyleColor_Grid alpha 0), so this is the only
// backdrop the canvas has.
//
// COORDINATE CONTRACT (the whole design in one paragraph):
//   Screen period = kBaseSpacingPx * pow(zoom, kZoomExponent). At
//   kZoomExponent = 1 the grid would be rigidly welded to canvas content
//   (lines through fixed canvas coordinates); at 0 it would never change size.
//   We want "visibly responds, much less than 1:1", so the exponent is
//   fractional -- see kZoomExponent below.
//
//   THE CONSEQUENCE, and it is the whole reason gPhasePx exists: a sublinear
//   period corresponds to NO fixed canvas-space lattice. The grid is a VIRTUAL
//   lattice, so its phase is a free choice rather than something derivable
//   from the view -- and derived-from-the-view is exactly the bug that produced
//   corner-anchored zoom. The phase is therefore STATE, owned and evolved on
//   the CPU by GraphGridPass::UpdatePhase (GraphGridPass.hpp): panning slides
//   it by the screen-space pan delta, and a zoom step rescales it about that
//   step's own screen-space fixed point, so the pattern appears to grow out of
//   whatever the editor zoomed about.
//
//   gPhasePx is that state: the screen-space position (in pixels, relative to
//   the RT's top-left, which is the canvas region's top-left) of the lattice's
//   origin. So the grid coordinate at pixel p is simply q = p - gPhasePx.
//
//   THE LATTICE IS THEREFORE VIRTUAL, and that is a visual fact, not just an
//   implementation one: it is a rendering convenience that corresponds to no
//   coordinate in the document, and it does not stay registered with anything
//   that does.
//
//   DO NOT ADD REAL-TRANSFORM FEATURES TO THIS BACKDROP. Canvas-space origin
//   axes (drawn on the editor's actual p = (c - O) * Z) were built and cut at
//   the desk: they were correct, and correctness was the problem -- a real
//   canvas feature and the virtual lattice necessarily slide against each other
//   at any zoom but 100%, and that reads as a rendering bug rather than as the
//   two honest coordinate systems it actually is. Anything welded to canvas
//   space belongs in the node layer, not here.
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
// 0.7 (desk call, RAISED from an initial 0.4 that tracked the view too
// loosely): halving the zoom shrinks the spacing to 0.5^0.7 = 0.62x and
// doubling grows it to 1.62x, so the grid visibly follows the view instead of
// merely nodding at it -- while still staying well clear of the 2x/0.5x lurch
// a welded grid would give.
//
// k also sets how often the octave LOD steps: an octave is 2^(1/k) of zoom, so
// a sweep of the whole domain crosses k*log2(domain ratio) boundaries. Over
// Unreal's 0.1-2.0 zoom table (ShaderEditorDocument.cpp, kZoomLevels) that is
// 0.7*log2(20) = 3.0 crossings, up from the 1.7 that 0.4 gave.
//
// MORE CROSSINGS IS NOT A RISK, and this is the load-bearing part of raising
// k: the crossfade below is a SUBSET argument, not a tuned blend. Lines at
// 2*pm are exactly every other line of pm, for any k, so a crossing can only
// ever fade out lines that are already redundant. More crossings therefore
// means more fully-faded transitions, never a pop. All k changes is how fast
// `t` walks [0,1): one 1.1x wheel notch now moves it 0.7*log2(1.1) = 0.10
// (was 0.06) -- still a gentle ramp, not a blink.
//
// Nor does the domain change density: the octave snap normalizes the period at
// EVERY zoom -- pm always lands in (kMinorTargetPx/2, kMinorTargetPx] -- which
// is why kBaseSpacingPx needed no compensating adjustment for this change.
// Worked endpoints at k = 0.7: pm = 16.0 px at zoom 0.100, 20.0 px at 1.000,
// 16.2 px at 2.000 -- the whole domain sits inside the design band.
static const float kZoomExponent = 0.7;

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

// ---- Vignette -------------------------------------------------------------
// Restrained radial darkening toward the region's edges -- a finish, not a
// spotlight. kVignetteStrength is the fraction of brightness removed at the
// far end of the ramp; the ramp itself runs over a radius normalized SEPARATELY
// per axis, which is what makes the falloff an ellipse matching the panel
// instead of a circle that crops badly in a wide dock.
static const float kVignetteStrength = 0.12;
static const float kVignetteInner    = 0.55;   // radius where darkening starts
static const float kVignetteOuter    = 1.25;   // radius of full strength (corner ~1.41)

// ---- Dither ---------------------------------------------------------------
// One LSB of ordered noise, applied last. The backdrop is RGBA8_UNORM and its
// tones are dark and very close together (canvas 0.118 vs minor 0.180), so both
// the vignette ramp AND the flat background quantize into visible bands without
// this.
//
// Interleaved Gradient Noise (Jimenez, "Next Generation Post Processing in Call
// of Duty: Advanced Warfare") rather than a value hash: it is the same single
// line, its spectrum is far closer to blue than a hash's white, and -- the
// reason that matters here -- it is a pure function of the pixel. The backdrop
// RT is CACHED and only re-rendered when the view actually changes
// (GraphGridPass::Update), so a frame-varying dither would make an idle canvas
// and a re-rendered one disagree; a deterministic one makes re-renders
// idempotent.
static const float kDitherAmount = 1.0 / 255.0;

cbuffer GraphGridCB : register(b0)
{
    float2 gPhasePx;      // lattice origin, in RT pixels -- the STATE, see above
    float  gZoom;         // node-editor view scale (1 = 100%)
    float  gPad0;
    float2 gViewSizePx;   // the visible REGION size (the RT is over-allocated)
    float2 gPad1;         // HLSL will not straddle a float4 across offset 32
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

// Interleaved Gradient Noise -- see kDitherAmount for why this one.
float InterleavedGradientNoise(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float4 ps_main(VSOutput i) : SV_Target0
{
    // Grid coordinate: the pixel measured from the lattice origin. gPhasePx
    // carries every view response there is -- pan and the zoom re-anchor both
    // land in it CPU-side (see the coordinate contract above).
    float2 q = i.pos.xy - gPhasePx;

    // Octave LOD. `pm` is the snapped minor period; it always satisfies
    // pm = kMinorTargetPx * 2^-t for t in [0,1), i.e. pm in (T/2, T].
    float basePeriod = kBaseSpacingPx * pow(max(gZoom, 1e-4), kZoomExponent);
    float level      = log2(kMinorTargetPx / max(basePeriod, 1e-4));
    float step0      = floor(level);
    float t          = level - step0;          // 0 -> just stepped, 1 -> about to
    float pm         = basePeriod * exp2(step0);

    // The classic pop-free pair. Lines at 2*pm are a strict SUBSET of the lines
    // at pm, so drawing pm at (1-t) and 2*pm at full strength fades exactly the
    // "odd" ones out as the octave boundary approaches. At the boundary the
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

    // Vignette over the VISIBLE region (not the over-allocated RT), normalized
    // per axis so the falloff is an ellipse fitted to the panel.
    float2 halfSize = max(gViewSizePx, 1.0) * 0.5;   // NB: `half` is an HLSL type
    float  r        = length((i.pos.xy - halfSize) / halfSize);
    col *= 1.0 - kVignetteStrength * smoothstep(kVignetteInner, kVignetteOuter, r);

    // Dither last: it exists to break up the quantization of everything above.
    col += (InterleavedGradientNoise(i.pos.xy) - 0.5) * kDitherAmount;

    return float4(col, 1.0);
}

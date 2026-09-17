// Grid shader: THE 3D REFERENCE GRID (F4 plan 1 Task 10, spec s5.2) -- an
// ANALYTIC grid on one large ground quad, depth-tested against the opaque
// pass's depth target (LESS_OR_EQUAL, no depth write) and alpha-blended
// (straight alpha) into the linear RGBA16F canvas after the mesh pass.
//
// THE MODEL IS UNREAL'S EDITOR GRID (FGridWidget::DrawNewGrid, Editor/
// UnrealEd/Private/EditorComponents.cpp -- a material-instance quad whose
// UAxisColor/VAxisColor and FmodFloor'd UV origin are set there): one quad
// centred under the camera whose ORIGIN WRAPS to a major-cell multiple
// (UE's FmodFloor) so its edges always lie on major lines, extent scaled
// with the camera's altitude so flying up never reveals its edge, minor and
// major lines derived from screen-space derivatives (fwidth) so a line is
// ~1 px wide at every distance, the two in-plane ORIGIN AXES drawn in colour
// over the lines, a fade with distance and a fade at grazing angles.
//
// NO VERTEX BUFFER: vs_main builds the quad from SV_VertexID (6 vertices, two
// triangles) and the C++ side issues CmdDraw(6). NO textures, NO sampler, NO
// root/push constants -- everything travels in ONE ordinary per-frame constant
// buffer (b1, space1 -- the same slot and space MeshNode's frame CB uses, so
// the SPIR-V shift table compile-shaders.bat carries for space1 already
// covers it). The block is 160 bytes, past Vulkan's 128-byte push-constant
// minimum, which is why it is a CB and not a push block at all.
//
// UNITS are METERS (MKS); +Y is UP (spec s2). Plane 0 = XZ (the 3D ground,
// normal +Y); plane 1 = XY (the 2D authoring plane, normal +Z). The in-plane
// U axis is world X on both; the V axis is Z (XZ) or Y (XY). Which COLOUR
// each axis draws in is the CALLER's (GridSceneDesc::axisUColor/axisVColor)
// -- this shader is plane-agnostic on colour and only on geometry does it
// branch on the plane id.
//
// MATRIX PACKING: column-major, dxc's default on both targets and glm's
// memory layout, so mul(M, v) is the ordinary M * v (same note as mesh.hlsl).

cbuffer GridFrameCB : register(b1, space1)
{
    float4x4 g_viewProjection;
    float4   g_eyeAndPlane;   // xyz: the eye in world space; w: plane id (0 = XZ, 1 = XY)
    float4   g_params;        // x: minor spacing (m), y: major every (m),
                              // z: fade distance (m), w: quad half-extent (m)
    float4   g_minorColor;    // straight alpha, linear
    float4   g_majorColor;
    float4   g_axisUColor;    // the world-X origin axis
    float4   g_axisVColor;    // the world-Z (XZ) or world-Y (XY) origin axis
};

struct VSOutput
{
    float4 pos   : SV_Position;
    float3 world : TEXCOORD0;   // world-space position on the plane
};

// In-plane (u, v) -> world, and back. XZ: (u, v) = (x, z), height y = 0.
// XY: (u, v) = (x, y), depth z = 0.
float3 PlaneToWorld(float2 uv, float planeId)
{
    return planeId < 0.5 ? float3(uv.x, 0.0, uv.y) : float3(uv.x, uv.y, 0.0);
}

float2 WorldToPlane(float3 world, float planeId)
{
    return planeId < 0.5 ? world.xz : world.xy;
}

float3 PlaneNormal(float planeId)
{
    return planeId < 0.5 ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
}

VSOutput vs_main(uint vertexId : SV_VertexID)
{
    // Two triangles, six vertices, a unit square in (u, v) in [-1, 1].
    // Winding is irrelevant: the pipeline culls NOTHING (the grid is seen from
    // both sides of the plane, and the XY plane is looked at edge-on and from
    // behind by an orbiting camera).
    const float2 corners[6] = {
        float2(-1.0, -1.0), float2( 1.0, -1.0), float2( 1.0,  1.0),
        float2(-1.0, -1.0), float2( 1.0,  1.0), float2(-1.0,  1.0)
    };
    const float  planeId    = g_eyeAndPlane.w;
    const float  majorEvery = g_params.y;
    const float  halfExtent = g_params.w;

    // THE WRAP (UE's FmodFloor): the quad is centred under the eye, snapped
    // DOWN to a major-cell multiple so its edges fall exactly on major lines
    // rather than cutting a cell part-way -- the pattern itself is
    // world-anchored (ps_main reads the interpolated WORLD position), so this
    // only moves where the quad ends, never where the lines are.
    const float2 eyeUV  = WorldToPlane(g_eyeAndPlane.xyz, planeId);
    const float2 centre = floor(eyeUV / majorEvery) * majorEvery;

    const float2 uv = centre + corners[vertexId] * halfExtent;

    VSOutput output;
    output.world = PlaneToWorld(uv, planeId);
    output.pos   = mul(g_viewProjection, float4(output.world, 1.0));
    return output;
}

// Distance to the nearest line of a grid with spacing `spacing`, in PIXELS,
// and the 0..1 line coverage that follows from it: 1.0 on the line, falling
// to 0 one pixel away. `fwidth` is the screen-space derivative of the plane
// coordinate, which is what makes the line ~1 px wide at every distance and
// every viewing angle -- the standard analytic-grid construction (also UE's
// material grid, and Ben Golus's "The Best Darn Grid Shader (Yet)").
float LineCoverage(float2 coord, float2 dcoord)
{
    const float2 g = abs(frac(coord - 0.5) - 0.5) / dcoord;
    return 1.0 - min(min(g.x, g.y), 1.0);
}

float4 ps_main(VSOutput input) : SV_Target0
{
    const float  planeId      = g_eyeAndPlane.w;
    const float  minorSpacing = g_params.x;
    const float  majorEvery   = g_params.y;
    const float  fadeDistance = g_params.z;

    const float2 planePos = WorldToPlane(input.world, planeId);

    // Minor lines every `minorSpacing`, major every `majorEvery` (metres).
    // fwidth is taken on the PLANE coordinate in metres and divided out
    // per level, so each level's line is one pixel wide in ITS units.
    // Floored away from zero: fwidth is never zero on a non-degenerate
    // quad, but 0/0 at the exact axis pixel would be NaN, not a line.
    const float2 dPlane   = max(fwidth(planePos), 1e-6);
    float        minorLine = LineCoverage(planePos / minorSpacing, dPlane / minorSpacing);
    float        majorLine = LineCoverage(planePos / majorEvery,   dPlane / majorEvery);

    // THE DENSITY FADE: a level whose cell spans fewer than ~kFadeCellPx
    // pixels on screen has lines closer together than they are wide, and
    // without this it collapses into a solid moire band toward the horizon
    // (the desk check that motivated it). Each level dissolves linearly as
    // its cell shrinks from kFadeCellPx to half that -- the same crossfade
    // idea the 2D grid's decade levels use (ViewportGrid.hpp), and what UE's
    // material grid does with its per-level "fade" terms.
    const float  kFadeCellPx = 8.0;
    const float2 minorCellPx = minorSpacing / dPlane;
    const float2 majorCellPx = majorEvery   / dPlane;
    minorLine *= saturate(min(minorCellPx.x, minorCellPx.y) / kFadeCellPx * 2.0 - 1.0);
    majorLine *= saturate(min(majorCellPx.x, majorCellPx.y) / kFadeCellPx * 2.0 - 1.0);

    // THE TWO ORIGIN AXES: the V axis is where u == 0 (a line along V through
    // the origin), the U axis where v == 0. Drawn OVER the grid lines in
    // their own colours (UE's UAxisColor / VAxisColor). Slightly wider than a
    // grid line (1.5 px) so they read as axes rather than as one more line.
    const float2 axisPx = abs(planePos) / dPlane;
    const float  vAxis  = 1.0 - saturate(axisPx.x - 0.5);   // |u| within ~1.5 px of 0
    const float  uAxis  = 1.0 - saturate(axisPx.y - 0.5);   // |v| within ~1.5 px of 0

    // THE FADES. Distance: linear to zero at fadeDistance (ours; UE relies on
    // its quad's extent + fog). Grazing: the grid dissolves as the view ray
    // flattens onto the plane, where fwidth explodes and every line would
    // otherwise smear into a solid band at the horizon.
    const float3 toEye   = g_eyeAndPlane.xyz - input.world;
    const float  dist    = length(toEye);
    const float  fade    = saturate(1.0 - dist / fadeDistance);
    const float  grazing = saturate(abs(dot(toEye / max(dist, 1e-4), PlaneNormal(planeId))) * 4.0);

    // Compose: minor under major under the axes.
    float4 color = lerp(g_minorColor, g_majorColor, majorLine);
    color.a     *= max(minorLine, majorLine);

    float4 axisColor = g_axisUColor;
    axisColor.a     *= uAxis;
    float4 vAxisColor = g_axisVColor;
    vAxisColor.a     *= vAxis;
    // The V axis wins over the U axis at the origin pixel (arbitrary, stable).
    axisColor = lerp(axisColor, vAxisColor, vAxisColor.a > axisColor.a ? 1.0 : 0.0);

    // Take the axis where it is present; the grid line elsewhere. Straight
    // alpha out: the pipeline blends src.a / 1-src.a.
    color = axisColor.a > color.a ? axisColor : color;
    color.a *= fade * grazing;
    return color;
}

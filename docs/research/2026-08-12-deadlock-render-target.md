# Deadlock-class render target — the finished 3D renderer's feature contract

**User directive (2026-08-12): the finished Arcane 3D renderer must support
what Valve's Deadlock renders in its latest form.** This document turns that
sentence into a concrete feature contract plus a per-feature reference map.
It scopes the follow-on arcs AFTER the NRI migration
(`docs/specs/2026-08-12-nri-adoption-design.md`); the migration's
foundational 3D slice is unchanged.

Compiled 2026-08-12 from three parallel research passes over public sources
only. Full agent reports are archived in the session record; the load-bearing
citations are inlined here.

## Reference-code policy (binding)

Source 2 has no published or licensed source, ever. Leaked Valve code exists
in the wild and is **off-limits — never sought, never consulted, never
referenced**; using it would legally taint the engine. The legal reference
channels, in order of usefulness:

1. **ValveResourceFormat** (github.com/ValveResourceFormat/ValveResourceFormat,
   MIT, clean-room): parses Source 2 formats AND reimplements the renderer
   readably (`Renderer/Renderer/` C#, `Renderer/Shaders/` Slang). Its code is
   MIT-reusable with attribution; the format behavior it documents is fact.
   Deadlock's internal codename is **citadel** — its shaders live in VRF as
   `pbr.frag.slang`, `environment_blend.frag.slang`, `citadel_overlay.frag.slang`,
   and above all **`common/citadel.slang`** (Deadlock's NPR lighting math).
2. **Valve's published material**: VDC wiki (CS2/HLA Workshop Tools lighting
   docs = the fullest public Source 2 lighting spec), patch notes, the TF2
   NPAR 2007 paper, SIGGRAPH 2006 Source shading chapter, GDC 2015/2016
   Vlachos + Ginsburg talks. No public Alyx *rendering* talk exists.
3. **Unreal source** — read-only reference (Epic EULA permits reading via
   linked GitHub, NOT copying into a non-UE engine). Checkout recipe:
   blobless sparse clone of `release` limited to `Engine/Source/Runtime/
   {Renderer,RenderCore,RHI}`, `Engine/Shaders`, and the two Lightmass dirs
   (~1 GB; NEVER run Setup.bat) into `D:\dev\starworks\Arcane\.example\`.
4. **Legally liftable code** where noted: Intel XeGTAO (MIT), Playdead TAA
   (MIT), MJP's BakingLab/Shadows samples, Filament (Apache-2.0, incl. its
   frame graph `filament/src/fg/`), Godot (MIT).

## What Deadlock actually is (corrections to our assumptions)

- **Deferred, not forward+.** VDC: Dota 2 and Deadlock run Source 2's
  deferred renderer; CS2/Alyx/s&box run forward. Confirmed independently by
  a RenderDoc capture showing a character rim-light pass after the deferred
  lighting resolve. Consequence: no MSAA; TAA-family AA only.
- **DX11 default + Vulkan, no DX12** (Valve dev redirected a DX12 request to
  `-vulkan`). We are on D3D12+VK via NRI — strictly ahead on API, and the
  feature set above the RHI is what parity means.
- **GI is fully baked.** VRAD3 bakes on GPU hardware ray tracing into paged
  lightmaps + light probe volumes + box-projected cubemaps. No realtime GI,
  no RT effects, no time-of-day. Valve ships relights as content patches.
- **"Barycentric lightmapping" is folklore** — no public source uses the
  term for any Source 2 title. Do not build against it. The Deadlock-era
  lightmap format (v8.3/8.4 per VRF) is **L1 SH2 per color channel**
  (`directional_irradiance_sh2_dc/_r/_g/_b`); the CS2-era one (≤8.2) is AHD
  with baked specular-occlusion in alpha.
- Deadlock keeps a **scene distance field** feeding four features at once:
  DF shadows, DF reflections, DF AO, and the hero **outlines**
  (`r_citadel_outlines`, distance-field-driven, not inverted-hull).

## The feature contract, by arc tier

Each tier is a candidate follow-on arc (or two). "Refs" = where to learn it:
VRF paths are readable reimplementation; UE paths are read-only; papers and
liftable code as noted.

### T1 — Cluster light grid + material system (the substrate everything shares)

| Feature | Deadlock evidence | Refs |
|---|---|---|
| One compute-built cluster/froxel light grid consumed by opaque shading, fog, translucency | Deferred w/ "almost infinite" dynamic lights (VDC); tiled-vs-clustered specifics unpublished | UE `LightGridInjection.cpp/.usf` (exp Z slicing, linked-list→compact); aortiz.me primer; Granite posts; Godot `RenderForwardClustered`; VRF `LightBinner.cs` + `compute_tile_cullbits.comp` |
| **Forward+ vs deferred: explicit decision owed at T1 spec time.** Build the grid once (it serves both); the space game's light counts + no-MSAA/TAA acceptance argue deferred; forward+ keeps transparency/MSAA simpler. Deadlock validates deferred; Alyx validates forward on the same data. | VDC deferred-renderer page trade-off list | UE ships both consumers of one grid — mirror that optionality |
| PBR metalness/roughness GGX, F0=mix(0.04, albedo, metal), multiscatter EnvBRDF | VRF `complex`/`pbr` shader census | Filament PBR doc (the math bible); Karis "Real Shading in UE4"; VRF `common/pbr.slang` |
| Material feature system: F_ features → static combos, dynamic combos at runtime, per-material dynamic expressions | `.vmat_c`/`.vcs` structure (VRF CompiledShader/) | Maps onto our shader-graph/material system; combo model = our permutation plan's benchmark |
| Light types: directional, omni (sphere/tube), spot w/ cookies ("barn"), rect area lights (Deadlock menu toggle), ortho; per-light Static/Stationary/Per-Pixel modes | CS2 lighting docs; Deadlock "Area Lights" setting | VRF `SceneLight.cs` (directlight modes, bakedshadowindex); UE stationary-light split; LTC papers for area lights |

### T2 — Baked GI (the biggest new subsystem: our VRAD3-equivalent)

| Feature | Deadlock evidence | Refs |
|---|---|---|
| GPU-raytraced offline lightmap baker | VRAD3 requires RTX/RDNA2+ (VDC) | UE `GPULightmass` plugin (path-traced baker, small read); MJP BakingLab (liftable) |
| Directional lightmaps: start AHD (irradiance + direction + directionality, spec-occlusion in alpha), upgrade path to L1 SH2 per channel | Lightmap v8.2 vs v8.3/8.4 (VRF tables) | VRF `common/lighting.slang` `ComputeLightmapShading`; UE `LightmapCommon.ush` HQ decode; McTaggart 2006 (radiosity normal mapping ancestor) |
| Paged lightmap arrays (512² pages, `sampler2DArray`, dedicated centroid UV stream) | VRF `WorldLoader.cs` lightmap sets | Same |
| Light probe volumes for dynamics: 3D textures, ambient-cube (6-slice) or SH; atlas packing; OBB-normalized sampling; indoor/outdoor priority + edge fades | `env_light_probe_volume` (VRF `SceneLightProbe.cs`, `lighting.lpv.slang`) | UE `PrecomputedVolumetricLightmap.cpp` (sparse SH bricks + indirection) — richer than Source 2's; choose per bake-cost |
| **Combined probe+cubemap entity** (one placed volume = probe grid + reflection cubemap + fade rules) — steal this authoring design | `env_combined_light_probe_volume` (VDC + VRF) | — |
| Baked direct-light data: 4-channel one-hot shadow mask (sun via `g_vSunLightBakedShadowMask` dot; realtime lights pick a channel) | Lightmap v8.2+ `direct_light_shadows` (VRF) | The stationary-light trick that makes "baked shadows + realtime specular" cheap — UE's analog is its screen-space shadow-mask channels |

### T3 — Reflections + shadows

| Feature | Deadlock evidence | Refs |
|---|---|---|
| Box-projected cubemap reflection ARRAY (144 max), priority blending to weight 0.99, edge fades, **SH-based specular normalization vs baked diffuse** (brighten capped by roughness, darken uncapped) | VRF `SceneEnvMap.cs`, `common/environment.slang` | Lagarde parallax-corrected cubemaps (THE paper); UE `ReflectionEnvironment*`; Filament IBL |
| Stable CSM for the sun: sphere-fit cascades + texel snapping, PCF, faded transitions; bicubic lightmap filtering at high settings | "Improved Cascade Shadowmaps" (VDC Deadlock); 2025-02 patch | MJP Shadows sample (liftable) + "A Sampling of Shadow Techniques"; Valient ShaderX6; UE `ShadowSetup.cpp`. **VSM verdict: skip** (Nanite-coupled; wrong for this class) |
| Per-light shadow atlas (global; shadowed omni = 4 slots) | CS2 docs; VRF `ShadowMapper.cs`/`ShadowAtlasPacker.cs` | UE shadow atlasing in `ShadowDepthRendering.cpp` |
| **Scene distance field tier**: DF shadows (far/soft), DF AO, DF reflections, and outlines all from one SDF representation | Deadlock menu toggles + `r_distancefield_enable` prereq for outlines | UE Distance Field pipeline (`DistanceFieldLightingShared.ush` etc.) — the architectural model; big infra, its own arc |
| Screen-space reflections overlay w/ capture fallback | Standard Source 2/Deadlock glass improvements | UE `ScreenSpaceRayTracing.cpp`; Stachowiak stochastic SSR; McGuire/Mara DDA |

### T4 — Atmosphere

| Feature | Deadlock evidence | Refs |
|---|---|---|
| Froxel volumetric fog + shadowed godrays (sun shafts measurable down-lane) | "Fog Quality", `r_enable_volume_fog`; VDC "high-fidelity godrays" | Wronski SIGGRAPH 2014 (source paper); Hillaire Frostbite 2015; UE `VolumetricFog.cpp/.usf`; VRF has the uniform list (present-but-disabled) |
| Gradient fog (distance × height band, exponent-shaped) — cheap analytic layer | `env_gradient_fog` exact semantics in VRF `fog.slang` | Trivial; implement from VRF semantics |
| **Cubemap "MIP fog"**: fog color from a cubemap, LOD driven by fog density (farther = blurrier mip) — huge look-per-cost, distinctly Source 2 | `env_cubemap_fog` (VDC + VRF exact math) | Implement from VRF semantics |
| 3D skybox (nested miniature scene) + 2D sky; imposter window interiors (interior mapping) | The Cursed Apple backdrop; 2025-02 patch | Interior mapping: van Dongen's paper; 3D skybox = second scene render at scale |

### T5 — Post stack + AA

| Feature | Deadlock evidence | Refs |
|---|---|---|
| HDR internal + auto-exposure (histogram, asymmetric speeds), SDR output; **pre-exposure** for fp16 range | VDC HDR page; Source 2 tonemap chain | UE `PostProcessEyeAdaptation.cpp` + 4.25 tech blog; VRF `PostProcessRenderer.cs` (10-frame weighted history) |
| Parametric filmic tonemap (Source 2 uses Hable/Uncharted params incl. CPU-inverted white point) + up to 4 blended 3D grading LUTs | VRF `.vpost_c` TonemapSettings + `combine_luts.comp` | Ours already tonemaps (2D arc); extend to parametric curve + LUT blend; Narkowicz ACES fit as bring-up alternative |
| **Split bloom: world-light bloom vs ability/effects bloom** (competitive-clarity design; materials can override bloom amount — `F_OVERRIDE_BLOOM_AMOUNT`) | Deadlock menu: "Post Process Bloom" + "Effects Bloom" | Gaussian pyramid (Jimenez CoD:AW); skip FFT bloom |
| AO two tiers: SSAO (cheap) + DF AO (from the T3 SDF) — build GTAO as the SSAO tier | Deadlock menu toggles | XeGTAO (MIT, liftable); ATVI GTAO report |
| DoF + motion blur toggles | Menu census | VRF `dof2.*`; standard implementations |
| TAA first-class (no MSAA in deferred), FXAA cheap option | Deadlock native TAA | Karis 2014 spec + Tardif starter pack + Playdead code (MIT); full checklist in the agent report (jitter/velocity dilation/Catmull-Rom/YCoCg variance clip/luma weighting) |
| Upscalers: FSR-class later, vendor-neutral first (NIS already in NRI vendoring plan); DLSS/Reflex = out (licensing rulings stand) | Deadlock ships FSR 1/2/3.1 + DLSS/DLAA + Reflex/Anti-Lag 2.0 | NRI upsclaer extensions when wanted |
| Transparency: sorted alpha first; **MBOIT** as the eventual OIT (Deadlock ships it flagged WIP) | Menu census | Münstermann I3D 2018 paper |

### T6 — The readability/NPR layer (the "Deadlock look")

This is where the directive bites hardest, and we have actual math via VRF's
`common/citadel.slang` (MIT):

| Feature | What it does | Refs |
|---|---|---|
| NPR toon diffuse | Light wrap + S-curve gain step (`g_flNPRDiffuseStepSharpness`), **blendable back to Lambert** (`g_flNPRDiffusePbrBlend`) — stylization as a dial, not a fork | `citadel.slang` NprToonDiffuse |
| Stepped specular | GGX quantized into N bands with energy renormalization | NprSteppedSpecular |
| Specular tint dial | Desaturated spec → albedo-tinted, renormalized reflectance | NprSpecularTint |
| Up-gated rim light | Rim term gated by world-up ramp ("tops of shoulders and heads"), optional depth occlusion, **rim mask in a texture channel** (`g_tTintMaskRimLightMask`.y); ships as a separate post-deferred pass | NprRimLight; RenderDoc capture evidence |
| NPR exposure targets | Characters get their own exposure clamp vs world auto-exposure | `g_vNPRExposureTargets` |
| DF outlines + through-wall glow | Hero/structure outlines from the scene SDF (~600u cutoff), x-ray proximity glow ("Visibility Outlines", 2026-01 patch) | T3's SDF; `r_citadel_*` cvars |
| World stylization | Hand-painted albedo discipline over honest PBR (not a toon pipeline); per-layer color-correct matrices in the world shader; TF2 doctrine (luminance/hue readability) | TF2 NPAR 2007; Dota 2 shader-mask spec; VRF `environment_blend` color matrices |
| Character shader as a distinct thing | Valve added a dedicated readability character shader (2024-10 patch) decoupled from world lighting | Model: one shading-model dispatch (UE `ShadingModels.ush` pattern), data-driven dials |

### T7 — World-rendering systems (as needed by the space game)

Aggregate/pre-batched static scene objects; bake-time object→probe/envmap
handshakes (precomputed binding beats runtime lookup); voxel visibility;
GPU occlusion culling (depth pyramid); decal/overlay shader with per-target
translucency remaps (`citadel_overlay`); displacement mapping; shader
precache pass to kill in-match compile hitches (Deadlock 2025-02 — our
PSO-cache warmup analog); MBOIT (T5).

## Explicitly NOT parity goals

DX11 (we're D3D12/VK — ahead), DLSS/Reflex/Anti-Lag (vendor SDK licensing
rulings stand; NIS + later FSR cover upscaling), CS2 voxel responsive smokes
(CS2-only, not in Deadlock), HDR display output (Source 2 doesn't either;
our tonemap direction already tracks it as future), realtime GI (Deadlock
has none — our GI arc is a BAKER, not Lumen).

## Sequencing

NRI migration (spec'd) → foundational 3D slice → **T1 (grid + PBR + the
forward/deferred decision) → T2 (baker + lightmaps + probes) → T3
(reflections + CSM + atlas; SDF tier can trail) → T4/T5 in either order →
T6 rides on all of it** (rim/outline/NPR dials need T1's material system,
T3's SDF, T5's exposure). T7 items slot in where the space game demands.
Each tier gets its own spec arc; this document is their shared target
contract. The pattern to copy from Valve's ops: ship relights as content,
treat readability features as renderer work.

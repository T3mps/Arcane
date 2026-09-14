# Source 2 renderer — public origins, legal sources, and what it was based on

**Date:** 2026-09-14
**Status:** research — citation-backed. Binding policy only in §1.
**Scope:** **rendering only.** 3D physics is later (Box3D). This exists so a year-end Deadlock-class *picture* can be implemented from legal sources.
**Read with:** `docs/research/2026-08-12-deadlock-render-target.md` (feature contract), `docs/research/2026-09-14-engine-ceiling-deadlock-and-box3d.md` (ceiling + sequencing).

This pass re-fetched public pages. It does **not** reopen leaked Source 2 trees. Where the 2026-08-12 contract cited something this pass could not re-verify, that is marked **UNRECHECKED**.

---

## 1. Legal channels (unchanged, restated)

Source 2 has **no published engine source**. Valve does not license Source 2 as code. Using leaked Valve trees would taint Arcane. Do not seek, clone, quote, or “just peek.”

| Channel | What it is | How we may use it |
|---|---|---|
| **ValveResourceFormat / Source 2 Viewer** (MIT) | Clean-room parser + **OpenGL reimplementation** of Source 2 asset rendering. README: *“based entirely on reverse engineering as Valve does not provide Source 2 documentation or code.”* | Read shaders/C# for *behavior*. Copy/adapt **only with MIT attribution** (`Powered by Source 2 Viewer (ValveResourceFormat)`). Do not present S2 formats as original research. https://github.com/ValveResourceFormat/ValveResourceFormat |
| **Valve published talks / PDFs** | GDC, SIGGRAPH, Steam Dev Days, workshop patch notes | Implement from the paper. Cite. |
| **Valve Developer Community** | Mix of Valve workshop docs and community wiki. Live site currently Anubis-gated; use Wayback when scraping. | Facts about *tools and lighting modes*. Wiki prose is not Valve source. |
| **Epic GitHub `release`** | Read-only under Epic EULA | Architecture reference. **Not** copy into Arcane. |
| **Liftable MIT/Apache code** | Filament PBR, XeGTAO, Playdead TAA, MJP samples, Godot | Copy with license file. |

**Not channels:** leaked `engine2.dll` trees, private Rubikon-Lite, “Source 2 SDK dump,” Discord dumps of `.vcs` compiler internals beyond what VRF already published.

---

## 2. What Source 2 was based on (rendering)

### 2.1 Direct answer

**Source 2’s renderer is Valve’s own successor to Source 1, not a licensed third-party engine.** There is no public evidence it is a fork of Unreal, id Tech, CryEngine, or Frostbite.

What *is* public:

1. **Gabe Newell, Nov 2012** (Eurogamer, 4chan office visit): asked if Source 2 would be “more than just an extension to Source” / “an entirely new engine,” he answered **“Yeah!”**  
   https://www.eurogamer.net/gabe-newell-confirms-valve-working-on-source-2

2. **Official announcement, GDC 2015:** successor to Source; creator-productivity focus; Vulkan-compatible version; free for Steam-published games. Jay Stelly (Polygon/IGN/Gematsu coverage of the same press line). First shipping game: *Dota 2 Reborn*, September 2015.

3. **Dan Ginsburg, Valve, “Porting Source 2 to Vulkan” (2015/2016 Khronos/GDC):** the **only Valve slide deck this pass opened that describes the renderer architecture.**
   - APIs then: OpenGL, DX9, DX11, Vulkan
   - Platforms: Windows, Linux, Mac; product: Dota 2 Reborn
   - **“DX11-like rendersystem abstraction”**
   - Multithreaded: DX9/GL = software command buffers; DX11 = deferred contexts; **single submission thread**
   - Vulkan port **started from the GL and DX11 renderer**; DX11 deferred contexts mapped well to Vulkan command buffers; shaders HLSL → GLSL → SPIR-V  
   PDF: https://nextgenapis.realtimerendering.com/presentations/6_Ginsburg_Source2.pdf

4. **Multiple renderer flavors in one engine** (VDC, community + workshop, not a Valve SIGGRAPH paper):
   - *Dota 2:* **deferred lighting, dynamic, does not need to be compiled.**  
     https://web.archive.org/web/20211210182908/https://developer.valvesoftware.com/wiki/Dota_2_Workshop_Tools/Level_Design/Dota/Lighting
   - *SteamVR Home:* VDC says Source 2 “appears to be designed with **multiple different renderers** in mind. SteamVR Home uses a **forward** renderer tailored for VR, while Dota 2 uses a **deferred** renderer.” The **same page** then describes SteamVR Home realtime lighting as “point light sources and **deferred shading** with cascaded shadow maps.” Treat that page as **internally inconsistent**; the load-bearing fact is **more than one lighting path exists**.  
     https://web.archive.org/web/20240112065133/https://developer.valvesoftware.com/wiki/SteamVR/Environments/Adding_Lighting
   - *Alex Vlachos, GDC 2015 Advanced VR Rendering:* the Source 2 **VR** Aperture/Robot Repair demo was a **forward** renderer (MSAA-friendly). Slides: http://alex.vlachos.com/graphics/Alex_Vlachos_Advanced_VR_Rendering_GDC2015.pdf (linked from contemporary coverage). That does **not** prove the non-VR renderer is forward.

5. **Tooling lineage is Source 1, rewritten:** Hammer (now mesh-centric `.vmap`), VRAD → VRAD2 (CPU) → VRAD3 (GPU RT bake). CS2 Workshop Tools (2023): Hammer “leverages GPU accelerated raytracing to both preview and bake lighting”; RT GPU required for full Hammer. That is **Valve’s own baker evolving**, not an Unreal Lightmass port.

### 2.2 What Source 2 is *not* based on

| Claim | Verdict |
|---|---|
| Source 2 renderer is a Quake / GoldSrc fork | **Not supported for Source 2.** The Quake→GoldSrc→Source 1 chain is documented for **Source 1 only** (Wikipedia *Source (game engine)*; Carmack 2004: bits of early Quake still in *Half-Life 2*). Newell called S2 an entirely new engine. Do not implement Arcane 3D as “Quake BSP renderer.” |
| Source 2 is Unreal/CryEngine under the hood | **No public evidence.** |
| One renderer for all S2 games | **False.** Dota deferred+dynamic vs VR forward vs CS2/Alyx baked lightmaps + PBR. Deadlock is its own (citadel) shader set on that family. |
| SIGGRAPH 2006 “Shading in Valve’s Source Engine” describes Source 2 | **False.** Mitchell/McTaggart/Green 2006 is **Source 1** (HL2 Radiosity Normal Mapping, Lost Coast HDR). Ancestor of *baked directional lightmaps*, not S2 code. PDF: https://cdn.akamai.steamstatic.com/apps/valve/2006/SIGGRAPH06_Course_ShadingInValvesSourceEngine.pdf |

### 2.3 Honest ancestry diagram (rendering)

```
Quake engine ──► GoldSrc ──► Source 1 (HL2 RNM, VRAD, cubemaps, Hammer brushes)
                                      │
                                      │  Valve rewrite ~2007–2015
                                      │  Newell: "entirely new engine"
                                      ▼
                                 Source 2
                    DX11-like rendersystem (Ginsburg)
                    ┌────────────┼────────────┐
                    │            │            │
              Dota deferred   VR forward   Baked PBR path
              (dynamic, no    (Vlachos     (VRAD2/VRAD3,
               light compile)  2015 demo)   CS2/Alyx/SteamVR)
                    │                         │
                    └──────── Deadlock ───────┘
                         citadel shaders (VRF)
```

**For Arcane:** copy **ideas and published math**, not a fictional “Source 2 SDK.” The implementable ancestor of the *look* is: clustered/deferred lighting (industry papers) + GGX PBR (Karis/Filament) + baked GI (VRAD3 as a *product idea*, GPULightmass/BakingLab as code) + VRF’s MIT `citadel*.slang` for NPR.

---

## 3. Deadlock / citadel — what VRF actually contains (this pass)

VRF `Renderer/` is an **OpenGL PBR viewer**, not Valve’s engine. Features listed in `Renderer/README.md`: PBR metalness/roughness, dynamic shadows, lightmaps, envmaps, light probes, fog.

**Deadlock-named shaders on `master` (fetched 2026-09-14):**

- `Renderer/Shaders/citadel_overlay.frag.slang` / `.vert.slang` / `citadel_overlay_features.slang`
- `Renderer/Shaders/environment_blend.frag.slang` (Deadlock environment; VRF issue #1092, 2026-02)
- `Renderer/Shaders/pbr.frag.slang` — **listed in the 2026-08-12 contract; confirm on disk before citing line numbers** (release notes for VRF 20.0: “dedicated shaders for Deadlock’s `pbr.vfx`, `environment_blend.vfx` and `citadel_overlay.vfx`”)
- `Renderer/Shaders/common/` — the 08-12 contract names `common/citadel.slang`, `common/pbr.slang`, `common/lighting.slang`, `common/environment.slang`, `fog.slang`. **Re-list that directory before T6 spec; do not assume filenames from memory.**
- Tiled lighting helpers present: `compute_tile_cullbits.comp.slang`, `compute_depthbin_cullbits.comp.slang`, `light_tiles_overlay.frag.slang`
- Post: `histogram.comp.slang`, `combine_luts.comp.slang`, `downsample_bloomthreshold.frag.slang`, `gaussian_bloom_blur.frag.slang`, `dof2.frag.slang`

VRF 20.0 notes also: 2.5D tiled light culling, second sun shadow cascade. That is **viewer** behavior reconstructed from assets, not a guarantee of Valve’s exact pass graph.

**Attribution:** if Arcane ports any of those Slang files, keep the MIT copyright and the Source 2 Viewer credit line.

---

## 4. Feature → legal source map (year-end 3D renderer)

Aligned with ceiling doc: **T1 → T5+T6 → T3 lite → T4 lite.** T2 baker and scene SDF **trail** (not required for a greybox Deadlock look by end of 2026).

| Arcane work | Public evidence it exists in S2/Deadlock | Implement from (legal) |
|---|---|---|
| **T1** GGX metal/rough PBR | CS2 Workshop: “CS2 uses physically based rendering… albedo… roughness.” https://www.counter-strike.net/workshop/workshopfinishes/ · VRF PBR pipeline | **Filament PBR** (Apache-2.0, the math bible) https://google.github.io/filament/Filament.md.html · Karis *Real Shading in Unreal Engine 4* (read, don’t copy UE `.ush`) · VRF `complex.frag.slang` / `pbr` (MIT, attribute) |
| **T1** clustered / tiled lights | VRF `compute_tile_cullbits.comp.slang`; Dota deferred many lights (`r_deferred_simple_light 2`) | Olsson clustered shading papers · Godot `RenderForwardClustered` (MIT) · aortiz.me primer · UE `LightGridInjection` **read-only** |
| **T1** forward vs deferred decision | Dota = deferred; VR demo = forward; SteamVR wiki = both words | Build the **grid once**; pick deferred if matching Deadlock (no MSAA, TAA). Decision owed at T1 spec, as 08-12 already said. |
| **T5** ACES already in Arcane; S2 uses Hable-class filmic | VRF `.vpost` / `combine_luts` (08-12; UNRECHECKED this pass except `combine_luts.comp.slang` exists) | Keep ACES for bring-up; Hable/Uncharted 2 curve is a published formula (Hable GDC 2010). LUT blend from VRF MIT shader. |
| **T5** histogram auto-exposure | VRF `histogram.comp.slang` exists | UE eye-adaptation blog (read-only) or implement from histogram compute |
| **T5** bloom | VRF `downsample_bloomthreshold` + `gaussian_bloom_blur` | Jimenez CoD:AW bloom (talk); do **not** need FFT |
| **T5** TAA | Deferred ⇒ no MSAA (Dota/Deadlock class) | Karis 2014 · Playdead temporal (MIT) https://github.com/playdeadgames/temporal |
| **T5** GTAO | Industry SSAO tier | **XeGTAO (MIT)** https://github.com/GameTechDev/XeGTAO |
| **T6** Deadlock NPR | VRF `citadel_overlay*.slang`; VRF 20.0 dedicated Deadlock shaders | **Port from VRF MIT Slang**, attribute. Do not wait for leaked `citadel.slang` from game VPKs compiled with Valve’s compiler. |
| **T3 lite** CSM | SteamVR/Alyx/CS2: cascaded shadows on sun; VRF “second sun shadow cascade” | **MJP Shadows sample** (permissive) · Valient ShaderX6 · “A Sampling of Shadow Techniques” |
| **T3 lite** box cubemaps | SteamVR VDC: cubemaps + box projection on materials; `env_combined_light_probe_volume` | **Lagarde** parallax-corrected cubemaps (the paper). VRF `environment` shaders if present. |
| **T4 lite** gradient / cubemap fog | 08-12 cites VRF `fog.slang` / `env_cubemap_fog` | Re-open VRF `common/` + VDC entity docs. Cheap; high Source look. |
| **T4** froxel volumetric fog | VDC “volumetric lighting” on S2; Wronski is the paper | Wronski SIGGRAPH 2014 · Hillaire Frostbite 2015 · **not** required for year-end greybox |
| **T2 baker** | CS2: GPU RT preview+bake, `vrad3.exe`; SteamVR `_vrad3` folder warning; VDC RAD table: Source 2 = VRAD2 CPU / VRAD3 GPU RT | **MJP BakingLab** (liftable) · UE GPULightmass **read-only**. Do not start for year-end picture. |
| **Scene SDF** | 08-12: Deadlock menu + `r_distancefield_enable` — **UNRECHECKED this pass** (would need a live Deadlock install / VRF cvar list, not a leak) | UE DF pipeline is read-only and huge. **Skip for 2026.** Keep Arcane JFA pick-outlines. |

---

## 5. Source 1 papers that are still useful (as *ideas*, not S2)

These are Valve-published, legal, and about **Source 1**. Use them to understand baked lighting *culture*, not as a porting source for `mesh.hlsl`.

| Paper | Year | Use |
|---|---|---|
| McTaggart, *Half-Life 2 / Source Shading* (GDC 2004) | 2004 | Radiosity + basis; start of directional lightmaps |
| Mitchell, McTaggart, Green, *Shading in Valve’s Source Engine* (SIGGRAPH 2006) | 2006 | RNM, HDR, irradiance volumes |
| Mitchell, Francke, *Illustrative Rendering in Team Fortress 2* (NPAR 2007) | 2007 | **Rim + readability** — closest published Valve NPR *before* Deadlock. Not citadel math. |

---

## 6. What this pass could not prove

- Exact Deadlock pass graph (deferred G-buffer layout, when rim runs). 08-12 cited a RenderDoc capture; **not re-run here.**
- `common/citadel.slang` filename on current VRF `master` — overlay shaders **do** exist; common/ was not fully listed in this fetch.
- “Barycentric lightmapping” as folklore — not re-searched; still do not build against that term.
- Deadlock DX11-only / “Valve redirected DX12 to `-vulkan`” — 08-12 claim; Ginsburg 2015 listed DX9/GL/DX11/Vulkan for S2. Deadlock’s *current* API default needs a live-game or patch-note cite before treating as law.
- Whether SteamVR Home is forward or deferred — VDC page contradicts itself.

If a T1 spec needs one of those, re-verify from VRF + a **first-party** Deadlock video-settings screenshot / patch notes, not from a leak.

---

## 7. Year-end rendering (this research’s implication)

Physics is out of scope. For **3D rendering by end of 2026**, the legal stack is enough:

1. **T1** — Filament/Karis GGX + a light grid (Godot/Olsson) + mesh materials stitching into `mesh.hlsl` (replace Lambert). Forward-vs-deferred: pick **deferred** if the target is Deadlock’s picture.
2. **T5+T6** — histogram + bloom from published filters; TAA from Playdead/Karis; NPR from **VRF citadel_overlay (MIT)**.
3. **T3 lite** — CSM (MJP) + one box cubemap (Lagarde).
4. Skip VRAD3-class baker and scene SDF in 2026.

That is a Deadlock-*like* untextured/PBR hero in a boxed interior, not CS2 bake quality. It does not require Source 2 source.

---

## 8. Primary URLs (keep this list in specs)

**Valve / first party**
- Newell 2012: https://www.eurogamer.net/gabe-newell-confirms-valve-working-on-source-2
- Ginsburg Vulkan: https://nextgenapis.realtimerendering.com/presentations/6_Ginsburg_Source2.pdf
- Vlachos GDC 2015 slides (VR forward): search `Alex_Vlachos_Advanced_VR_Rendering_GDC2015.pdf`
- Vlachos GDC 2016: http://alex.vlachos.com/graphics/Alex_Vlachos_Advanced_VR_Rendering_Performance_GDC2016.pdf
- SIGGRAPH 2006 S1 shading: https://cdn.akamai.steamstatic.com/apps/valve/2006/SIGGRAPH06_Course_ShadingInValvesSourceEngine.pdf
- CS2 PBR workshop: https://www.counter-strike.net/workshop/workshopfinishes/
- CS2 Workshop Tools bake (2023 notes, widely quoted): Hammer GPU RT preview+bake, RTX 2060 Ti / 6600 XT class

**VDC (Wayback if Anubis)**
- Dota lighting (deferred, no compile): https://developer.valvesoftware.com/wiki/Dota_2_Workshop_Tools/Level_Design/Dota/Lighting
- SteamVR lighting (multiple renderers / bake): https://developer.valvesoftware.com/wiki/SteamVR/Environments/Adding_Lighting
- Source 2 overview: https://developer.valvesoftware.com/wiki/Source_2

**VRF**
- https://github.com/ValveResourceFormat/ValveResourceFormat
- Viewer: https://s2v.app
- Shaders: `Renderer/Shaders/` especially `citadel_overlay*`, `environment_blend*`, `complex.frag.slang`, `compute_tile_cullbits.comp.slang`

**Liftable / papers**
- Filament PBR
- Karis UE4 shading (read)
- Lagarde cubemaps
- Wronski 2014 volumetrics
- XeGTAO, Playdead TAA, MJP Shadows + BakingLab
- Godot clustered forward (MIT)

Wikipedia *Source 2* / *Source (game engine)* are useful for **dates and citations**, not for shader math.

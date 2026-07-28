# Sprite Asset (.arcsprite) + SpriteRenderer Rework — Design

**Date:** 2026-07-28
**Branch:** continues the current stack (`arcane-entity-rename` tip); implementation
branch cut is a plan-time call.
**Reference:** vendored UE Paper2D source (authoritative, cited by line) + Unity
docs (linked). All engine-convention claims below were verified this session, not
recalled.

## Goal

Replace the SpriteRenderer's raw `textureId` + hand-authored `size` with a
Guid-referenced sprite asset, matching the convention both reference engines
share: **the renderer names a sprite object; the sprite object carries the
pixel-to-world metadata; Transform scale is the sizing mechanism.** The
"w/h in meters" that prompted this arc was never the anomaly — meters are our
MKS directive working — the anomaly was the renderer doing the sprite asset's
job, with a 32-meter default as the symptom.

## Verified convention (the why)

- **UE (`PaperSpriteComponent.h:35-44`):** the render component's content
  fields are `SourceSprite` + `SpriteColor`. No size field. Rendered size =
  sprite `SourceDimension` / per-sprite `PixelsPerUnrealUnit`
  (`PaperSprite.h:123-124`) x component scale. The asset also carries the
  sub-rect (`SourceUV`/`SourceDimension`, `PaperSprite.h:76-81`) and pivot.
- **UE creation flow:** import texture -> explicit "Create Sprite" action;
  `UPaperImporterSettings` (`PaperImporterSettings.h:68-70`) auto-fills
  defaults (pixels-per-unit, material analysis) so it is one gesture.
- **Unity:** no size on the renderer in Simple draw mode (Size exists only for
  Sliced/Tiled). PPU/pivot/mode live in the texture's import settings; the
  Sprite object is a sub-asset auto-generated at import. In a 2D project the
  importer defaults to Sprite type, so the UX is drop-PNG-and-go. Unity's
  "primitive shapes" are built-in sprite assets sized 1x1 unit, scaled via
  Transform. Docs: texture-type-sprite.html, import-images-sprites-landing.html.
- **Conclusion adopted (user-approved):** `.arcsprite` standalone asset (UE's
  storage) + auto-mint on drop (Unity's UX); `size` removed entirely; scale
  sizes everything, including our SDF primitives.

## User-locked decisions

1. Standalone `.arcsprite` asset, Guid-referenced — NOT a texture sidecar, NOT
   PPU-on-the-renderer.
2. Auto-mint on drop + explicit Create Sprite action.
3. Convention-pure: `size` deleted; primitives/untextured sized by Transform
   scale on a 1x1 m base.
4. UE-complete asset in v1: sub-rect AND pivot ship now (Approach B).
5. **Hard break: no migration, no legacy support.** Test projects (Aphelyon
   sandbox scenes, Sandbox/PlaygroundGame code) get re-authored in-arc.
   (Corrected at plan time: the loader's warning is COMPONENT-level only,
   `SceneSerializer.hpp:275-289`; stale `size`/`textureId` keys inside a
   still-known component are dropped SILENTLY by the generic reader,
   `ReflectionJson.hpp:379-394`. Accepted under this decision.)

## Section 1 — The .arcsprite asset

JSON asset following `.arcmat` conventions: Guid identity, mount-path
registered, saved beside content. Fields:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `texture` | Guid | nil | Source texture asset. Nil renders as the untextured tint quad. |
| `ppu` | float | 100 | Pixels per meter (matches `pixelsPerMeter` and Unity's default). |
| `sourcePos` | vec2 (pixels) | (0,0) | Sub-rect origin in the texture (UE `SourceUV` shape). |
| `sourceSize` | vec2 (pixels) | (0,0) | Sub-rect dimensions; **(0,0) = whole texture**. |
| `pivot` | vec2 normalized | (0.5,0.5) | Unity-style normalized pivot; presets are future editor sugar. |

Absent fields take defaults — every future field is additive. Pixel-space rect
(not UV) is the authoring unit, converted at resolve time.

## Section 2 — Component + runtime

`SpriteRenderer` becomes:
`{ Guid sprite; vec4 tint; int32 sortingLayer; int32 orderInLayer; SpriteShape shape; Guid material; }`

**Deleted: `textureId`, `size`, and `TextureTable`** (its only consumer was
this path; deleting it discharges its own "Full Assets integration deferred"
note, `SceneResources.hpp:82`, and the Batcher2D comment hook at
`Batcher2D.hpp:191` gets revisited at plan time).

New `SpriteTable` resource on the `SpriteMaterialTable` pattern
(`SceneResources.hpp:99-109`): host-populated, `Resolve(Guid)` returns a
precomputed record `{ ITexture*, uvMin, uvMax, sizeMeters, pivot }` where
`sizeMeters = sourceSize / ppu` (texture dims when sourceSize is (0,0)) and
UVs come from the pixel rect over texture dims. How the host loads texture
Guids to GPU textures follows the existing material-texture path (plan-time
verification item).

Submission (`RenderSystems.hpp`):
- `dstSize = resolved.sizeMeters * worldScale * ctx->zoom`
- `dstPos = screenPos - dstSize * resolved.pivot` — note today's code is
  exactly this with pivot hardcoded (0.5,0.5) (`RenderSystems.hpp:75`), so
  pivot generalizes the existing formula.
- Nil sprite Guid, unresolved Guid, or nil texture in the asset: 1x1 m tint
  quad x scale — the same draw-plain-until-ready philosophy as unresolved
  materials (`RenderSystems.hpp:113-119`).
- `Circle`/`Capsule`: 1x1 m base x scale. Disc diameter = scale.x
  (rotation-invariant, as today); capsule length = scale.x, height = scale.y,
  keeping the x >= y convention.

**Invariant consequences (review gates):**
- Transform position semantics change from "sprite center" to "pivot point,
  default center". At default pivot, byte-identical placement to today.
- Physics-matched sprites keep pivot (0.5,0.5); the collider-matching
  primitives never consume the sprite asset, so physics alignment is
  untouched.
- **Implementer must verify batcher rotation semantics:** if `Quad`/`Rect`
  rotate about the quad center, non-center pivots need rotate-about-pivot
  math in submission (they coincide at center pivot, which is why today's
  code never had to care).
- Render interpolation (`PreviousTransform`) lerps position before the pivot
  offset is applied — order unchanged, no new interaction.

## Section 3 — Editor

- `AssetKind::Sprite` (`.arcsprite`), `kAssetKindCount` 7 -> 8, icon, and the
  field-name heuristic (`AssetBrowser.hpp:111-121`) learns `"sprite"`. The
  Inspector's existing AssetRef arm (drag-drop, kind filter, C1 read-only
  gate) serves the component field with no new machinery.
- **Auto-mint on drop:** dropping a *texture* payload onto the component's
  sprite field: if exactly ONE registered `.arcsprite` already references
  that texture, assign it; zero OR multiple matches mint a fresh
  `<texture-name>.arcsprite` beside the texture with all defaults and assign
  it (never guess among duplicates). Deterministic, no dialog.
- **Explicit path:** texture row context menu -> "Create Sprite" (precedent:
  Material "New Instance...", Scene "Set as Boot Scene",
  `AssetBrowser.cpp:112-124`).
- **Editing:** double-click `.arcsprite` -> compact **SpriteDocument** in the
  DocumentHost (the `docs.OpenPath` route, `AssetBrowser.cpp:126-140`):
  texture preview with the sub-rect outlined, numeric fields (ppu, rect,
  pivot), save. Visual rect-dragging on the preview is future polish, not v1.

## Section 4 — Break policy + testing honesty

No migration code of any kind (user directive this session). Content re-keyed
by hand in-arc: sandbox/PlaygroundGame spawn code updated; Aphelyon scenes
re-authored when next opened — the loader already warns on unknown fields.

Gate-coverable (real tests): `.arcsprite` save/load round-trip + absent-field
defaults, `SpriteTable::Resolve` (nil/unresolved/full-rect/sub-rect/derived
size), UV + sizeMeters math, `AssetKindOf(".arcsprite")`, heuristic mapping.
Desk-only (screenshot/interaction): submission rendering, pivot visual, auto-
mint UX, SpriteDocument, drag-drop. Same honesty split as the Inspector
polish arc.

## Non-goals

- Flip X/Y on the renderer (UE doesn't have it; UV swap is cheap later).
- Atlas auto-slicing / multi-sprite-per-texture tooling (rect field is the
  seam; tooling later).
- Drag-texture-into-viewport entity creation (Unity nicety; separate arc).
- Sliced/Tiled draw modes (the only case either engine puts Size on a
  renderer; not needed yet).
- Any migration/compat path (decision #5).

## Plan-time verification items

1. How material texture bindings load texture Guids to `nvrhi::ITexture*`
   host-side — the SpriteTable population rides that path.
2. Batcher `Quad`/`Rect` rotation origin (center vs pos) for the pivot math.
3. `.arcmat` JSON envelope/save conventions, to mirror exactly.
4. `Batcher2D.hpp:191`'s comment hook — what it expects when TextureTable dies.
5. DocumentHost document contract (what a minimal document implements).

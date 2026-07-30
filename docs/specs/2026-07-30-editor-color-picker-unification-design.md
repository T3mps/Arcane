# Editor colour editing: one picker, sRGB authoring — design

Status: APPROVED (user, 2026-07-30). Successor doc: an implementation plan.

## Why

Two requests, one arc:

1. **A picker.** Clicking a colour field's swatch in the Inspector should open a
   graphical picker with a radial hue wheel and a hex box you can paste into. The
   existing four 0-255 RGBA fields stay exactly as they are.
2. **Unify it.** Colour currently edits three different ways in this editor.

And one defect found while scoping, which the user chose to fix in the same arc:
the tint is authored in a different colour space than every texture it multiplies.

## What is true today (verified, with citations)

| site | display | picker |
|---|---|---|
| `InspectorView.cpp:669` — component fields (`SpriteRenderer::tint`) | 0-255 (ImGui default `Uint8`) | **off** — `ImGuiColorEditFlags_NoPicker` |
| `InspectorView.cpp:643` — the same fields, MULTI-select | raw linear floats (`integral == false`) | n/a |
| `ShaderEditorDocument.cpp:5826` — material params | 0..1 floats (no flags passed) | on, square + hue bar |
| `ShaderEditorDocument.cpp:4873` — `ConstColor` graph node | `DragFloat4` (linear) + a swatch | **none** |

So single-select and multi-select disagree *inside one field*, and the shader
editor disagrees with both.

Colour space, the substantive defect:

- `Assets.cpp:176` creates textures as `nvrhi::Format::SRGBA8_UNORM`, so the
  sampler decodes sRGB → linear in hardware.
- `tonemap.hlsl:33` is `pow(ACESFilmic(linearColor), 1.0 / 2.2)` — the canvas is
  linear and gets encoded once, at the end.
- The tint is **not** decoded. Authored `#808080` therefore means linear `0.216`
  as a texture pixel and linear `0.502` as a tint, in the same multiply.

UE agrees with the direction we chose: `SColorPicker.h:338` documents its hex box
as sRGB gamma, and it carries an explicit sRGB display toggle.

## Decisions

1. **sRGB in, linear stored.** The widgets display and accept sRGB-encoded
   values; storage stays the linear float it is today. **Not a data migration** —
   no stored value changes, so nothing changes on screen. The visible consequence
   is *re-labelling*: a tint authored as `128` reads `186` afterwards. 0, 255 and
   alpha are unaffected.
2. **The true sRGB piecewise curve, not `pow(2.2)`.** The point is agreeing with
   the texture sampler, which uses the real curve, and with what a hex from a
   design tool means. The tonemap's `pow(1/2.2)` is a *display* transfer, was
   deliberately byte-matched to the retired client's `post_process.glsl`, and is
   explicitly out of scope.
3. **Alpha is never encoded.** It is coverage, not colour.
4. **An `hdr` mode.** A graph `ConstColor` or an emissive material param may
   exceed 1, where sRGB encoding is meaningless. Those keep raw linear floats.
   One helper, one policy, two documented modes.
5. **Radial wheel**, per the request: `ImGuiColorEditFlags_PickerHueWheel`.
6. **The helper does not own undo.** It returns `changed`; each call site keeps
   its own bracketing.

## Design

### The helper — `ArcaneEditor/src/EditorWidgets.{hpp,cpp}`

Existing unit, and already in `ArcaneTests`' file list (`premake5.lua:611`), so
the conversions are unit-testable with no GPU and no ImGui frame.

```cpp
// sRGB <-> linear, the IEC 61966-2-1 piecewise curve -- the same transfer
// SRGBA8_UNORM applies in hardware, so a tint and a texture pixel authored as
// the same number finally mean the same colour. NOT pow(2.2): that is the
// tonemap's display encode and stays where it is.
[[nodiscard]] float SrgbToLinear(float srgb) noexcept;   // display -> storage
[[nodiscard]] float LinearToSrgb(float linear) noexcept; // storage -> display

// THE colour widget for the editor. Reads and writes LINEAR; presents sRGB.
// Four 0-255 channel boxes plus a swatch that opens ImGui's picker: radial hue
// wheel, alpha bar, and RGB/HSV/Hex rows (the hex box accepts paste).
// `linear` is float[4] rather than glm::vec4& so all three call sites bind
// unchanged. hdr = true presents raw linear floats instead (no encode) and adds
// ImGuiColorEditFlags_HDR, for values that may exceed 1.
// Returns true on the frames the value changed.
bool ColorField4(const char* id, float linear[4], bool hdr = false);
```

`id` is passed through to `ColorEdit4` verbatim and each call site keeps the
string it uses today — do NOT "unify" that too. The Inspector passes a hidden
`##id` because it draws its own label column, while the material params panel
passes a visible name; a visible label also becomes the picker popup's title
(`imgui_widgets.cpp:5970-5973`). Changing either would break that site's row
layout.

Flags, set explicitly rather than inherited from ImGui's defaults:

- normal: `Uint8 | AlphaBar | PickerHueWheel`
- hdr: `Float | HDR | AlphaBar | PickerHueWheel`

Nothing sets `NoPicker`. The hex box needs no flag of ours: `ColorEdit4` forwards
`PickerMask_` (so the wheel carries into the popup) and force-sets
`DisplayMask_` on the picker (`imgui_widgets.cpp:5975-5976`), and `ColorPicker4`
draws a hex `ColorEdit4` whenever `DisplayHex` is in the mask
(`imgui_widgets.cpp:6304-6305`).

Data flow per frame, non-hdr:

```
stored linear[4] --LinearToSrgb x3, alpha passthrough--> display[4]
   ImGui::ColorEdit4(display)  (channel boxes + swatch -> picker popup)
display[4] --SrgbToLinear x3, alpha passthrough--> stored linear[4]   (only if changed)
```

Writing back only when ImGui reports a change matters: a blind round-trip every
frame would quantise the stored float to 255ths continuously, so a value set by
script or animation would decay just by being looked at.

### Call sites

1. **`InspectorView.cpp:669`** (single-select) → `ColorField4`, `hdr = false`.
   Existing `BeginGestureIfActivated` + `ForEachTarget` write path unchanged.
2. **`InspectorView.cpp:643`** (multi-select) → pass **encoded** values in `cur[]`
   and `integral = true`, and decode the committed channel before `ApplyImmediate`
   writes it. Without this, multi-select would keep showing raw linear floats —
   the inconsistency inside one field that exists today.
3. **`ShaderEditorDocument.cpp:5826`** (material params) → `ColorField4`,
   **`hdr = false`**. Display changes float → 0-255; this is the unification the
   user asked for.

   Why `false` unconditionally, rather than deriving it: nothing declares a
   colour param as HDR. `ParamMeta` carries only `sliderMin`/`sliderMax`
   (`MaterialTypes.hpp:133-139`), documented as the **Float** slider's range and
   ignored by the colour widget, and every existing template leaves them 0..1 —
   so deriving `hdr` from them would invent a signal and mean nothing for
   existing content. The type already answers it: `MatParamType::Color` exists
   because "the editor shows a color picker" (`MaterialTypes.hpp:26`) — it is a
   *colour*, so it authors like every other colour. A parameter that wants an
   unclamped multiplier is a `Float4`, which keeps its `DragFloat4` and is not a
   colour widget at all.
4. **`ShaderEditorDocument.cpp:4873`** (`ConstColor` node) → `ColorField4`,
   `hdr = true`, wrapped in `ed::Suspend()` / `ed::Resume()`. The comment there
   claiming "popups cannot open inside the canvas" is **wrong**: the same file
   already opens canvas popups that way at `:2848`, `:3916` and `:3987`.

### Undo

No popup-aware gesture is needed, contrary to the `NoPicker` comment
(`InspectorView.cpp:656-661`). It reasoned correctly that popup widgets are not
the ColorEdit item, but landed on the wrong end of the bracket:

- **Open:** ImGui rewrites `g.LastItemData.ID` to the picker's `ActiveId` while
  the popup is live, expressly so `IsItemActive()` keeps working on `ColorEdit4`
  (`imgui_widgets.cpp:6044-6046`), and `MarkItemEdited` runs on that id
  (`:6048`). So `IsItemActivated()` fires on the frame a picker widget is
  pressed, and `EditGesture::BeginOnActivate` parks that id.
- **Close:** on release, `g.ActiveId` becomes 0, the rewrite stops, and the
  ownership guard in `EvaluateEnd` correctly refuses to close on a foreign id —
  so the release frame does **not** commit. `EditGesture::ScopeGuard`'s
  abandonment path (`ShouldCloseAbandoned`: parked gesture whose owner no longer
  holds `ActiveId`) closes it that same frame, with Commit semantics, which is
  precisely what this case needs.

Net semantics: **one drag inside the picker = one undo step**, the same rule the
inline channel drags already follow. A second drag in the same popup session is a
second step (`ShouldCloseStaleOnActivate` commits the first). This is asserted by
test, not assumed — see below.

## Verification

**USER'S CALL (2026-07-30): no in-depth tests for this arc — "just my visual
gate, as this is a very visual thing."** This section is written to that decision;
an earlier draft specified three headless test groups and is deliberately
superseded. The per-task desk-check lists in the plan are the verification of
record.

What that means concretely:

- Each task ends with a clean build, plus an explicit check that
  `ArcaneEditor.exe` actually relinked — a green `Arcane`/`ArcaneTests` build does
  NOT prove it, since neither compiles `InspectorView.cpp` or
  `ShaderEditorDocument.cpp`.
- The user's visual gate covers: the wheel appears on swatch click at every site;
  a pasted hex reproduces its source on screen; one Ctrl+Z per drag and none for
  a click that dragged nothing; multi-select blanks and writes correctly; the
  graph-node picker opens above the canvas; and the expected re-label (an
  existing `128` reading `186`) with **no change to the rendered colour**.
- The undo claim in the section below is therefore verified by eye (one Ctrl+Z
  per drag) rather than by a headless frame-sequence test.

One residual risk is accepted knowingly: a wrong transfer curve looks *plausible*
on screen, so the visual gate cannot catch it. The plan carries an optional
throwaway console print asserting `SrgbToLinear(128/255) == 0.2158` (the value
that proves we match the texture hardware rather than merely being
self-consistent) to be deleted before commit. It is a sanity check, not a test.

## Non-goals

- A dedicated `Arcane::Color` field type. It is the right destination — it would
  retire the field-NAME heuristic that `InspectorFields.hpp:47-51` itself calls a
  placeholder — but it is a new reflected type plus a migration of every existing
  colour field. Separate arc; this helper is what it would plug into.
- Changing the tonemap's `pow(1/2.2)`.
- Migrating stored colour data (explicitly unnecessary — see Decision 1).
- Eyedropper, palettes, recent-colour swatches.

## Known upstream wart

`imgui_widgets.cpp:5903` — with `ImGuiColorEditFlags_HDR`, hue/saturation "snap
in weird ways when SV values go below 0". Affects the `hdr` mode only, is
upstream's FIXME, and is not worked around here.

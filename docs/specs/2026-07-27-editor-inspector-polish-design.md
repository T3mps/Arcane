# Editor Inspector: readable names, search, categories

Date: 2026-07-27
Status: design approved
Scope: presentation + findability for the Inspector panel. The reflection
machinery underneath (classification, mixed-value masks, gesture bracketing)
is already sound and is NOT being rebuilt.

---

## 1. Problem

The Inspector works but reads like a debugger, not an authoring tool.

It renders raw C++ identifiers -- `Arcane::SpriteRenderer` as a header,
`sortingLayer` and `orderInLayer` as field labels -- has no search of any kind,
shows every field of a component in one flat list, and offers no way to revert
a field you changed.

The underlying panel is not the problem. `InspectorFields.hpp` already does
reflection-driven classification, per-scalar mixed-value masks for multi-select
(cited to UE's `ComponentTransformDetails.cpp`), and `EditorPanels.cpp` handles
per-component ID scoping, deferred structural removal, tag components and
asset-reference widgets. What is missing is the surface.

**The decisive find: Astra already ships the metadata this needs.**
`ThirdParty/Astra/include/Astra/Reflection/Attribute.hpp` defines `Category`,
`DisplayName`, `Tooltip`, `Range`, `Hidden`, `ReadOnly` and `ColorFormat`, all
attachable with the existing `ASTRA_REFLECT_ATTR` macro and queryable through
`FieldInfo::GetAttribute<A>()`. `FieldInfo::attributes` is populated and, apart
from `Serializable`, entirely unread. This arc is mostly about consuming
metadata that already exists -- no new reflection machinery, and nothing
modified in vendored Astra.

## 2. Unreal references

Verified by reading the vendored source at
`Arcane/.example/UnrealEngine-release/`, not from memory:

| Behaviour | Source |
|---|---|
| Details panel search filtering properties | `Engine/Source/Editor/PropertyEditor/Private/SDetailsViewBase.cpp:1016` -- `OnFilterTextChanged` -> `FilterView` |
| Identifier -> display string | `Engine/Source/Runtime/Core/Private/UObject/UnrealNames.cpp:2693` -- `FName::NameToDisplayString`, which takes a `bIsBool` flag |
| Per-property reset-to-default | `Engine/Source/Editor/PropertyEditor/Private/UserInterface/PropertyEditor/SResetToDefaultPropertyEditor.cpp` |
| Category grouping with a no-category fallback | `Engine/Source/Editor/PropertyEditor/Private/DetailCategoryBuilderImpl.cpp:208-231` -- `FDetailCategoryImpl`, `NoCategoryName` |

## 3. A new pure module

`Arcane/ArcaneEditor/src/InspectorMeta.hpp` / `.cpp` -- sibling to
`InspectorFields.hpp` and bound by the same rule: **no ImGui**, so the
`[editor]` units drive all of it headlessly. It goes in its own file rather than
growing `InspectorFields.hpp`, which is already carrying classification,
write-backs and the mixed-value mask.

- `std::string DisplayNameForField(const Astra::FieldInfo&)` -- the
  `Astra::DisplayName` attribute when present, else derived from the
  identifier: split camelCase and snake_case, capitalise each word.
  `sortingLayer` -> "Sorting Layer", `order_in_layer` -> "Order In Layer".
- `std::string DisplayNameForComponent(std::string_view typeName)` -- strip the
  namespace, then apply the same derivation: `Arcane::SpriteRenderer` ->
  "Sprite Renderer".
- `std::string_view CategoryOfField(const Astra::FieldInfo&)` -- the
  `Astra::Category` attribute's value, or empty for "no category". Empty
  renders ungrouped ABOVE any named category, matching UE's `NoCategory`
  fallback.
- `bool MatchesInspectorFilter(std::string_view componentDisplayName, std::string_view fieldDisplayName, std::string_view rawFieldName, std::string_view query)`
  -- case-insensitive substring. Matches against BOTH the display name and the
  raw identifier, so a user who knows the code can search `sortingLayer` and a
  user who does not can search `sorting`. An empty query matches everything.
  **A query matching the COMPONENT's display name matches every field in it**,
  so searching "sprite" shows the whole Sprite Renderer rather than none of it.
  That rule lives HERE, in the pure function, precisely so it is pinned by a
  test rather than buried as an `if` in the draw loop.
- `std::optional<std::pair<float,float>> RangeOfField(const Astra::FieldInfo&)`
  -- the `Astra::Range` attribute, feeding drag/slider bounds.
- `bool FieldIsReadOnly(const Astra::FieldInfo&)` and
  `bool FieldIsAttributeHidden(const Astra::FieldInfo&)`.

**`InspectorMeta.cpp` needs an explicit entry** in `Arcane/premake5.lua`'s
`project "ArcaneTests"` files block followed by `GenerateProjects.bat`.
`ArcaneTests` does not glob editor sources -- it lists them one by one -- and
the `ArcaneEditor` project's own `src/**.cpp` glob is evaluated at GENERATE
time, so a new TU without regeneration produces LNK2019.

### Naming hazard, called out because it will bite

`Astra::Hidden` is a FIELD ATTRIBUTE meaning "do not show this property".
`Arcane::Hidden` is a MARKER COMPONENT meaning "render submission skips this
entity". They are unrelated and share a name. Every mention in code and comment
must be namespace-qualified, and the predicate above is deliberately called
`FieldIsAttributeHidden` rather than `FieldIsHidden`.

## 4. What changes in the panel

`DrawInspectorPanel` in `EditorPanels.cpp`:

- A search box at the top, above the component list, filtering both components
  and fields live. A component whose header matches keeps all its fields; a
  component that does not match shows only its matching fields, and disappears
  entirely when none match. State lives in `InspectorState`.
- Fields grouped by category within each component: uncategorised first, then
  each named category under its own collapsible sub-header.
- Component headers and field labels use the display names from `InspectorMeta`.
- Hovering a label shows the `Astra::Tooltip` attribute when present, and the
  raw C++ identifier in every case -- so the friendly name never costs you the
  ability to find the field in source.
- `Astra::ReadOnly` fields draw disabled; `Astra::Hidden` fields are skipped.
- Numeric drags honour `Astra::Range` when present.

Existing behaviour that must not regress: per-component `PushID` scoping, the
deferred `pendingRemove`, the component-type intersection for multi-select, the
mixed-value blanking, and gesture bracketing through the `CommandStack`.
Grouping and filtering change WHICH fields are drawn and in what order, never
how an edit is applied.

## 5. Reset-to-default

Per-field revert, in the shape of UE's `SResetToDefaultPropertyEditor`: an
affordance appears beside a field whose value differs from a default-constructed
instance of its component, and clicking it writes the default back through the
same `Apply*Edit` path a manual edit uses, so it is one undo entry like any
other.

**This item is conditional and planning must resolve it first.** It needs a
default-constructed instance of the owning component to compare against, and
whether `Astra::ComponentDescriptor` exposes construction cheaply is unverified.
If it does not, **reset-to-default drops out of scope** rather than growing new
engine API for a presentation feature. Planning decides this before Task 1 and
records the answer; it does not get discovered mid-implementation.

Multi-select: the affordance shows when ANY selected entity differs from the
default, and clicking resets all of them -- consistent with how every other
field edit in this panel already fans out.

## 6. Annotating the engine's components

`Transform`, `SpriteRenderer`, `PostProcess` and `EntityInfo` in
`Arcane/Scene/Components.hpp` gain `Category` and `Tooltip` attributes, and
`Range` where a bound is genuinely real rather than invented.

This is purely additive. A component with no attributes renders exactly as it
does after section 3 -- derived display names, no categories -- so Sandbox,
PlaygroundGame and the external Aphelyon module need no changes and cannot
break. Nothing outside `Arcane/Scene/Components.hpp` is annotated in this arc.

One concrete opportunity while annotating: `SpriteRenderer::size` still defaults
to `{32, 32}`, which under MKS is 32 METRES, and its comment already says so.
This arc does NOT change that default -- it is a behaviour change affecting
every existing scene and deserves its own decision -- but a `Tooltip` saying the
units are metres is exactly the kind of thing this metadata exists for.

## 7. Testing

All headless, no `[gpu]`, in a new `Arcane/Tests/src/EditorInspectorMetaTest.cpp`
tagged `[editor]`:

- Display-name derivation: camelCase, snake_case, an already-spaced name, a
  single word, an all-caps acronym, a leading-capital name, and the empty
  string. Each must be pinned, because this is the function whose output the
  user reads on every field.
- `DisplayName` attribute overrides derivation.
- Component names strip the namespace before deriving.
- Category extraction, including the absent case falling into the ungrouped
  bucket.
- Filter matching: display-name hit, raw-identifier hit, case-insensitivity,
  empty query matches all, no-match.
- `Range`, `ReadOnly` and `Astra::Hidden` extraction, each with its
  attribute-absent fallback.

Note `ArcaneTests` runs in random order under a time-based seed, so capture the
"Randomness seeded to" banner and re-run under `--rng-seed 6` and
`--rng-seed 17` before calling the gate green. Baseline is 30105 assertions /
571 cases.

## 8. Verification beyond the gate

**The test gate does not compile `EditorPanels.cpp`.** `ArcaneTests` lists
editor sources individually and that file is not among them, so a green suite
proves the pure module works and says nothing about the panel. After every
change, confirm `ArcaneEditor.exe`'s timestamp is newer than every file under
`Arcane/ArcaneEditor/src`, and hand the ImGui surface to a human.

Desk-verify list: search narrows as you type and clears correctly; a component
with no matching fields disappears; categories group and collapse; labels read
as words with the raw identifier in the tooltip; multi-select still blanks mixed
values; Add/Remove Component still work through the header menu; and an edit is
still one undo entry.

## 9. Out of scope

- Copy/paste of component or field values.
- Keyboard traversal between fields.
- Reordering components in the panel.
- Annotating anything outside `Arcane/Scene/Components.hpp`.
- Changing `SpriteRenderer::size`'s default (see section 6).
- Any change to vendored Astra.

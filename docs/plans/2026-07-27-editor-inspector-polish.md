# Editor Inspector Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Arcane Editor's Inspector readable and searchable — human names instead of C++ identifiers, a live filter, category grouping, tooltips, ranges, and per-field reset-to-default.

**Architecture:** A new pure `InspectorMeta` module owns every decision (name derivation, attribute extraction, filter matching, default-value comparison) with no ImGui, so it is unit-tested headlessly; `DrawInspectorPanel` becomes a consumer of it. All the metadata already exists in Astra's reflection attributes and is currently unread.

**Tech Stack:** C++23, Astra reflection (`FieldInfo::GetAttribute<A>()`), Dear ImGui (docking), Catch2, premake5.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-27-editor-inspector-polish-design.md`. Decisions there are locked.
- **Nothing in vendored Astra may be modified.** `ThirdParty/Astra/**` is read-only for this arc. Every attribute needed already exists.
- **`Astra::Hidden` is a FIELD ATTRIBUTE** ("do not show this property"). **`Arcane::Hidden` is a MARKER COMPONENT** (render submission skips the entity). Unrelated, same name. Namespace-qualify both at every mention; the predicate is named `FieldIsAttributeHidden`, never `FieldIsHidden`.
- Exception-free. UTF-8 without BOM, ASCII-only comments. Comments explain WHY, not what, and must never claim more than the code delivers.
- **`ArcaneTests` does NOT glob editor sources.** `Arcane/premake5.lua` lists each `ArcaneEditor/src/*.cpp` explicitly. A new editor `.cpp` a test drives MUST be added there, then `GenerateProjects.bat` re-run. The `ArcaneEditor` project's own `src/**.cpp` glob is evaluated at GENERATE time, so a new TU without regeneration produces LNK2019.
- **`EditorPanels.cpp` is NOT compiled by the test gate.** A green suite proves the pure module and says nothing about the panel. After every panel change, confirm `ArcaneEditor.exe` is newer than every file under `Arcane/ArcaneEditor/src`.
- **Build with the VS 18 toolchain explicitly.** Plain `msbuild` on PATH is VS 2022 and fails with MSB8020:
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal`
- **Gate runs from the exe directory:** `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "~[gpu]"`. Random order under a time-based seed, so re-run under `--rng-seed 6` and `--rng-seed 17`. **Baseline: 30105 assertions / 571 cases.**
- Existing Inspector behaviour must not regress: per-component `PushID` scoping, deferred `pendingRemove`, component-type intersection for multi-select, mixed-value blanking, and `CommandStack` gesture bracketing. This arc changes WHICH fields draw and in what order — never how an edit is applied.

## Resolved before Task 1 (the spec required this)

**Reset-to-default is IN SCOPE.** `Astra::ComponentDescriptor` (`ThirdParty/Astra/include/Astra/Component/Component.hpp`) exposes everything needed: `size`, `alignment`, `hash`, `defaultConstruct` (`ConstructFn* = void(void*)`), `destruct` (`DestructFn* = void(void*)`), a `DefaultConstruct(void*)` helper that already handles the tag/empty and trivially-default-constructible cases, and `Destruct(void*)`. A scratch instance can be built, read, and destroyed safely.

## Attribute shapes (verified, use verbatim)

From `ThirdParty/Astra/include/Astra/Reflection/Attribute.hpp`:

```cpp
struct Range       : AttributeBase<Range>       { double min, max, step; };          // step defaults 0.0
struct Hidden      : AttributeBase<Hidden>      { };
struct ReadOnly    : AttributeBase<ReadOnly>    { };
struct DisplayName : AttributeBase<DisplayName> { std::string_view name; };
struct Tooltip     : AttributeBase<Tooltip>     { std::string_view text; };
struct Category    : AttributeBase<Category>    { std::string_view category; };
```

Query with `field.GetAttribute<Astra::Category>()`, returning `const A*` (null when absent). Attach with `ASTRA_REFLECT_ATTR(Category, "Rendering")` immediately after an `ASTRA_REFLECT_FIELD`.

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/ArcaneEditor/src/InspectorMeta.hpp` / `.cpp` (create) | All pure decisions: name derivation, attribute extraction, filter matching, default-value comparison. No ImGui. |
| `Arcane/premake5.lua` (modify) | `InspectorMeta.cpp` into the `ArcaneTests` file list. |
| `Arcane/ArcaneEditor/src/EditorPanels.hpp` (modify) | `InspectorState` gains the search buffer. |
| `Arcane/ArcaneEditor/src/EditorPanels.cpp` (modify, `DrawInspectorPanel` from line ~1310) | Consumes `InspectorMeta`. |
| `Arcane/Arcane/src/Arcane/Scene/Components.hpp` (modify) | Category/Tooltip/Range annotations. |
| `Arcane/Tests/src/EditorInspectorMetaTest.cpp` (create) | `[editor]` units for the whole pure module. |

---

### Task 1: Display-name derivation

**Files:**
- Create: `Arcane/ArcaneEditor/src/InspectorMeta.hpp`, `Arcane/ArcaneEditor/src/InspectorMeta.cpp`
- Modify: `Arcane/premake5.lua`
- Test: `Arcane/Tests/src/EditorInspectorMetaTest.cpp`

**Interfaces:**
- Produces:
  - `std::string Arcane::Editor::DeriveDisplayName(std::string_view identifier)`
  - `std::string Arcane::Editor::DisplayNameForField(const Astra::FieldInfo&)`
  - `std::string Arcane::Editor::DisplayNameForComponent(std::string_view typeName)`

This is the function whose output the user reads on every field, so every case is pinned by a test.

**Derivation rules** (UE's `FName::NameToDisplayString`, `UnrealNames.cpp:2693`, minus its `bIsBool` `b`-prefix convention, which this codebase does not use):
1. Underscores become spaces.
2. A space is inserted before an uppercase letter that follows a lowercase letter or a digit.
3. A space is inserted before an uppercase letter that is followed by a lowercase letter and preceded by an uppercase letter — the acronym boundary, so `HTTPServer` becomes `HTTP Server`.
4. The first letter of each word is uppercased.
5. Runs of spaces collapse; leading and trailing spaces are trimmed.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/EditorInspectorMetaTest.cpp`:

```cpp
// InspectorMeta: the Inspector's PURE decisions -- what a field is called, what
// category it belongs to, whether a filter matches it. No ImGui, so the whole
// surface the user reads is pinned here rather than desk-verified.

#include <catch2/catch_test_macros.hpp>

#include "InspectorMeta.hpp"

using namespace Arcane::Editor;

TEST_CASE("DeriveDisplayName turns identifiers into words", "[editor]")
{
    CHECK(DeriveDisplayName("sortingLayer") == "Sorting Layer");
    CHECK(DeriveDisplayName("orderInLayer") == "Order In Layer");
    CHECK(DeriveDisplayName("order_in_layer") == "Order In Layer");
    CHECK(DeriveDisplayName("size") == "Size");
    CHECK(DeriveDisplayName("Position") == "Position");
    CHECK(DeriveDisplayName("textureId") == "Texture Id");

    // Acronym boundary: the run stays together and the next word splits off.
    CHECK(DeriveDisplayName("HTTPServer") == "HTTP Server");
    CHECK(DeriveDisplayName("useHDR") == "Use HDR");

    // A digit ends a word, so a trailing number reads as its own token.
    CHECK(DeriveDisplayName("vec2Field") == "Vec2 Field");

    // Already-readable input must survive untouched rather than gain spaces.
    CHECK(DeriveDisplayName("Already Spaced") == "Already Spaced");

    // Degenerate input must not crash or produce stray spaces.
    CHECK(DeriveDisplayName("") == "");
    CHECK(DeriveDisplayName("_") == "");
    CHECK(DeriveDisplayName("__a__b__") == "A B");
}

TEST_CASE("DisplayNameForComponent strips the namespace first", "[editor]")
{
    CHECK(DisplayNameForComponent("Arcane::SpriteRenderer") == "Sprite Renderer");
    CHECK(DisplayNameForComponent("Arcane::EntityInfo") == "Entity Info");
    CHECK(DisplayNameForComponent("Transform") == "Transform");
    // Nested namespaces: only the trailing type name matters.
    CHECK(DisplayNameForComponent("A::B::PostProcess") == "Post Process");
    CHECK(DisplayNameForComponent("") == "");
}
```

- [ ] **Step 2: Run to verify it fails**

```
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal
```
Expected: FAIL to compile — `InspectorMeta.hpp` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Arcane/ArcaneEditor/src/InspectorMeta.hpp`:

```cpp
#pragma once

// InspectorMeta: every decision the Inspector makes ABOUT a field, separated
// from the drawing of it -- what it is called, what category it sits in,
// whether a search matches it, whether it still holds its default.
//
// Pure by construction (no ImGui, no registry mutation) because the test gate
// does not compile EditorPanels.cpp: anything left in the draw loop is
// desk-verified only. Same split as InspectorFields.hpp, which owns
// classification and write-backs; this file owns presentation decisions.
//
// The metadata all of this reads ALREADY EXISTS. Astra ships Category,
// DisplayName, Tooltip, Range, Hidden and ReadOnly attributes
// (Astra/Reflection/Attribute.hpp) attachable with ASTRA_REFLECT_ATTR; until
// this module they were declared and never read.

#include <Astra/Reflection/FieldInfo.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Arcane::Editor
{
    // "sortingLayer" -> "Sorting Layer". Follows UE's FName::NameToDisplayString
    // (UnrealNames.cpp:2693) minus its bIsBool `b`-prefix rule, which is a UE
    // naming convention this codebase does not share.
    [[nodiscard]] std::string DeriveDisplayName(std::string_view identifier);

    // The Astra::DisplayName attribute when the field carries one, else the
    // derived name. An explicit attribute always wins: it is the author saying
    // the derivation is wrong for this field.
    [[nodiscard]] std::string DisplayNameForField(const Astra::FieldInfo& field);

    // Component headers: strip the namespace, then derive.
    // "Arcane::SpriteRenderer" -> "Sprite Renderer".
    [[nodiscard]] std::string DisplayNameForComponent(std::string_view typeName);
}
```

Create `Arcane/ArcaneEditor/src/InspectorMeta.cpp`:

```cpp
#include "InspectorMeta.hpp"

#include <Astra/Reflection/Attribute.hpp>

#include <cctype>

namespace Arcane::Editor
{
    namespace
    {
        bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
        bool IsLower(char c) { return c >= 'a' && c <= 'z'; }
        bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        // A word boundary falls BEFORE index i.
        bool BreaksBefore(std::string_view s, std::size_t i)
        {
            if (i == 0 || i >= s.size()) return false;
            if (!IsUpper(s[i])) return false;

            const char prev = s[i - 1];
            // lower|digit -> Upper: the ordinary camelCase boundary.
            if (IsLower(prev) || IsDigit(prev)) return true;
            // Upper -> Upper followed by lower: the END of an acronym run, so
            // "HTTPServer" splits once (HTTP | Server) rather than per letter.
            if (IsUpper(prev) && i + 1 < s.size() && IsLower(s[i + 1])) return true;
            return false;
        }
    }

    std::string DeriveDisplayName(std::string_view identifier)
    {
        std::string spaced;
        spaced.reserve(identifier.size() + 8);
        for (std::size_t i = 0; i < identifier.size(); ++i)
        {
            const char c = identifier[i];
            if (c == '_') { spaced.push_back(' '); continue; }
            if (BreaksBefore(identifier, i)) spaced.push_back(' ');
            spaced.push_back(c);
        }

        // Split on spaces, capitalise each word, rejoin with single spaces --
        // which collapses runs and trims both ends in one pass.
        std::string out;
        out.reserve(spaced.size());
        bool wordStart = true;
        for (char c : spaced)
        {
            if (c == ' ') { wordStart = true; continue; }
            if (wordStart)
            {
                if (!out.empty()) out.push_back(' ');
                out.push_back(IsLower(c) ? static_cast<char>(c - 'a' + 'A') : c);
                wordStart = false;
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string DisplayNameForField(const Astra::FieldInfo& field)
    {
        if (const Astra::DisplayName* d = field.GetAttribute<Astra::DisplayName>())
            return std::string(d->name);
        return DeriveDisplayName(field.name);
    }

    std::string DisplayNameForComponent(std::string_view typeName)
    {
        const std::size_t sep = typeName.rfind("::");
        const std::string_view leaf =
            (sep == std::string_view::npos) ? typeName : typeName.substr(sep + 2);
        return DeriveDisplayName(leaf);
    }
}
```

- [ ] **Step 4: Add the source to ArcaneTests and regenerate**

In `Arcane/premake5.lua`, inside `project "ArcaneTests"`'s `files { ... }` block, after the `SceneSession.cpp` entry, add:

```lua
        -- Inspector polish: InspectorMeta (display-name derivation, attribute
        -- extraction, filter matching) source-compiles into the test exe so the
        -- [editor] units drive it directly. It is the whole surface the user
        -- reads in the Inspector, and EditorPanels.cpp is not compiled here --
        -- so anything left in the draw loop would have no coverage at all.
        "%{wks.location}/ArcaneEditor/src/InspectorMeta.cpp",
```

Then:
```
cd D:\dev\starworks\Gacha\Arcane
.\GenerateProjects.bat
```

- [ ] **Step 5: Run to verify it passes**

```
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[editor]" --rng-seed 6
.\ArcaneTests.exe "[editor]" --rng-seed 17
```
Expected: PASS under both seeds.

- [ ] **Step 6: Commit**

```bash
git add Arcane/ArcaneEditor/src/InspectorMeta.hpp Arcane/ArcaneEditor/src/InspectorMeta.cpp Arcane/premake5.lua Arcane/Tests/src/EditorInspectorMetaTest.cpp
git commit -m "feat(editor): derive readable Inspector names from identifiers"
```

---

### Task 2: Attribute extraction

**Files:**
- Modify: `Arcane/ArcaneEditor/src/InspectorMeta.hpp` / `.cpp`
- Test: `Arcane/Tests/src/EditorInspectorMetaTest.cpp`

**Interfaces:**
- Consumes: Task 1's header.
- Produces:
  - `std::string_view CategoryOfField(const Astra::FieldInfo&)` — empty when absent
  - `std::string_view TooltipOfField(const Astra::FieldInfo&)` — empty when absent
  - `std::optional<Astra::Range> RangeOfField(const Astra::FieldInfo&)`
  - `bool FieldIsReadOnly(const Astra::FieldInfo&)`
  - `bool FieldIsAttributeHidden(const Astra::FieldInfo&)`

`FieldIsAttributeHidden` is named for `Astra::Hidden`, the field attribute. It has nothing to do with `Arcane::Hidden`, the marker component that makes render submission skip an entity.

- [ ] **Step 1: Write the failing test**

Append to `Arcane/Tests/src/EditorInspectorMetaTest.cpp`:

```cpp
#include <Astra/Reflection/Reflection.hpp>

namespace
{
    // A throwaway reflected type carrying one of every attribute this module
    // reads, plus a bare field, so both the present and absent paths are real
    // rather than hand-built FieldInfo values.
    struct MetaProbe
    {
        float ranged   = 0.0f;
        int   locked   = 0;
        int   secret   = 0;
        float described = 0.0f;
        int   bare     = 0;
    };

    ASTRA_REFLECT_TYPE(MetaProbe)
        ASTRA_REFLECT_FIELD(MetaProbe, ranged)
            ASTRA_REFLECT_ATTR(Range, 0.0, 10.0, 0.5)
            ASTRA_REFLECT_ATTR(Category, "Bounds")
        ASTRA_REFLECT_FIELD(MetaProbe, locked)
            ASTRA_REFLECT_ATTR(ReadOnly)
        ASTRA_REFLECT_FIELD(MetaProbe, secret)
            ASTRA_REFLECT_ATTR(Hidden)
        ASTRA_REFLECT_FIELD(MetaProbe, described)
            ASTRA_REFLECT_ATTR(DisplayName, "Custom Label")
            ASTRA_REFLECT_ATTR(Tooltip, "explains itself")
        ASTRA_REFLECT_FIELD(MetaProbe, bare)
    ASTRA_REFLECT_TYPE_END()

    const Astra::FieldInfo& ProbeField(std::string_view name)
    {
        const Astra::TypeMeta* meta = Astra::GetMeta<MetaProbe>();
        REQUIRE(meta != nullptr);
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.name == name) return f;
        FAIL("no such field: " << std::string(name));
        return meta->fields[0];   // unreachable; keeps the compiler happy
    }
}

TEST_CASE("attributes are read when present", "[editor]")
{
    const auto range = RangeOfField(ProbeField("ranged"));
    REQUIRE(range.has_value());
    CHECK(range->min == 0.0);
    CHECK(range->max == 10.0);
    CHECK(range->step == 0.5);

    CHECK(CategoryOfField(ProbeField("ranged")) == "Bounds");
    CHECK(FieldIsReadOnly(ProbeField("locked")));
    CHECK(FieldIsAttributeHidden(ProbeField("secret")));
    CHECK(TooltipOfField(ProbeField("described")) == "explains itself");

    // An explicit DisplayName beats derivation -- the author overriding it is
    // the whole point of the attribute.
    CHECK(DisplayNameForField(ProbeField("described")) == "Custom Label");
}

TEST_CASE("a field with no attributes falls back cleanly", "[editor]")
{
    const Astra::FieldInfo& bare = ProbeField("bare");
    CHECK_FALSE(RangeOfField(bare).has_value());
    CHECK(CategoryOfField(bare).empty());
    CHECK(TooltipOfField(bare).empty());
    CHECK_FALSE(FieldIsReadOnly(bare));
    CHECK_FALSE(FieldIsAttributeHidden(bare));
    CHECK(DisplayNameForField(bare) == "Bare");
}
```

- [ ] **Step 2: Run to verify it fails**

Build as in Task 1. Expected: FAIL to compile — `CategoryOfField` and friends do not exist.

- [ ] **Step 3: Write the implementation**

Append to `InspectorMeta.hpp` (inside the namespace), and add `#include <Astra/Reflection/Attribute.hpp>` to the header's includes:

```cpp
    // The Astra::Category attribute's value, or empty for "uncategorised".
    // Empty is a real, common answer -- it renders ungrouped ABOVE any named
    // category, matching UE's NoCategory fallback
    // (DetailCategoryBuilderImpl.cpp:230).
    [[nodiscard]] std::string_view CategoryOfField(const Astra::FieldInfo& field);

    // Astra::Tooltip's text, or empty.
    [[nodiscard]] std::string_view TooltipOfField(const Astra::FieldInfo& field);

    // Astra::Range, for drag bounds. nullopt when the field has none, which is
    // NOT the same as a zero range -- an unbounded drag is the default.
    [[nodiscard]] std::optional<Astra::Range> RangeOfField(const Astra::FieldInfo& field);

    // Astra::ReadOnly: draw the field disabled rather than hiding it.
    [[nodiscard]] bool FieldIsReadOnly(const Astra::FieldInfo& field);

    // Astra::Hidden -- the FIELD ATTRIBUTE meaning "do not show this property".
    // Nothing to do with Arcane::Hidden, the marker component that makes render
    // submission skip an entity. The names collide; the meanings do not.
    [[nodiscard]] bool FieldIsAttributeHidden(const Astra::FieldInfo& field);
```

Append to `InspectorMeta.cpp` (inside the namespace):

```cpp
    std::string_view CategoryOfField(const Astra::FieldInfo& field)
    {
        if (const Astra::Category* c = field.GetAttribute<Astra::Category>())
            return c->category;
        return {};
    }

    std::string_view TooltipOfField(const Astra::FieldInfo& field)
    {
        if (const Astra::Tooltip* t = field.GetAttribute<Astra::Tooltip>())
            return t->text;
        return {};
    }

    std::optional<Astra::Range> RangeOfField(const Astra::FieldInfo& field)
    {
        if (const Astra::Range* r = field.GetAttribute<Astra::Range>())
            return *r;
        return std::nullopt;
    }

    bool FieldIsReadOnly(const Astra::FieldInfo& field)
    {
        return field.GetAttribute<Astra::ReadOnly>() != nullptr;
    }

    bool FieldIsAttributeHidden(const Astra::FieldInfo& field)
    {
        return field.GetAttribute<Astra::Hidden>() != nullptr;
    }
```

- [ ] **Step 4: Run to verify it passes**

Build, then:
```
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[editor]" --rng-seed 6
.\ArcaneTests.exe "[editor]" --rng-seed 17
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add Arcane/ArcaneEditor/src/InspectorMeta.hpp Arcane/ArcaneEditor/src/InspectorMeta.cpp Arcane/Tests/src/EditorInspectorMetaTest.cpp
git commit -m "feat(editor): read Astra's Category/Tooltip/Range/ReadOnly/Hidden attributes"
```

---

### Task 3: Filter matching

**Files:**
- Modify: `Arcane/ArcaneEditor/src/InspectorMeta.hpp` / `.cpp`
- Test: `Arcane/Tests/src/EditorInspectorMetaTest.cpp`

**Interfaces:**
- Produces:
  - `bool MatchesInspectorFilter(std::string_view componentDisplayName, std::string_view fieldDisplayName, std::string_view rawFieldName, std::string_view query)`
  - `bool ComponentMatchesFilter(std::string_view componentDisplayName, std::string_view query)`

The component-matches-everything rule lives here, in the pure function, precisely so a test pins it instead of it being an `if` buried in the draw loop.

- [ ] **Step 1: Write the failing test**

Append to `Arcane/Tests/src/EditorInspectorMetaTest.cpp`:

```cpp
TEST_CASE("the Inspector filter matches either spelling", "[editor]")
{
    // Friendly name and raw identifier both hit, so it does not matter whether
    // the user knows the code.
    CHECK(MatchesInspectorFilter("Sprite Renderer", "Sorting Layer", "sortingLayer", "sorting"));
    CHECK(MatchesInspectorFilter("Sprite Renderer", "Sorting Layer", "sortingLayer", "sortingLayer"));
    CHECK(MatchesInspectorFilter("Sprite Renderer", "Sorting Layer", "sortingLayer", "LAYER"));

    // A component-name hit keeps every field, so searching "sprite" shows the
    // whole component rather than none of it.
    CHECK(MatchesInspectorFilter("Sprite Renderer", "Tint", "tint", "sprite"));
    CHECK(ComponentMatchesFilter("Sprite Renderer", "sprite"));

    // Empty query matches everything -- the unfiltered case is not special-cased
    // at the call site.
    CHECK(MatchesInspectorFilter("Sprite Renderer", "Tint", "tint", ""));
    CHECK(ComponentMatchesFilter("Sprite Renderer", ""));

    // A genuine miss.
    CHECK_FALSE(MatchesInspectorFilter("Sprite Renderer", "Tint", "tint", "physics"));
    CHECK_FALSE(ComponentMatchesFilter("Sprite Renderer", "physics"));
}
```

- [ ] **Step 2: Run to verify it fails**

Build. Expected: FAIL to compile — `MatchesInspectorFilter` does not exist.

- [ ] **Step 3: Write the implementation**

Append to `InspectorMeta.hpp`:

```cpp
    // Case-insensitive substring, for the Inspector's search box.
    //
    // An empty query matches EVERYTHING, so the unfiltered case needs no
    // special-casing at the call site.
    [[nodiscard]] bool ComponentMatchesFilter(std::string_view componentDisplayName,
                                              std::string_view query);

    // Matches against the component name, the field's display name AND its raw
    // identifier -- a user who knows the source can search `sortingLayer` and a
    // user who does not can search `sorting`.
    //
    // A component-name hit matches every field in it: searching "sprite" should
    // show the whole Sprite Renderer, not an empty one. That rule lives here
    // rather than in the draw loop so a test pins it.
    [[nodiscard]] bool MatchesInspectorFilter(std::string_view componentDisplayName,
                                              std::string_view fieldDisplayName,
                                              std::string_view rawFieldName,
                                              std::string_view query);
```

Append to `InspectorMeta.cpp`, adding a helper in the anonymous namespace:

```cpp
        // Case-insensitive substring. ASCII-only folding is sufficient: these
        // are C++ identifiers and author-written attribute strings.
        bool ContainsFold(std::string_view haystack, std::string_view needle)
        {
            if (needle.empty()) return true;
            if (needle.size() > haystack.size()) return false;
            const auto lower = [](char c) {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            };
            for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
            {
                std::size_t j = 0;
                for (; j < needle.size(); ++j)
                    if (lower(haystack[i + j]) != lower(needle[j])) break;
                if (j == needle.size()) return true;
            }
            return false;
        }
```

and the two public functions:

```cpp
    bool ComponentMatchesFilter(std::string_view componentDisplayName,
                                std::string_view query)
    {
        return ContainsFold(componentDisplayName, query);
    }

    bool MatchesInspectorFilter(std::string_view componentDisplayName,
                                std::string_view fieldDisplayName,
                                std::string_view rawFieldName,
                                std::string_view query)
    {
        if (query.empty()) return true;
        if (ContainsFold(componentDisplayName, query)) return true;
        return ContainsFold(fieldDisplayName, query) || ContainsFold(rawFieldName, query);
    }
```

- [ ] **Step 4: Run to verify it passes**

Build, run `[editor]` under both seeds. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add Arcane/ArcaneEditor/src/InspectorMeta.hpp Arcane/ArcaneEditor/src/InspectorMeta.cpp Arcane/Tests/src/EditorInspectorMetaTest.cpp
git commit -m "feat(editor): Inspector filter matching over friendly and raw names"
```

---

### Task 4: Default-value comparison

> **CUT 2026-07-27, by user decision.** Almost no component value will ever be
> the default, so a revert affordance would be lit on nearly every field --
> clutter, not an affordance. This task was implemented (`4cfffcf6`) and then
> reverted, because its only consumer was Task 8, which was cut for the same
> reason. Left here, unexecuted, as a record of what was tried.
>
> Two findings survive the revert for anyone who revisits reset-to-default:
> - A **byte** comparison cannot work for `std::string`. Under `/MDd`,
>   `_ITERATOR_DEBUG_LEVEL=2` gives every `std::string` a `_Container_proxy*`,
>   and MSVC never initialises the small-string buffer past the terminator --
>   so two default-constructed empty strings are not byte-identical. This was
>   measured, not assumed: the reverted implementation's byte-comparison arm
>   failed against its own test before it was replaced with a value comparison.
> - The right foundation for a typed reset is Astra's `FieldInfo::setter`
>   (`ThirdParty/Astra/include/Astra/Reflection/FieldInfo.hpp:379-382`) -- a
>   type-erased assignment (`obj->*FieldPtr = *static_cast<const
>   DecayedType*>(inValue)`) that goes through `DecayedType::operator=` and so
>   deep-copies a `std::string` correctly. A raw-bytes approach never can.

**Files:**
- Modify: `Arcane/ArcaneEditor/src/InspectorMeta.hpp` / `.cpp`
- Test: `Arcane/Tests/src/EditorInspectorMetaTest.cpp`

**Interfaces:**
- Produces: `bool FieldDiffersFromDefault(const Astra::ComponentDescriptor& desc, const Astra::FieldInfo& field, const void* instance)` and `void ReadDefaultFieldBytes(const Astra::ComponentDescriptor& desc, const Astra::FieldInfo& field, void* outBytes)`

This is the engine half of reset-to-default. It builds a scratch default instance, reads the field out of it, and destroys it.

**The hazard:** the scratch must be aligned to `desc.alignment` and **must be destructed** — `EntityInfo` holds a `std::string`, so leaking it leaks heap. Use an aligned `operator new`/`operator delete` pair with `desc.Destruct` in between, on every path.

- [ ] **Step 1: Write the failing test**

Append to `Arcane/Tests/src/EditorInspectorMetaTest.cpp`:

```cpp
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>

#include <memory>

TEST_CASE("FieldDiffersFromDefault sees a changed field and not an unchanged one", "[editor]")
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*creg);

    const Astra::ComponentDescriptor* desc =
        creg->GetComponentDescriptorByHash(Astra::TypeID<Arcane::Transform>::Hash());
    REQUIRE(desc != nullptr);
    REQUIRE(desc->meta != nullptr);

    const Astra::FieldInfo* rotation = nullptr;
    for (const Astra::FieldInfo& f : desc->meta->fields)
        if (f.name == "rotation") rotation = &f;
    REQUIRE(rotation != nullptr);

    Arcane::Transform t;   // default-constructed: rotation is the default
    CHECK_FALSE(FieldDiffersFromDefault(*desc, *rotation, &t));

    t.rotation = 1.5f;
    CHECK(FieldDiffersFromDefault(*desc, *rotation, &t));

    // Reading the default back out gives the value a reset would write.
    float defaultRotation = -1.0f;
    ReadDefaultFieldBytes(*desc, *rotation, &defaultRotation);
    CHECK(defaultRotation == 0.0f);
}

TEST_CASE("FieldDiffersFromDefault handles a component holding a string", "[editor]")
{
    // EntityInfo carries a std::string, so the scratch default instance MUST be
    // destructed or this leaks. Run under a leak checker if you have one; the
    // assertion here is just that it behaves.
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*creg);

    const Astra::ComponentDescriptor* desc =
        creg->GetComponentDescriptorByHash(Astra::TypeID<Arcane::EntityInfo>::Hash());
    REQUIRE(desc != nullptr);
    REQUIRE(desc->meta != nullptr);

    const Astra::FieldInfo* name = nullptr;
    for (const Astra::FieldInfo& f : desc->meta->fields)
        if (f.name == "name") name = &f;
    REQUIRE(name != nullptr);

    Arcane::EntityInfo info;
    CHECK_FALSE(FieldDiffersFromDefault(*desc, *name, &info));

    info.name = "Player";
    CHECK(FieldDiffersFromDefault(*desc, *name, &info));
}
```

- [ ] **Step 2: Run to verify it fails**

Build. Expected: FAIL to compile — `FieldDiffersFromDefault` does not exist.

- [ ] **Step 3: Write the implementation**

Append to `InspectorMeta.hpp`, adding `#include <Astra/Component/Component.hpp>`:

```cpp
    // Copy the field's DEFAULT bytes (from a scratch default-constructed
    // instance of the owning component) into `outBytes`, which must have room
    // for field.size.
    //
    // Builds and DESTROYS a whole component to read one field. That is
    // deliberate: a default is whatever the type's NSDMIs and default
    // constructor produce, and there is no cheaper way to ask. Called only on
    // user interaction, never per frame.
    void ReadDefaultFieldBytes(const Astra::ComponentDescriptor& desc,
                               const Astra::FieldInfo& field,
                               void* outBytes);

    // Whether `instance`'s field still holds its default. Compares BYTES, so a
    // type whose equality differs from its representation (padding, a string's
    // capacity) can report a false difference -- acceptable for deciding
    // whether to offer a revert affordance, and never used to skip a write.
    [[nodiscard]] bool FieldDiffersFromDefault(const Astra::ComponentDescriptor& desc,
                                               const Astra::FieldInfo& field,
                                               const void* instance);
```

Append to `InspectorMeta.cpp`, adding `#include <cstring>` and `#include <new>`:

```cpp
    namespace
    {
        // RAII scratch instance: aligned storage + DefaultConstruct on entry,
        // Destruct + aligned delete on exit. EntityInfo holds a std::string, so
        // skipping the destruct leaks heap on every hover.
        class ScratchDefault
        {
        public:
            explicit ScratchDefault(const Astra::ComponentDescriptor& d) : m_desc(d)
            {
                if (d.size == 0) return;   // tag component: no storage, no fields
                m_mem = ::operator new(d.size, std::align_val_t(d.alignment));
                d.DefaultConstruct(m_mem);
            }
            ~ScratchDefault()
            {
                if (!m_mem) return;
                m_desc.Destruct(m_mem);
                ::operator delete(m_mem, std::align_val_t(m_desc.alignment));
            }
            ScratchDefault(const ScratchDefault&) = delete;
            ScratchDefault& operator=(const ScratchDefault&) = delete;

            [[nodiscard]] const void* Get() const { return m_mem; }

        private:
            const Astra::ComponentDescriptor& m_desc;
            void* m_mem = nullptr;
        };
    }

    void ReadDefaultFieldBytes(const Astra::ComponentDescriptor& desc,
                               const Astra::FieldInfo& field,
                               void* outBytes)
    {
        ScratchDefault scratch(desc);
        if (!scratch.Get()) return;
        std::memcpy(outBytes,
                    static_cast<const std::byte*>(scratch.Get()) + field.offset,
                    field.size);
    }

    bool FieldDiffersFromDefault(const Astra::ComponentDescriptor& desc,
                                 const Astra::FieldInfo& field,
                                 const void* instance)
    {
        if (!instance) return false;
        ScratchDefault scratch(desc);
        if (!scratch.Get()) return false;
        return std::memcmp(static_cast<const std::byte*>(instance) + field.offset,
                           static_cast<const std::byte*>(scratch.Get()) + field.offset,
                           field.size) != 0;
    }
```

**Note for the implementer:** verify `ComponentDescriptor`'s member names (`DefaultConstruct`, `Destruct`, `size`, `alignment`, `meta`) against `ThirdParty/Astra/include/Astra/Component/Component.hpp` before writing, and adapt if they differ. Also verify `ComponentRegistry::GetComponentDescriptorByHash` is the real accessor name.

If a byte comparison proves unworkable for `std::string` fields (a default-constructed string's internal buffer pointer differs from another's), replace `FieldDiffersFromDefault`'s string path with a typed comparison driven by `InspectorFields::ClassifyField`, and say so in your report — do not leave a comparison that reports every string as modified.

- [ ] **Step 4: Run to verify it passes**

Build, run `[editor]` under both seeds. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add Arcane/ArcaneEditor/src/InspectorMeta.hpp Arcane/ArcaneEditor/src/InspectorMeta.cpp Arcane/Tests/src/EditorInspectorMetaTest.cpp
git commit -m "feat(editor): compare a field against its component's default"
```

---

### Task 5: Readable labels, tooltips, ReadOnly and Hidden in the panel

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (`DrawInspectorPanel`, from ~line 1310, and the `ImGuiFieldVisitor` it drives)

**Interfaces:**
- Consumes: `DisplayNameForField`, `DisplayNameForComponent`, `TooltipOfField`, `FieldIsReadOnly`, `FieldIsAttributeHidden`, `RangeOfField`.

**No unit test — by design.** This is ImGui. Its correctness rests on Tasks 1-4, which are tested. Do not invent a test; the verification is the build, an unchanged gate, and desk-verify.

- [ ] **Step 1: Use display names for component headers**

In `DrawInspectorPanel`, the header currently reads:

```cpp
const bool open = ImGui::CollapsingHeader(typeName.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
```

Change it to use the friendly name while keeping `typeName` for the hide-list check and the `PushID` (both of which must stay keyed on the real type):

```cpp
// Friendly header, raw type in the tooltip: the label should read as words,
// but the C++ type name is what you search the source for.
const std::string headerLabel = Arcane::Editor::DisplayNameForComponent(typeName);
const bool open = ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", typeName.c_str());
```

- [ ] **Step 2: Use display names, tooltips, ranges and the two attributes for fields**

In the `ImGuiFieldVisitor::Visit` implementation, at the top of the per-field work: skip `Astra::Hidden` fields, wrap `Astra::ReadOnly` fields in `BeginDisabled`/`EndDisabled`, label with the display name, and attach the tooltip. Add `#include "InspectorMeta.hpp"` to `EditorPanels.cpp`.

```cpp
// Astra::Hidden -- the FIELD ATTRIBUTE. Nothing to do with Arcane::Hidden, the
// marker component that makes render submission skip an entity.
if (Arcane::Editor::FieldIsAttributeHidden(field))
    return;

const std::string label = Arcane::Editor::DisplayNameForField(field);
const std::string_view tip = Arcane::Editor::TooltipOfField(field);
const bool readOnly = Arcane::Editor::FieldIsReadOnly(field);
if (readOnly) ImGui::BeginDisabled();
```

with the matching `if (readOnly) ImGui::EndDisabled();` at the end of the visit, and every widget switched from the raw `field.name` to `label.c_str()`. After each widget:

```cpp
// The raw identifier is always in the tooltip, so a friendly label never
// costs you the ability to find the field in source.
if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
{
    if (!tip.empty())
        ImGui::SetTooltip("%.*s\n\n%.*s", (int)tip.size(), tip.data(),
                          (int)field.name.size(), field.name.data());
    else
        ImGui::SetTooltip("%.*s", (int)field.name.size(), field.name.data());
}
```

For float and int widgets, honour a `Range` when present by using the clamped drag form; keep the existing unclamped call when `RangeOfField` returns `nullopt`. Read the existing widget calls and preserve every other argument exactly — especially the mixed-value blanking and the gesture bracketing, which must behave identically.

- [ ] **Step 3: Build and verify the gate is unchanged**

```
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "~[gpu]" --rng-seed 6
```
Expected: the full non-GPU suite passes with the count Task 4 left it at. This task adds no assertions.

- [ ] **Step 4: Confirm the editor relinked**

```
Get-Item D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe | Select-Object LastWriteTime
```
It must be newer than every file under `Arcane/ArcaneEditor/src`. **The gate does not compile `EditorPanels.cpp`, so this is the only evidence the change built at all.**

- [ ] **Step 5: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(editor): readable Inspector labels, tooltips, ranges, ReadOnly/Hidden"
```

---

### Task 6: The search box

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (`InspectorState`, ~line 153), `Arcane/ArcaneEditor/src/EditorPanels.cpp`

**Interfaces:**
- Consumes: `MatchesInspectorFilter`, `ComponentMatchesFilter`, `DisplayNameForField`, `DisplayNameForComponent`.

**No unit test — the matching logic is already covered by Task 3.**

- [ ] **Step 1: Give InspectorState a search buffer**

In `EditorPanels.hpp`:

```cpp
    struct InspectorState
    {
        Arcane::TransactionId gestureTxn = Arcane::TransactionId::None;
        // Live search text. A fixed buffer rather than std::string because
        // ImGui::InputText writes into it directly; 128 is far past any
        // plausible field name.
        char searchBuffer[128] = {};
    };
```

- [ ] **Step 2: Draw the search box and filter**

In `DrawInspectorPanel`, after the selection header and its `ImGui::Separator()`, before the component loop:

```cpp
// Search, UE's Details-panel shape (SDetailsViewBase.cpp:1016 --
// OnFilterTextChanged -> FilterView). Filters components AND fields live.
ImGui::SetNextItemWidth(-FLT_MIN);
ImGui::InputTextWithHint("##inspector_search", ICON_LC_SEARCH " Filter",
                         state.searchBuffer, sizeof(state.searchBuffer));
const std::string_view query(state.searchBuffer);
```

Inside the component loop, after `headerLabel` is computed and before the header is drawn, decide whether the component survives. A component whose NAME matches keeps all its fields; otherwise it needs at least one matching field, or it is skipped entirely:

```cpp
// A component is shown when its own name matches, or when at least one of
// its fields does. Computing this BEFORE drawing the header is what lets a
// component with no matches disappear rather than showing an empty section.
bool componentVisible = Arcane::Editor::ComponentMatchesFilter(headerLabel, query);
if (!componentVisible && ci.meta)
{
    for (const Astra::FieldInfo& f : ci.meta->fields)
    {
        if (Arcane::Editor::FieldIsAttributeHidden(f)) continue;
        if (Arcane::Editor::MatchesInspectorFilter(
                headerLabel, Arcane::Editor::DisplayNameForField(f), f.name, query))
        {
            componentVisible = true;
            break;
        }
    }
}
if (!componentVisible)
    continue;   // BEFORE the PushID, so the ID stack stays balanced
```

**The `continue` must sit before `ImGui::PushID`.** The existing code documents that nothing between the push and its pop may `continue` — placing this check after the push would unbalance the ID stack.

Then in the field visitor, skip a field the filter rejects, unless the component name itself matched (in which case everything shows). Pass the query and the component's display name into `ImGuiFieldVisitor` as members set before the visit.

- [ ] **Step 3: Build, verify the gate is unchanged, confirm the editor relinked**

Same commands as Task 5 Steps 3-4.

- [ ] **Step 4: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(editor): live search in the Inspector"
```

---

### Task 7: Category grouping

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp`

**Interfaces:**
- Consumes: `CategoryOfField`.

**No unit test — grouping is presentation; `CategoryOfField` is covered by Task 2.**

- [ ] **Step 1: Group fields by category within a component**

The reflection visitor walks fields in declaration order, so grouping needs two passes over `ci.meta->fields`: first the uncategorised, then each named category under a collapsible sub-header. Uncategorised comes FIRST, matching UE's `NoCategory` fallback (`DetailCategoryBuilderImpl.cpp:230`).

Inside the `if (open)` branch, replace the single visitor drive with:

```cpp
// Categories, UE's Details shape: uncategorised fields first (the
// NoCategory bucket, DetailCategoryBuilderImpl.cpp:230), then each named
// category under its own collapsible sub-header. Order of named categories
// is first-appearance in declaration order, so it is stable and matches how
// the component reads in source.
std::vector<std::string_view> categories;   // named, in first-appearance order
for (const Astra::FieldInfo& f : ci.meta->fields)
{
    const std::string_view cat = Arcane::Editor::CategoryOfField(f);
    if (cat.empty()) continue;
    if (std::find(categories.begin(), categories.end(), cat) == categories.end())
        categories.push_back(cat);
}
```

then drive the visitor once with an active-category filter set to empty (uncategorised), and once per entry in `categories` inside an `ImGui::TreeNodeEx(...,  ImGuiTreeNodeFlags_DefaultOpen)` scoped by `ImGui::PushID(category)`. Add a `std::string_view activeCategory` member to `ImGuiFieldVisitor` that makes `Visit` skip any field whose `CategoryOfField` differs.

**Do not reorder the fields themselves** — within a category they keep declaration order, which is what makes a component read the same in the Inspector as in source.

- [ ] **Step 2: Build, verify the gate is unchanged, confirm the editor relinked**

Same commands as Task 5 Steps 3-4.

- [ ] **Step 3: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(editor): group Inspector fields by category"
```

---

### Task 8: Reset-to-default affordance

> **CUT 2026-07-27, by user decision.** Almost no component value will ever be
> the default, so a revert affordance would be lit on nearly every field --
> clutter, not an affordance. Never implemented. Task 4, which existed solely
> to feed this task, was reverted as a consequence (`4cfffcf6` implemented,
> then reverted the same day).

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp`

**Interfaces:**
- Consumes: `FieldDiffersFromDefault`, `ReadDefaultFieldBytes`, and `InspectorFields`' existing `ApplyBoolEdit` / `ApplyIntEdit` / `ApplyFloatEdit` / `ApplyGuidEdit`.

- [ ] **Step 1: Draw the affordance and apply the reset**

For each field whose `FieldDiffersFromDefault` is true on the primary entity, draw a small revert button on the same line, right-aligned, UE's `SResetToDefaultPropertyEditor` shape. Clicking it writes the default through **the same `Apply*Edit` path a manual edit uses**, inside the same gesture bracketing, so a reset is one undo entry exactly like any other edit.

Multi-select: show the affordance when ANY selected entity differs from the default, and reset ALL of them — consistent with how every other field edit in this panel fans out.

Read the default with `ReadDefaultFieldBytes` into a correctly-typed local, then dispatch on the field's `InspectorFields::FieldKind` to the matching `Apply*Edit`. **Do not `memcpy` the default straight into the live component** — that would bypass the write-back path and the undo bracketing.

A `ReadOnly` field never gets the affordance.

- [ ] **Step 2: Build, verify the gate is unchanged, confirm the editor relinked**

Same commands as Task 5 Steps 3-4.

- [ ] **Step 3: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(editor): per-field reset-to-default in the Inspector"
```

---

### Task 9: Annotate the engine's scene components

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/Components.hpp`

Purely additive: a component with no attributes renders exactly as it did after Task 5, so Sandbox, PlaygroundGame and the external Aphelyon module need no changes and cannot break.

- [ ] **Step 1: Add Category, Tooltip and Range attributes**

Read the existing `ASTRA_REFLECT_TYPE` blocks in `Components.hpp` first and follow their formatting. Annotate:

- **`Transform`** — `position`, `rotation`, `scale` under `Category("Transform")`. Tooltip on `rotation` stating the unit is radians. Tooltip on `position` stating the units are metres (MKS).
- **`SpriteRenderer`** — `textureId`, `material`, `tint` under `Category("Appearance")`; `size`, `shape` under `Category("Shape")`; `sortingLayer`, `orderInLayer` under `Category("Sorting")`. **Tooltip on `size` stating the units are WORLD METRES, not pixels** — the field already carries that warning as a code comment, and the Inspector is where someone would actually read it.
- **`PostProcess`** — its fields under `Category("Post Processing")`.
- **`EntityInfo`** — `name` with a tooltip explaining it is the Outliner display name; `id` marked `ASTRA_REFLECT_ATTR(ReadOnly)`, since a stable identity is not something to hand-edit.

Add a `Range` only where a bound is genuinely real. Do not invent bounds for `position` or `size` — a scene may legitimately be any size.

**Do NOT change `SpriteRenderer::size`'s default of `{32, 32}`.** It is 32 metres and it is wrong, but changing a component default shifts every existing scene and is out of scope for this arc. The tooltip is the deliverable here.

- [ ] **Step 2: Build and run the FULL gate under both seeds**

```
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "~[gpu]" --rng-seed 6
.\ArcaneTests.exe "~[gpu]" --rng-seed 17
```
Expected: identical pass counts under both seeds. Attributes are metadata — **if any scene-serialization test changes behaviour, stop**: that means an attribute altered `IsSerializable()`, which it must not.

- [ ] **Step 3: Confirm the editor relinked, then commit**

```bash
git add Arcane/Arcane/src/Arcane/Scene/Components.hpp
git commit -m "feat(arcane): categorise and document the scene components' fields"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §3 `DisplayNameForField` / `DisplayNameForComponent` / derivation | 1 |
| §3 `CategoryOfField`, `RangeOfField`, `ReadOnly`, `Astra::Hidden` | 2 |
| §3 `MatchesInspectorFilter` incl. component-matches-all | 3 |
| §3 premake entry + regeneration | 1 |
| §3 naming hazard (`Astra::Hidden` vs `Arcane::Hidden`) | Global Constraints, 2, 5 |
| §4 search box, grouping, labels, tooltips, ReadOnly/Hidden, ranges | 5, 6, 7 |
| §4 no regression in PushID / pendingRemove / intersection / mixed / gestures | 5, 6, 7 (stated per task) |
| §5 reset-to-default, resolved as feasible | CUT 2026-07-27 (was 4, 8) -- see per-task notes |
| §6 annotate `Components.hpp`, do not change the size default | 9 |
| §7 test roster | 1, 2, 3 (4 implemented then CUT -- its three test cases were reverted with it) |
| §8 exe-timestamp verification | Global Constraints, 5, 6, 7 (8 CUT -- see per-task note) |

**Placeholders:** none. Tasks 5-7 have no unit tests by design and say so with the reason, rather than leaving a gap unexplained. Task 4 carried an explicit fallback instruction if byte comparison proved unworkable for `std::string` (it did; see the per-task note), and Task 8 was cut before that instruction mattered.

**Type consistency:** `DeriveDisplayName`, `DisplayNameForField`, `DisplayNameForComponent`, `CategoryOfField`, `TooltipOfField`, `RangeOfField`, `FieldIsReadOnly`, `FieldIsAttributeHidden`, `ComponentMatchesFilter`, `MatchesInspectorFilter` are each defined once and used with the same spelling and signature throughout. `ReadDefaultFieldBytes` and `FieldDiffersFromDefault` were implemented for Task 4 and then reverted with it (2026-07-27); they no longer exist in the codebase.

**Three places the implementer must verify against the codebase rather than trust this plan:** `ComponentDescriptor`'s exact member spellings (`DefaultConstruct`, `Destruct`, `size`, `alignment`, `meta`); `ComponentRegistry`'s descriptor accessor name; and the existing `ImGuiFieldVisitor`'s structure, since Tasks 5-8 all modify it and this plan describes its shape rather than reproducing it.

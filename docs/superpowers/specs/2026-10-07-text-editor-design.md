# Arcane Text Editor -- design

**Status:** Proposed (brainstorm 2026-10-07; sections 1-6 approved in conversation; revision 2 folds in an independent review -- see s13; awaiting written-spec review)
**Scope:** spec 1 of 2. Spec 2 (editor-wide Command Palette, Goto Anything, Find in Files) follows this one and consumes the hooks defined in s9.
**Research:** lite-xl (`D:\dev\_reference\lite-xl-master`, MIT), Zed (`D:\dev\_reference\zed-main`, GPL editor crates / Apache `sum_tree`), Zep, ImGuiColorTextEdit (BalazsJako + santaclose + goossens forks), Sublime Text's `.sublime-syntax` model, tree-sitter, Lexilla. Findings are summarised where they drive a decision.

---

## 1. Goal

A Sublime-Text-4-class text editor that lives inside the Arcane Editor as an ordinary document: its own tab that docks, undocks and drags like every other Arcane document.

- **It edits all text except C++.** C++ stays in the external IDE permanently -- the debugger integration matters more, and Arcane never intends to replace that. `.cpp`/`.hpp`/`.h` keep opening through `IdeLaunch`.
- **It is the fallback viewer.** Any text file with no dedicated Arcane document editor opens here, instead of "no editor registered".
- **It is fast on large files** (logs, diagnostics) and correct on every file it saves.
- **Power-user features in v1:** Sublime's multi-cursor model and an opt-in, near-full vim emulation.

### 1.1 Decisions (user, 2026-10-07)

| # | Decision |
|---|---|
| D1 | Full document tab, all non-C++ text, fallback viewer for anything without a dedicated editor. Bar: Sublime Text 4. |
| D2 | v1 features: baseline (highlighting, undo/redo, line numbers, brackets, folding, save/dirty) + **vim mode** + **multi-cursor**. |
| D3 | Command Palette, Goto Anything and Find in Files are **editor-wide**, not text-editor features: a separate spec 2, written after this one. |
| D4 | Highlighting = **data-driven pattern grammars** (lite-xl's model, made Arcane-native). Tree-sitter may be added later for structure, behind the same scope interface. |
| D5 | **No other editors' files.** Arcane does not load `.sublime-syntax`, `.tmLanguage`, `.tmTheme`, VS Code themes or `.vimrc`. Grammars, colours and vim configuration are Arcane-native. (Porting grammar *content* from MIT sources into the Arcane format, with attribution, is fine.) |
| D6 | Shortcuts are **unified** with the existing editor action table (s7). |
| D7 | Vim depth: **near-full emulation, as far as reasonable** -- a selling point for the power users Arcane targets (s6). |
| D8 | Split view of one buffer moves **into v1** (vim's `:sp`/`:vs` need it). |
| D9 | A dev-only **headless Neovim oracle** generates/verifies the vim behaviour tables (s11). |
| D10 | "Open as Text" on an asset that has a dedicated editor opens **read-only with an Enable editing unlock** (s5.4). |
| D11 | Regex engine: **PCRE2 with JIT**, vendored; the one regex *engine* for grammars, find/replace and vim. Vim patterns reach it through a vim-to-PCRE2 translator (s6.5). |
| D12 | **Release gate after step 4** plus the step-6 essentials (s12). Vim ships behind an **Experimental** label that widens with each vim pass and drops when the last pass and the oracle suite are green. |
| D13 | Vim flavour: **Neovim defaults** (`Y` = `y$`, `hlsearch`/`incsearch` on, `startofline` off, `&` remapped, `nrformats` without octal). Classic-Vim behaviour stays reachable through options. |

### 1.2 Non-goals

- C++ editing, debugging, IntelliSense/LSP, build-error squiggles from the compiler.
- Vimscript, vim plugins, `:!` shell filters, `:terminal`, digraphs, spell check. (Digraphs and an opt-in `:!` that confirms before running are cheap later additions if wanted.)
- Loading other editors' grammar/theme/config files (D5).
- A hex/binary viewer. Binary files are refused with a message.
- Minimap, soft wrap, word completion, diff view: later steps, not v1 (s12.3).
- Collaborative editing. (Zed's CRDT machinery -- Lamport clocks, fragment/locator anchors, version vectors -- is deliberately not reproduced.)
- **Text-rendering limits (inherited from ImGui):** no complex-script shaping (Arabic, Devanagari render wrong), no bidi, no automatic font fallback (CJK needs a fallback font merged into the atlas; not shipped in v1), no colour emoji. Grapheme-correct cursor movement (s5.1) does not fix rendering; these are display limits, stated so nobody mistakes them for bugs.

---

## 2. Architecture

All code lives under `ArcaneEditor/src/TextEditor/`. Four layers; each depends only on the layers above it.

| Layer | Units | Depends on |
|---|---|---|
| **Core** (pure C++, no ImGui) | `Rope`, `TextBuffer`, `EolMap`, `SelectionSet`, `TextCommands`, `FoldModel`, `DisplayMap`, `TextSearch` | nothing (PCRE2 for `TextSearch`) |
| **Syntax** | `Grammar`, `GrammarRegistry`, `Tokenizer`, `HighlightCache`, `ScopeMap` | Core, PCRE2 |
| **Vim** | `VimState`, `VimKeymap`, `VimMotions`, `VimOperators`, `VimTextObjects`, `VimRegisters`, `VimEx` | Core (+ Syntax for comment tokens and `iskeyword`) |
| **View + Document** | `TextEditorView`, `TextDocument`, `TextDocumentInspectorPage`, `TextStatusStrip`, `FindBar` | all above, ImGui, DocumentHost |

Why this split:
- Core and Syntax are unit-testable with no window (s11).
- Vim is a layer, not a fork: with `editor.text.vim.enabled` off, no vim code runs and the core carries no vim state.
- The shader editor's snippet/body fields and the crash log tail (today `ImGui::InputTextMultiline`) can adopt the same core later; that migration is out of scope here.

### 2.1 The single command layer

`TextCommands` is the only place text is changed or selections are moved. The normal keymap (s7), the vim layer (s6), the find bar, and spec 2's palette all call it. Every command:
1. runs inside a transaction (`TextBuffer::Transact`),
2. transforms every selection with a per-selection function (`MoveWith`, `MoveHeadsWith`, `MoveCursorsWith`),
3. submits one batched `Edit[]` to the buffer.

This is what makes every command multi-cursor-correct and atomic, and what lets vim operators reuse normal commands (s6.2).

---

## 3. Data model

### 3.1 Rope

- A balanced B+ tree of UTF-8 leaves. A **concrete rope written for Arcane, informed by Zed's `sum_tree`** (Apache-2.0; attribution kept in `ThirdParty/NOTICES` as a courtesy). It is not a port of `sum_tree`'s generic `Summary`/`Dimension` machinery -- translating Rust generics is a rewrite either way, and one concrete summary is simpler. Zed's `rope`/`text` crates are GPL and are design reference only.
- **Leaf size is measured, not assumed.** Step 1 benchmarks leaf targets from 128 B to 1 KB on the s11.5 workloads (100 MB log open, random edits, row seeks, snapshot cost across threads) and fixes the constant from the results. Zed's 64-128 B suits Zed's file sizes; Arcane's large-log target may favour Ropey-style ~1 KB leaves (a 512 MB file at 1 KB is ~0.5 M leaves, not ~5 M). Node refcounts are atomic because snapshots cross threads, which is part of what the benchmark measures.
- Each node caches a summary: `{bytes, newlines, lastLineBytes, longestLineBytes}`. Byte offset <-> `Point{row, byteCol}` is O(log n) by seeking on one dimension while accumulating the other; no line-start table is rebuilt per edit.
- **Persistent:** an edit copies only the touched path. A `RopeSnapshot` is O(1) to take and immutable, so the highlighter worker and large-file search read a consistent snapshot while the user types. Every snapshot carries the buffer `version` it was taken at (s4.4).

### 3.2 Positions and anchors

- Internal positions are byte offsets, or `Point{row, byteCol}`. Display columns (tab width, wide glyphs) are computed only by the view.
- Anything that must survive edits -- selections, vim marks, folds, bookmarks -- is an **anchor**: `{offset, bias}`, shifted by every edit (left bias stays before an insertion at its offset; right bias moves after). Anchors live in sorted vectors per owner; shifting is O(anchors) per edit, which is fine because anchor counts stay small.
- **Search matches are never anchors.** A big log under `hlsearch` can hold hundreds of thousands of matches; shifting them per edit would be the dominant cost. Matches are recomputed per snapshot for the visible range (plus a background total count) and discarded on the next version.
- Turning all matches into cursors (Alt+Enter, vim `gb` repeated) creates real selections; above `editor.text.maxCursors` (default 10 000) it asks first.

### 3.3 Edits, transactions, undo

- `TextBuffer::Edit(span<const EditOp>)` takes sorted, non-overlapping `{range, newText}` ops and applies them in one pass, producing `EditRecord{range, oldText, newText}`s.
- Transactions nest (`Begin`/`End` depth; empty transactions are dropped). An undo entry is `{records[], selectionsBefore, selectionsAfter}` -- undo restores the cursors as well as the text.
- **Grouping:** consecutive typing transactions merge within `editor.text.undoGroupMs` (default 300) unless a boundary intervenes (cursor moved by a non-typing command, a save, an explicit break). Vim groups a whole insert session explicitly (`GroupUntil`).
- History is per document, survives save, and is capped by memory (`editor.text.undoMemoryMB`, default 64), dropping oldest entries.
- **Dirty:** `changeId != savedChangeId`. Undoing back to the saved state reads clean again; editing after undoing past the save point makes the saved state unreachable (dirty until saved).

### 3.4 Selections

- `Selection{id, anchor, head, goalColumn}`; `reversed` is derived from anchor > head. `goalColumn` keeps the visual column across up/down through short lines.
- `SelectionSet` keeps selections sorted and disjoint. Merge rule: overlapping, same start, or a cursor touching another selection's boundary. The **newest** selection is primary; the view follows it.
- A drag in progress is a separate `pending` selection merged on release.

### 3.5 Files, encodings, line endings

| Concern | Rule |
|---|---|
| Line endings | **Every line keeps its original ending.** The rope stores LF; an `EolMap` records the file's dominant style plus a sparse set of exception rows (CRLF in an LF file, LF in a CRLF file, lone `\r`), shifted by edits like anchors. A lone `\r` (classic Mac) is a line break. New lines take the dominant style. An unedited file saves back **byte-identical**; a one-character edit changes one line in version control. Converting is explicit: the status strip's line-ending menu (and a `text.convertLineEndings` action) rewrites every line in one undo step. Mixed files show a status-strip note but are never silently normalised. |
| Encodings | UTF-8 (BOM preserved if present) and UTF-16 LE/BE with BOM are decoded and saved back in the same encoding. |
| Invalid bytes / binary | Invalid UTF-8 or binary content (NUL bytes in the first 8 KB) opens **read-only** with a banner explaining why; saving can never corrupt the file. Pure binary formats with no text editor are refused with a message. |
| Size thresholds | Above `editor.text.syntaxMaxMB` (default 32), stateful grammars stop highlighting; **line-local grammars keep highlighting at any size** (s4.4), so logs never lose their level colours. Above `editor.text.openMaxMB` (default 512) the file is not opened ("open externally" notice). |
| Long lines | A line longer than `editor.text.longLineMaxBytes` (default 64 KB; e.g. one-line minified JSON) is tokenized only up to the cap; the rest of that line draws plain, and the **next** line restarts from the grammar's root state (a resync) so nothing after it is held hostage. Drawing such a line clips to the visible horizontal window. |
| Append-only growth | Detected cheaply: the size grew **and** a hash of a small window (4 KB) just before the old end still matches. Only then is the new tail appended instead of reloading. A **Follow** toggle (status strip) keeps the view at the end, like `tail -f`. A shrink (truncation) or a window mismatch (rotation, rewrite) is a full reload under the s5.3 external-change rules. |

### 3.6 Display map

One `DisplayMap` sits between the buffer and the view and owns the buffer-row <-> display-row mapping. In v1 it has a single layer, **folds**: a sorted list of folded row ranges, with display row = buffer row minus hidden rows above (prefix sums). Everything row-visual goes through it -- drawing, scrolling, vim `j`/`k` vs `gj`/`gk`, `H M L`, `scrolloff`, `zj`/`zk`. Soft wrap (the first post-v1 follow-up) becomes a second layer under the same interface instead of a retrofit through every caller. Tab expansion stays a draw-time computation per visible line.

---

## 4. Syntax highlighting

### 4.1 Grammar files: `.arcsyntax`

JSON, in the same style as `.arctheme`. Search path, later entries overriding earlier by `name`:
1. engine `data/Syntaxes/`
2. project `Config/Syntaxes/`
3. plugin-registered grammars

Saving a grammar reloads it live (and re-highlights open documents using it).

```json
{ "format": "arcsyntax", "version": 1, "name": "JSON", "scope": "source.json",
  "files": [".json", ".arcmat", ".arcproj", ".arcscene", ".arctheme", ".arcsyntax", ".meta"],
  "firstLine": "^\\s*[\\[{]",
  "comment": { "line": "//", "block": ["/*", "*/"] },
  "brackets": [["{","}"], ["[","]"]],
  "autoClose": [["\"","\""], ["{","}"], ["[","]"]],
  "indent": { "increase": "[\\[{]\\s*$", "decrease": "^\\s*[\\]}]" },
  "wordChars": "A-Za-z0-9_",
  "symbols": [ { "match": "\"([^\"]+)\"\\s*:", "name": 1, "kind": "key" } ],
  "lineLocal": false,
  "patterns": [
    { "match": "\"(?:[^\"\\\\]|\\\\.)*\"(?=\\s*:)", "scope": "entity.name.key.json" },
    { "begin": "\"", "end": "\"", "escape": "\\\\.", "scope": "string.quoted.double.json" },
    { "match": "-?\\d+(\\.\\d+)?([eE][+-]?\\d+)?", "scope": "constant.numeric.json" },
    { "match": "\\b(true|false|null)\\b", "scope": "constant.language.json" } ] }
```

Rule kinds:
- `match` + `scope` (+ `captures: {"1": scope, ...}`): a single-line token.
- `begin` / `end` / `escape` + `scope` / `contentScope` + nested `patterns`: a region that may span lines.
- `keywords: {scope: [words]}` on an identifier rule: table lookup after matching (lite-xl's `symbols`).
- `embed: "<grammar name>"` inside a region: another language for the region's content (Markdown fenced blocks).
- `foldMarkers: {begin, end}`: explicit fold regions in addition to indentation.
- `symbols`: patterns that feed the symbol provider (s9).
- `lineLocal: true`: the grammar has **no rule that spans lines** (no multi-line regions), so a line's tokens never depend on earlier lines. Validated on load (a line-local grammar with a multi-line region is an error). Line-local grammars -- Arcane log, INI/cfg, CSV-like logs, plain key/value files -- are tokenized on demand for visible lines only, at any file size (s4.4).

Context-dependent scopes use ordinary rules, not special syntax: the JSON key above is a single-line `match` with a lookahead, listed before the generic string region so it wins the tie at the same position.

Detection order: file extension (`files`) -> `firstLine` regex -> the user's choice in the status-strip language picker, remembered per extension in `editor.text.languageByExtension`.

The format is validated on load; a bad grammar produces a Problems row naming the file and rule, and the language falls back to Plain Text.

### 4.2 Regex engine: PCRE2 + JIT

Vendored under `ThirdParty/pcre2` (BSD), built as a static lib via premake with JIT enabled, 8-bit code units. One engine for grammar rules, in-file find/replace, vim `/` `?` `:s` `:g` (vim syntax is translated first, s6.5). `std::regex` is too slow; RE2 would bring abseil.

- **Bounded everywhere.** Every match context sets `pcre2_set_match_limit` and `pcre2_set_depth_limit` (`editor.text.regexMatchLimit`, `editor.text.regexDepthLimit`). A limit hit is not a hang: a grammar rule that hits it is disabled for that line and reported once as a Problems row naming the grammar and rule; a find or `:s` pattern that hits it stops and says so in the find bar / vim message line.
- **Contiguous input.** PCRE2 needs a contiguous subject, a rope is not. Single-line patterns (the grammar tokenizer always; find/vim when the pattern cannot match `\n`) run line by line, copying each line into a reused scratch buffer -- cheap, and lines past `longLineMaxBytes` are matched in windows. Patterns that can span lines (contain `\n`, `\s` across lines in vim `\_` forms, or `(?s)`) run on the worker against a materialized copy of the snapshot, refused above `editor.text.multilineSearchMaxMB` (default 64) with a message.

### 4.3 Tokenizer

- **TextMate matching:** at the current position, among the active rule list, the **earliest** match wins; ties go to the first rule listed. Each rule's next match on the current line is cached, so a line is scanned about once rather than once per rule.
- **State:** the stack of open regions at a line boundary, interned to a small `StateId` (equal stacks share an id; equality is an integer compare).
- **Per-line cache:** `{startState, endState, runs[]}`, where a run is `{byteStart, scopeId}` (run-length, never per glyph -- ImGuiColorTextEdit's ~12 bytes/char model is what rules it out for large logs). Scopes are interned dotted strings.
- **A per-line time budget** (`editor.text.tokenizeLineBudgetUs`) yields a partial line that resumes later, so a pathological line never stalls the UI; long lines are additionally capped (s3.5).

### 4.4 Scheduling and cache ownership

A line's start state depends on every line before it, so "tokenize on a cache miss" must never mean "tokenize from line 0 on the UI thread".

- **Cache entries are versioned.** Each entry is `{bufferVersion, startState, endState, runs[]}`. The buffer bumps `version` on every edit and shifts/invalidates entries from the edited row on (the edit log maps rows forward).
- **The worker owns tokenizing.** It runs on a `RopeSnapshot` at version *v* and produces entries tagged *v*. The UI thread **applies a worker result only if its tag equals the current buffer version**; a stale result (the user typed meanwhile) is discarded and the worker restarts from the earliest invalid row on a fresh snapshot. Only the UI thread writes the shared cache; the worker hands results over through a queue. No locks on the cache itself.
- **Bounded UI-thread catch-up.** On a visible-line miss, the UI thread may tokenize synchronously only if the nearest valid cached line above is within `editor.text.syncCatchUpLines` (default 200) -- typing and nearby scrolling stay instant. Farther misses (Ctrl+End in a 30 MB file, a jump to a mark, a Goto) **draw plain text** (or line-local tokens, below) for those lines and raise the worker's priority to that region; colour appears when the worker gets there. There is no heuristic mid-file resync for stateful grammars -- correctness over guessing.
- **The worker works outward from the view:** from the earliest invalid row toward the visible end plus a margin, within `editor.text.highlightFrameBudgetMs` of wall time per frame.
- **Convergence stop:** after an edit, re-tokenizing starts at the edited line and **stops as soon as a recomputed end state equals the cached end state** for the next line. Typing inside a string usually re-tokenizes one line.
- **Line-local grammars** (`lineLocal: true`) skip all of the above: any visible line is tokenized on demand from the root state, synchronously, at any file size and any scroll position. This is what keeps a 400 MB Arcane log coloured.
- Stateful grammars above `syntaxMaxMB` are not tokenized; below it, only as far as the view has been.

### 4.5 Colours

`.arctheme` gains a `syntax` block mapping scope prefixes to a colour (literal or theme token) and style:

```json
"syntax": {
  "comment":            { "color": "editor.theme.textDim", "italic": true },
  "string":             { "color": "#ce9178ff" },
  "constant.numeric":   { "color": "#b5cea8ff" },
  "keyword":            { "color": "editor.theme.accent", "bold": false },
  "entity.name.key":    { "color": "#9cdcfeff" }
}
```

Resolution is longest dotted prefix (`string.quoted.double.json` -> `string.quoted` -> `string`), built once per theme into a flat `scopeId -> style` table. Dark and Light ship palettes derived from their existing tokens. A theme without a `syntax` block gets the built-in default.

### 4.6 Brackets and folding

- Bracket pairs come from the grammar; brackets inside `string.*` or `comment.*` scopes are skipped. The primary cursor's enclosing pair is highlighted (only when the selection is empty, as Zed does).
- Folding is indentation-based by default (a row is foldable if the next non-blank row is indented more; the closing-bracket row stays visible), plus `foldMarkers` from the grammar and manual folds (vim `zf`, a gutter action).

### 4.7 v1 languages

JSON and the Arcane JSON assets (`.arcmat .arcproj .arcscene .arctheme .arcsyntax .meta` ...), HLSL, Lua (covers premake), Markdown (with embedded fenced code), INI/cfg, TOML, YAML, XML, **Arcane log** (timestamps, `[info]`/`[warn]`/`[error]` level colours matching the Console), batch, PowerShell, shell, Plain Text (fallback).

Lua and Markdown start from lite-xl's `language_*.lua` content, translated into `.arcsyntax` with the MIT notice in the file header and `ThirdParty/NOTICES` (lite-xl ships no JSON/HLSL/INI/TOML/YAML grammars; those are written fresh). No GPL Zed query file is used.

---

## 5. View and document

### 5.1 `TextEditorView`

- **Drawing:** a custom ImGui widget with a fixed line height, drawing coloured runs into the window draw list for the **visible rows only** (clipper row math). Font: JetBrains Mono (already shipped); `editor.text.font`, `editor.text.fontSize`; Ctrl+mouse wheel zooms per document.
- **Gutter:** line numbers (absolute / relative / hybrid -- `editor.text.lineNumbers`), fold arrows, bookmark and Problems markers.
- **Text area:** current-line highlight, per-row selection rectangles, multiple carets (`editor.text.caretBlink`), bracket-pair boxes, search-match backgrounds (active match distinct), indent guides, optional whitespace rendering, column rulers (`editor.text.rulers`).
- **Scrollbar annotations:** search matches, other cursors, Problems.
- **Long lines:** horizontal scroll; each visible line's glyph x-prefix is cached so column <-> x is O(log n), not O(line length) (a known lite-xl weak spot).
- **Unicode:** the cursor moves by grapheme cluster (a UAX #29 subset: combining marks, ZWJ emoji sequences, regional-indicator pairs), so accents and emoji are never split.
- **IME:** composition position reported through ImGui's platform IME data at the primary caret; the composition string is drawn inline (underlined) until committed.
- **Mouse:** click, drag, double-click word, triple-click line, Ctrl+click add/remove cursor, Alt+drag column selection, drag-select auto-scroll.
- **Split view (D8):** a second `TextEditorView` over the same `TextBuffer`, with its own scroll, `DisplayMap` (so folds are per view) and `SelectionSet`, docked beside the first. Edits in one view shift the other view's anchors through the buffer's edit notifications.

### 5.2 Built-in controls

- **Find bar** (Ctrl+F; Ctrl+H for replace), docked at the top of the document: regex, case, whole word, in-selection toggles; "3 of 17" count; Enter/Shift+Enter next/previous; **Alt+Enter turns every match into a cursor**; Replace All is one undo step; large files are searched on the worker against a snapshot.
- **Go to line** (Ctrl+G): a small popup taking `line` or `line:col`. Spec 2's Goto Anything `:` supersedes it.
- **Status strip** (document footer): Ln/Col, selection count, encoding, line endings, indentation ("Spaces: 4") -- each clickable to change -- language (opens the picker), vim mode + pending keys + "recording @q", Follow toggle for growing files, read-only badge.

### 5.3 `TextDocument`

- An `EditorDocument` (and `InspectorSource`). Identity: the asset GUID when the file is a registered asset; otherwise the **normalized absolute path**. `DocumentHost` gains a path-keyed lookup for GUID-less documents so a second open focuses the existing tab.
- **Registration:** `DocumentHost` gets a **fallback factory** used when no extension factory claims a path and the file sniffs as text; C++ extensions stay routed to `IdeLaunch`. Text extensions with no dedicated editor (`.md .ini .log .hlsl .lua .toml .yaml .xml .txt ...`) register this factory explicitly.
- **Reading:** files are opened with `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`, so a running game (or the editor's own logger) can keep writing, rotating or deleting the log being viewed.
- **Save** preserves encoding, BOM and per-line endings (s3.5). `editor.text.ensureFinalNewline` and `editor.text.trimTrailingWhitespace`, both off by default.
  - Atomic on Windows: write a temp file beside the target, flush, then **`ReplaceFileW`** (not a plain rename), which keeps the original's ACLs, attributes, alternate data streams and file identity for other watchers. A file that does not exist yet is created with a plain rename.
  - **Sharing violation** (the file is open without delete/write sharing, e.g. a log held by a running process): the save fails cleanly with a banner -- *"<file> is in use by another program"* -- offering **Retry** and **Save As...**; the buffer stays dirty and nothing is lost. Read-only attribute or ACL refusals get the same banner with the reason.
  - **Self-save suppression:** after a save the document records the written size, mtime and a content hash; watcher events matching them are ignored, so saving never triggers a "changed on disk" reload of itself.
- **External changes** (file watcher, debounced):
  - clean document -> silent reload keeping cursors and scroll (positions mapped by line/col);
  - dirty document -> banner: **Reload** / **Keep mine**;
  - append-only growth -> append (s3.5); truncation or rotation -> reload under the two rules above;
  - deleted -> banner; Save recreates the file.
- **Crash safety:** unsaved text documents join the editor's existing autosave/recovery path (crash-window arc): buffers are snapshotted to the recovery store and offered back after a crash.
- **Session restore:** cursor, scroll, folds and split state per file, stored in user data (`editor.text.rememberViewState`).
- **Inspector page** (`TextDocumentInspectorPage`): path, size, encoding, language, line endings, indentation, read-only reason -- editable where meaningful (language, encoding, line endings, indentation). This keeps Arcane's one-Inspector model; the document draws no properties block of its own.

### 5.4 Open as Text on assets with a dedicated editor (D10)

- Asset context menu and Inspector gain **Open as Text** for any text-backed asset.
- If the asset has a dedicated editor, the text document opens **read-only** with a banner and an **Enable editing** button.
- After unlocking, Save writes the file and triggers the registry's reload of that asset. An open dedicated editor for the same asset then:
  - **has unsaved changes** -> shows its own conflict banner instead of silently losing either side;
  - **is clean** -> reloads, and **its undo history for that asset is cleared**, with a visible note in its toolbar ("History cleared: the file was changed outside this editor"). Commands in a document-scoped stack may hold pointers into the old in-memory objects and must not survive the reload. Entries in the editor's global `CommandStack` that target that asset are pruned the same way (the Inspector arc's prune-on-invalidate rule).
- The same rule applies to any external change that reloads an asset under a clean dedicated editor, not only Open as Text; the plan audits which dedicated editors already do this.

---

## 6. Vim layer

### 6.1 Activation and hooks

- `editor.text.vim.enabled` (Preferences, live). Off: no vim code runs.
- On: a `VimState` attaches to each `TextEditorView` and uses a small hook surface exposed by the view/core:
  - `SetCursorShape(block | bar | underline)`
  - `SetLineMode(bool)` (linewise selections)
  - `SetInputEnabled(bool)` (whether typed characters insert)
  - `SetClipAtLineEnds(bool)` (normal-mode cursor never past the last char)
  - events: `InputHandled`, `TransactionBegun`, `TransactionUndone`, `SelectionsChanged`
- **The cursor model, in one place.** The editor's cursors sit *between* characters; vim's sit *on* a character. `VimCursorModel` is the only code that converts, and every vim feature goes through it:
  - Normal/Visual: a vim cursor on character *c* at offset *o* is the editor caret at *o* with `SetClipAtLineEnds(true)` keeping *o* < line end (an empty line is the one exception: *o* = line start).
  - Inclusive motions (`e`, `$`, `f`, `t`, `%`) extend the editor range to *o* + length(*c*) before the operator runs; exclusive motions do not; linewise motions expand to whole lines including the line break.
  - Visual mode's selection always includes the character under the vim cursor: editor range = [min, max + length(char at max)).
  - `p` inserts after the vim cursor (editor offset *o* + length(*c*)), `P` at *o*; linewise registers insert below / above the line.
  - Entering Insert with `i` keeps *o*, with `a` moves to *o* + length(*c*); leaving Insert moves back one character (unless at line start), as vim does.
  These rules are tested directly (s11.3) because they are where vim layers historically break.
- The editor knows nothing else about vim. (This is the shape of Zed's `vim` crate -- an addon over the editor -- reimplemented clean-room; Zed's vim code and `vim.json` are GPL and are not copied. Vim's key behaviour itself is not copyrightable.)

### 6.2 Modes and grammar

- Modes: Normal, Insert, Replace, Visual, VisualLine, VisualBlock, operator-pending, command line (`:` `/` `?`).
- Grammar: `[count]["reg][count]operator[count]{motion | textobject}` or `[count]action`. Effective count = pre x post.
- **Every operator = expand each selection by the motion/object, then run a normal `TextCommands` command** (delete, change, yank, indent ...). This is why vim and multi-cursor compose for free. `MotionKind` is linewise / exclusive / inclusive, per vim.
- VisualBlock is represented as one selection per line in the ordinary `SelectionSet`.
- In normal mode with "Vim handles Ctrl keys" on, `gb` adds the next match (Sublime's Ctrl+D) so multi-cursor stays reachable.

### 6.3 Coverage (v1)

| Area | Coverage |
|---|---|
| Motions | `h j k l w W b B e E ge gE 0 ^ $ g_ gg G \| f F t T ; , % ( ) { } H M L`, `/ ? n N * #`, `` ` `` / `'` marks, `Ctrl+D Ctrl+U Ctrl+F Ctrl+B Ctrl+E Ctrl+Y zz zt zb` |
| Operators | `d c y > < = g~ gu gU J gJ zf`, `gc` (comment toggle from the grammar's `comment`), `Ctrl+A` / `Ctrl+X` |
| Text objects | `iw aw iW aW is as ip ap`, `i" a" i' a' i\` a\``, `i( a( ib i{ a{ iB i[ a[ i< a<`, `it at` (XML) |
| Registers | `"` unnamed, `0-9` (deletes shift), `a-z` (`A-Z` append), `-` `_` `+` `*` `/` `:` `.` `%`. Multi-cursor yank stores one entry per cursor; paste with an equal cursor count distributes. `editor.text.vim.useSystemClipboard` makes `"` alias `+`. |
| Repeat | `.` replays the last change (recorded commands + inserted text). `q{reg}` / `@{reg}` / `@@`: macros are **key text stored in the register**, so `"ap` pastes a macro for editing, as in vim. |
| Marks / jumps | `a-z` per file; `A-Z` global (opening the file through `DocumentHost`); special marks `` ` ' . ^ [ ] < > ``; jump list `Ctrl+O` / `Ctrl+I`; change list `g;` / `g,` |
| Undo | `u`, `Ctrl+R`; one insert session = one undo step |
| Folds | `zc zo za zR zM zf zd zj zk` over `FoldModel` |
| Insert mode | `Ctrl+W Ctrl+U Ctrl+R{reg} Ctrl+O Ctrl+T Ctrl+D`, `Esc` / `Ctrl+[` (per-mode chord table, s6.6) |
| Ex | ranges `% . $ N 'a /pat/ ?pat? +n -n '<,'>`; `:w :q :wq :x :q! :e :sav :r`, `:s` with `g i c` (`c` prompts per match) and `&`, `:g` / `:v`, `:normal`, `:d :y :m :t :co :> :< :j :sort` (with `n u i r` options), `:noh`, `:set`, `:map` family (`nmap vmap imap omap` + `noremap` forms), `:sp :vs :bn :bp :ls`, `:marks :reg :jumps` (popup listings) |

### 6.4 Options, remaps, persistence

- **Flavour (D13): Neovim defaults.** `Y` = `y$`, `hlsearch` and `incsearch` on, `startofline` off, `&` = `:&&`, `nrformats` = `bin,hex` (no octal), `scrolloff` 0, `wrapscan` on. Classic-Vim behaviour is one option away for each.
- Options are Arcane settings under `editor.text.vim.*`: `ignorecase`, `smartcase`, `hlsearch`, `incsearch`, `wrapscan`, `gdefault`, `timeoutlen`, `scrolloff`, `startofline`, `nrformats`, `yankToEol` (the `Y` choice), `leader`, `handleCtrlKeys` (default on while vim is on), `useSystemClipboard`.
- Tab width, spaces/tabs, shift width and relative numbers are **editor-wide** text settings (`editor.text.*`), not vim-only.
- `:set` changes a value for the **session only**; the Preferences page changes it permanently.
- Remaps: a table on a **Vim** Preferences page (`editor.text.vim.remaps`: `{mode, from, to, recursive}`); `:map` adds a session-only remap. Remaps resolve through a key trie with `timeoutlen` for ambiguous prefixes.
- Marks (`A-Z` and per-file), registers, and search/command history persist in user data across sessions (`editor.text.vim.persistState`).

### 6.5 Vim patterns -> PCRE2

Vim's regex dialect differs from PCRE2 (`\<` `\>`, `\(` `\)`, `\|`, `\{n,m}`, magic levels, `\zs`/`\ze`, `~`). `VimPattern::Translate(vimPattern, options) -> PCRE2 pattern | error` is a pure function used by `/`, `?`, `*`, `#`, `:s`, `:g`, `:v` and `:sort /pat/`.

| Vim | PCRE2 |
|---|---|
| magic (default): `\(` `\)` `\|` `\{n,m}` `\+` `\?` `\=` | `(` `)` `|` `{n,m}` `+` `?` `?` |
| `\v` very magic, `\m` magic, `\M` nomagic, `\V` very nomagic | switch the translation table from that point on |
| `\<` `\>` | `\b(?=\w)` / `\b(?<=\w)` (word = the grammar's `wordChars`, `iskeyword`) |
| `\zs` `\ze` | `\K` / a lookahead wrapping the remainder |
| `\c` `\C` anywhere | `(?i)` / `(?-i)` for the whole pattern, overriding `ignorecase`/`smartcase` |
| `~` (last substitute string) | the escaped literal of the previous `:s` replacement |
| `\_s` `\_.` `\n` | multi-line classes -> the multi-line search path (s4.2) |
| `\%^` `\%$` `\%V` `\%23l` | `\A`, `\z`, and position predicates checked after matching |
| `:s` replacement `\0`-`\9` `&` `\u` `\U` `\l` `\L` `\e` `\r` | expanded by the substitute engine, not by PCRE2 |

Anything outside the table is a **documented divergence** (listed in the vim help page and in the oracle's divergence notes, s11.3), reported as "E: unsupported pattern item `\%[...]`" rather than silently misbehaving.

### 6.6 Per-mode chord table (vim on, `handleCtrlKeys` on)

Rule of thumb: **Normal and Visual follow vim; Insert keeps the Windows editing chords** (it is the typing mode, and that is where a stray vim chord surprises Windows users most), plus vim's insert-mode deletion/register chords. With `handleCtrlKeys` off, every Ctrl chord in every mode falls through to the action table (s7) and vim keeps only plain keys, `Esc` and `Ctrl+[`.

| Chord | Normal | Visual | Insert | Vim off |
|---|---|---|---|---|
| `Esc`, `Ctrl+[` | cancel pending | -> Normal | -> Normal | `Esc` = ui.cancel; `Ctrl+[` = outdent |
| `Ctrl+Z` / `Ctrl+Y` | undo / scroll up 1 | undo / scroll up 1 | undo / redo | undo / redo |
| `Ctrl+R` | redo | redo | insert register `{reg}` | (unbound) |
| `Ctrl+C` | cancel pending | copy, -> Normal | copy | copy |
| `Ctrl+X` / `Ctrl+A` | decrement / increment number | decrement / increment | cut / select all (-> Visual) | cut / select all |
| `Ctrl+V` | Visual Block | toggle Visual Block | **paste** | paste |
| `Ctrl+W` | window prefix (`Ctrl+W v/s/h/j/k/l/c`) over split views | window prefix | delete word back | **close document** |
| `Ctrl+D` / `Ctrl+U` | half page down / up | half page down / up | outdent line / delete to line start | add next match / (unbound) |
| `Ctrl+F` / `Ctrl+B` | page down / up | page down / up | find / (unbound) | find / (unbound) |
| `Ctrl+E` | scroll down 1 | scroll down 1 | (unbound) | (unbound) |
| `Ctrl+O` / `Ctrl+I` | jump back / forward | -- | one Normal command | open scene / (unbound) |
| `Ctrl+T` | -- | -- | indent line | (unbound) |
| `Ctrl+S` | save | save | save | save |
| `Ctrl+H` | `h` | `h` | backspace | replace |

- With vim on, **`Ctrl+W` never closes a document** (it is delete-word in Insert and the window prefix elsewhere). Closing is `:q`, the tab's close button, middle-click, or the new `document.closeAlt` action (`Ctrl+F4`), which works in every mode.
- `Ctrl+[` is `Esc` whenever vim is on; outdent then lives on `Shift+Tab` / `<<` only.
- Multi-cursor stays reachable in Normal mode through `gb` (add next match), since `Ctrl+D` is half-page.
- This table is data (`VimKeymap` defaults); remaps (s6.4) can change any row, and the Shortcuts page shows the vim-mode meaning beside each action that vim shadows.

---

## 7. Shortcuts: unified with the action table (D6)

The editor already has one action table (`Input/EditorActionTable.hpp`) with contexts ranked by specificity (Global < Document < panels < Text) and `PressedInWindow` for a focused window answering a Global action itself.

1. **Shared verbs reuse existing action ids.** `edit.undo`, `edit.redo`, `edit.redoAlt`, `edit.cut`, `edit.copy`, `edit.paste`, `document.save`, `document.close` are answered by the focused text document via `PressedInWindow` (the Settings window's local-undo precedent). Rebinding Undo rebinds it everywhere. Each text document owns its history: Ctrl+Z in a text file never touches the scene's `CommandStack`.
2. **Text-only commands are new rows** in the same table under a new `ActionContext::TextDocument` (specificity above Document, alongside panels). Initial set (ids `text.*`): word left/right (+select), line start/end, document start/end, page up/down, add next match (Ctrl+D), select all matches (Alt+F3), split selection into lines (Ctrl+Shift+L), add cursor above/below (Ctrl+Alt+Up/Down), column select by keyboard, select line (Ctrl+L), expand selection to brackets, duplicate line (Ctrl+Shift+D), delete line (Ctrl+Shift+K), move line up/down (Alt+Up/Down), indent/outdent (Tab / Shift+Tab, Ctrl+] / Ctrl+[), toggle line comment (Ctrl+/), toggle block comment (Ctrl+Shift+/), join lines (Ctrl+J), find (Ctrl+F), replace (Ctrl+H), find next/previous (F3 / Shift+F3), go to line (Ctrl+G), fold / unfold (Ctrl+Shift+[ / ]), go to matching bracket (Ctrl+M), toggle bookmark (Ctrl+F2), next/previous bookmark (F2 / Shift+F2), zoom in/out/reset, toggle follow. They appear on the Shortcuts page, are rebindable, get conflict detection, and are listed by spec 2's palette for free.
3. **Ctrl+D** in a text document is add-next-match (Sublime); the Global `edit.duplicate` is shadowed there by context specificity, exactly as `graph.duplicate` shadows it in the graph today. Duplicate line is Ctrl+Shift+D.
4. **Vim keys are not chords** and live in vim's own sequence keymap (s6.4); vim's commands still call `TextCommands`, so `u` and Ctrl+Z share one history. Which Ctrl chords vim claims, per mode, is the s6.6 table; with `handleCtrlKeys` off, every Ctrl chord keeps its action-table meaning.
5. **New global row:** `document.closeAlt` (Ctrl+F4), so closing a document never depends on Ctrl+W, which vim repurposes.

---

## 8. Settings

All under `editor.text.*` (Preferences scope unless noted), registered through the settings arc's reflection (`ARC_REFLECT_TYPE_ATTR(Settings, ...)`) so they appear on the Preferences window, the sweep tests and the inventory:

`font`, `fontSize`, `tabSize`, `insertSpaces`, `detectIndentation`, `lineNumbers`, `rulers`, `renderWhitespace`, `indentGuides`, `caretBlink`, `highlightCurrentLine`, `undoGroupMs`, `undoMemoryMB`, `syntaxMaxMB`, `openMaxMB`, `longLineMaxBytes`, `multilineSearchMaxMB`, `maxCursors`, `tokenizeLineBudgetUs`, `highlightFrameBudgetMs`, `syncCatchUpLines`, `regexMatchLimit`, `regexDepthLimit`, `ensureFinalNewline`, `trimTrailingWhitespace`, `languageByExtension`, `rememberViewState`, `autoClosePairs`, `autoIndent`, and the `vim.*` group from s6.4. Budgets and limits are Dev-flagged (advanced) rows; their defaults are set from the s11.5 measurements, not guessed. Project scope may override indentation and line-ending defaults (`PreferencesProject`).

---

## 9. Hooks for spec 2 and the rest of Arcane

- `DocumentHost::OpenAt(path, Point)` (and `OpenAt(guid, Point)`): open or focus, move the primary cursor, centre the line. Console/Problems `file:line` links use it for non-C++ files; C++ still goes to `IdeLaunch`.
- `ISymbolProvider` on documents: `Symbols() -> vector<{name, kind, range}>`. `TextDocument` implements it from the grammar's `symbols` rules (Markdown headings, JSON keys, HLSL/Lua functions). Spec 2's Goto Anything `@` queries the active document through it; other documents (shader graph, input actions) may implement it later.
- `TextCommands` are registered as actions (s7), so the palette lists them without extra wiring.
- `TextSearch` exposes a pure `Search(snapshot, query) -> matches` used by the find bar and reusable by spec 2's Find in Files.

---

## 10. Third-party and licensing

| Source | Licence | Use |
|---|---|---|
| Zed `sum_tree` | Apache-2.0 | Informs the concrete `Rope` (design + attribution in NOTICES; no generic machinery ported) |
| Zed `rope`, `text`, `editor`, `vim`, `language`, grammars, themes | GPL-3.0+ | **Design reference only.** No code, query or keymap file copied or translated. |
| lite-xl | MIT | Lua + Markdown grammar content translated to `.arcsyntax`, notice kept |
| PCRE2 | BSD | Vendored `ThirdParty/pcre2`, static, JIT |
| rapidcheck | (already vendored) | Property tests |
| Neovim | Apache-2.0 / Vim licence | Dev-time oracle only (s11.3), never linked or shipped |
| Zep, ImGuiColorTextEdit, Lexilla, Sublime | MIT / permissive | Design reference only; nothing vendored |

---

## 11. Testing

### 11.1 Core
- **Property tests** (rapidcheck): random edit/undo/redo sequences against a `std::string` model -- text equality, rope invariants (leaf sizes, summaries equal recomputed), undo-to-start equals original, redo-to-end equals final, dirty flag correctness.
- **Selections:** merge rules, anchor bias on edits at boundaries; `maxCursors` confirmation.
- **`TextCommands` tables:** every command with one and several cursors: `{start text with cursor markers, command, expected text with cursor markers}`.
- **Byte-identical round trips:** every supported encoding, BOM and line-ending style -- including mixed files and lone `\r` -- loads and saves byte-identical when unedited; a one-character edit to a mixed file changes exactly one line's bytes (`EolMap` property test under random edits).
- **DisplayMap:** buffer row <-> display row under random fold/unfold/edit sequences equals a brute-force model.
- **Leaf-size benchmark** (step 1, s3.1): the chosen constant and its measurements are recorded in the step's report.

### 11.2 Syntax
- **Grammar scope tests:** each language ships sample files with comment-annotated expectations (`// ^^^^ constant.numeric.json`), run by ArcaneTests. A grammar cannot regress silently.
- **Incremental == full:** property test that after random edits, the incremental highlight cache equals a full re-tokenize.
- **Version discipline:** a worker result tagged with an older buffer version is never applied (deterministic test with a held worker + interleaved edits); a far jump draws plain lines and fills in when the worker arrives; UI-thread catch-up never exceeds `syncCatchUpLines`.
- **Line-local grammars:** a visible line deep in a file above `syntaxMaxMB` is coloured; a line-local grammar declaring a multi-line region is rejected on load.
- **Long lines:** a 40 MB single-line JSON opens, draws, and the following line highlights from the root state.
- **Regex limits:** a catastrophic pattern (grammar rule and find query) hits the match limit, is reported, and the worker and UI keep running.
- Grammar validation: malformed grammars produce a Problems row and fall back to Plain Text.

### 11.3 Vim
- Table-driven cases `{start text + cursor, keys, expected text + cursor, expected mode/register state}`, hundreds of them, pure C++.
- **Cursor-model tests** for every rule in s6.1 (inclusive motions, visual's extra character, `$`, `p`/`P`, `i`/`a`, leaving Insert, empty lines).
- **Pattern translator tests:** every row of the s6.5 table, plus the divergence list producing its error.
- **Chord table tests:** each row of s6.6 in each mode, and with `handleCtrlKeys` off.
- **Neovim oracle (D9, D13):** `scripts/vim-oracle.ps1` runs each case through `nvim --headless --clean`. **Neovim's own defaults are the target**, so the script sets no compatibility options; it records the Neovim version it ran. It writes or verifies the expected columns. Dev-time only; tests never require Neovim. Cases where Arcane deliberately differs carry an explicit `divergence` note.

### 11.4 View and document
- **Goldens:** the headless editor opens sample files (JSON, HLSL, Markdown, log; vim normal/visual; find bar open; split view) and screenshots them -- re-blessed through the established golden procedure.
- **Witness scenarios:**
  - open a log another process holds open for writing (no delete sharing), follow it while it grows, survive a rotation and a truncation;
  - save into a file locked by another process -> the in-use banner, buffer still dirty, Retry succeeds after release;
  - a save does not trigger a self-reload; `ReplaceFileW` keeps a read-only file's attributes and ACLs;
  - external change with and without unsaved edits;
  - Open as Text read-only -> unlock -> save -> asset reload, with the dedicated editor clean (history cleared, note shown) and dirty (conflict banner);
  - crash recovery restores an unsaved buffer.

### 11.5 Performance budgets (`[perf]`, baselined)
- Open a 100 MB log (line-local grammar): time to first coloured frame.
- Ctrl+End in a 30 MB stateful-grammar file: frame time stays within budget (plain lines first, colour later).
- Keystroke: edit + retokenize + layout under 1 ms on a 10k-line JSON file.
- Scroll: frame cost at 60 lines/frame on a large file.
- Highlight-on-open of a 5 MB file to the visible range.

Baselines live with the existing automation baselines; regressions fail the suite.

---

## 12. Delivery

### 12.1 Order (each step ends green; the plan breaks them into tasks)

1. **Core:** leaf-size benchmark, `Rope`, `TextBuffer` + undo, `EolMap`, `SelectionSet`, `TextCommands`, `DisplayMap` (folds), encodings. No UI; fully tested.
2. **First visible editor:** `TextDocument`, basic `TextEditorView` (draw, edit, multi-cursor, mouse, save incl. `ReplaceFileW` + sharing rules, external change), fallback registration in `DocumentHost`, shared-verb actions + `document.closeAlt`. Usable from here.
3. **Syntax:** PCRE2 vendored (with limits); grammar engine with versioned cache, line-local grammars, long-line cap; v1 grammars; `.arctheme` `syntax` block; grammar tests.
4. **Editing features:** find/replace, go to line, folding, brackets, split view, status strip, Inspector page, text-only actions in the table.
5. **Vim** (behind **Experimental**, D12), four passes: (a) cursor model, modes, motions, operators, text objects, chord table; (b) registers, dot-repeat, macros, marks; (c) ex commands + the pattern translator; (d) options, remaps, persistence, Vim Preferences page. Oracle tables grow with each pass.
6. **Polish:** log follow, crash recovery, session restore, Open as Text (with the undo-history rule), perf budgets, goldens, witness scenarios.

### 12.2 Release gate (D12)

- **The text editor is released** when steps 1-4 and the step-6 essentials -- log follow, crash recovery, Open as Text, the perf budgets, goldens and witness scenarios -- are green, through a gate task like the settings arc's. Session restore may trail.
- **Vim ships behind an "Experimental" label** (a badge on the Vim Preferences page and in the status strip while vim is on). Each pass widens what Experimental covers; the label drops when pass (d) and the full oracle suite are green, through a second, vim-only gate.
- Spec 2 (palette, Goto Anything, Find in Files) can start after the first gate; it does not wait for vim.

### 12.3 After v1 (not in this spec)
Minimap; soft wrap (likely first, for Markdown and logs -- a second `DisplayMap` layer); word completion; diff view; tree-sitter for structural folding/outline/syntax-aware text objects behind the scope interface; shader editor snippet fields and crash log tail adopting the core; CJK fallback font; digraphs; opt-in confirmed `:!`.

---

## 13. Revision 2 (2026-10-07): independent review folded in

An independent review of revision 1 found real gaps; all were accepted. What changed and where:

| Finding | Resolution |
|---|---|
| Jump-to-end would tokenize a whole file synchronously; cache ownership between UI thread and worker undefined | Versioned cache entries, worker-owned tokenizing, stale results discarded, bounded UI catch-up, plain-until-ready (s4.4) |
| Highlighting switched off for large logs; no long-line policy | `lineLocal` grammars highlight at any size; `longLineMaxBytes` cap with next-line resync (s3.5, s4.1, s4.4) |
| Mixed line endings normalised on save (whole-file VCS diffs) | Per-line endings preserved via `EolMap`; byte-identical unedited saves; explicit convert (s3.5) |
| Vim regex vs PCRE2 flavour clash; contiguous input; catastrophic backtracking | `VimPattern::Translate` (s6.5); line-by-line scratch + materialized multi-line path; match/depth limits everywhere (s4.2) |
| Neovim oracle vs Vim semantics undefined | D13: Neovim defaults are the target (s6.4, s11.3) |
| Ctrl+[ / Ctrl+W / Ctrl+V collisions | Per-mode chord table (s6.6); `document.closeAlt` (s7) |
| Windows save/watch details | `ReplaceFileW`, sharing modes, sharing-violation banner, self-save suppression, cheap append detection, rotation/truncation (s5.3, s3.5) |
| Open as Text could leave a dedicated editor's undo history pointing at stale objects | History cleared on reload under a clean editor; global stack pruned (s5.4) |
| Folds, wrap and vim rows need one mapping | `DisplayMap` in v1, folds only (s3.6) |
| Leaf size and generic port assumed | Benchmark-chosen leaf size; concrete rope informed by `sum_tree` (s3.1, s10) |
| Search matches as anchors would scale badly | Matches recomputed per snapshot; `maxCursors` confirmation (s3.2) |
| Vim cursor-on vs caret-between mismatch | `VimCursorModel` rules in one place, tested (s6.1, s11.3) |
| ImGui text-rendering limits unstated | Added to non-goals (s1.2) |
| `when.followedBy` one-off extension | Removed; lookahead `match` rule ordered before the string region (s4.1) |
| v1 too big for one release | D12: release gate after step 4 + step-6 essentials; vim Experimental (s12.2) |

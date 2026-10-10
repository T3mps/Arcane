# Arcane Text Editor -- design

**Status:** Proposed (brainstorm 2026-10-07; sections 1-6 approved in conversation; revisions 2-4 fold in three independent reviews -- see s13-s15; revision 5 adds user-chosen openers (D15, s5.6); revision 6 folds in the implementation plan's reconciliation (s17) -- the plan is `docs/superpowers/plans/2026-10-07-text-editor.md`; revision 7 folds in an external review of that plan (s18); awaiting written-spec review)
**Scope:** spec 1 of 2. Spec 2 (editor-wide Command Palette, Goto Anything, Find in Files) follows this one and consumes the hooks defined in s9.
**Research:** lite-xl (`D:\dev\_reference\lite-xl-master`, MIT), Zed (`D:\dev\_reference\zed-main`, GPL editor crates / Apache `sum_tree`), Zep, ImGuiColorTextEdit (BalazsJako + santaclose + goossens forks), Sublime Text's `.sublime-syntax` model, tree-sitter, Lexilla. Findings are summarised where they drive a decision.

---

## 1. Goal

A Sublime-Text-4-class text editor that lives inside the Arcane Editor as an ordinary document: its own tab that docks, undocks and drags like every other Arcane document.

- **It edits any text, and the user chooses who opens what (D15).** Arcane never forces tooling: every file type opens with the opener the user picks -- the Arcane Text Editor, Visual Studio, the system default, or any other program. Defaults are sensible, not mandatory: C/C++ opens in Visual Studio (its debugger integration is why), everything else that is text opens in the Arcane Text Editor. Arcane does not try to replace an IDE's language services for C++ (no IntelliSense, debugging or compiler diagnostics in-editor), but editing C++ in Arcane is a supported choice.
- **It is the fallback viewer.** Any text file with no dedicated Arcane document editor opens here, instead of "no editor registered".
- **It is fast on large files** (logs, diagnostics) and correct on every file it saves.
- **Power-user features in v1:** Sublime's multi-cursor model and an opt-in, near-full vim emulation.

### 1.1 Decisions (user, 2026-10-07)

| # | Decision |
|---|---|
| D1 | Full document tab for text, fallback viewer for anything without a dedicated editor. Bar: Sublime Text 4. (Amended by D15: C++ is no longer excluded; who opens what is the user's choice. Any earlier "never opens C/C++" constraint, including the plan skeleton's, is superseded: `FileOpener` decides, and C/C++ default to Visual Studio.) |
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
| D14 | **Read-only files are treated as VCS-locked until proven otherwise.** The save banner names the likely VCS (git/Git LFS lockable, Perforce, Lore, Unity VCS; Lore never sets files read-only and Unity VCS does only with its opt-in setting, s5.3) and puts "Remove read-only and save" behind a confirmation. Source-control integration is a separate future spec: git first, Lore eventually. |
| D15 | **User-chosen openers, never forced tooling** (user, 2026-10-07): per file type, the user picks the Arcane Text Editor, Visual Studio, the system default application, or a custom program. Defaults: C/C++ -> Visual Studio; other text -> Arcane Text Editor; assets with a dedicated Arcane editor -> that editor. A one-off **Open With** menu exists everywhere a file can be opened. Every open route honours the choice (s5.6). Project settings may suggest only built-in openers; programs are named only in the user's own Preferences, and the settings registry refuses a program from every rung inside a project (`CVarFlags::LaunchesProgram`, settings S7-SEC) (s5.6, rev 7). |

### 1.2 Non-goals

- C++ **language services**: IntelliSense/LSP, debugging, build-error squiggles from the compiler. (Editing C++ text in Arcane is supported by choice, D15; it gets syntax highlighting, not language services.)
- Vimscript, vim plugins, `:!` shell filters, `:terminal`, digraphs, spell check. (Digraphs and an opt-in `:!` that confirms before running are cheap later additions if wanted.)
- Loading other editors' grammar/theme/config files (D5).
- A hex/binary viewer. Binary files are refused with a message.
- Minimap, soft wrap, word completion, diff view: later steps, not v1 (s12.3).
- Collaborative editing. (Zed's CRDT machinery -- Lamport clocks, fragment/locator anchors, version vectors -- is deliberately not reproduced.)
- Source-control integration (status, lock, check-out, diff). Arcane has none today; v1 only *identifies* a likely VCS to word the read-only banner (D14, s5.3). Integration is its own future spec: git (GitHub-hosted projects) first, Epic's Lore later, Perforce / Unity VCS only if users ask.
- **Text-rendering limits (inherited from ImGui):** no complex-script shaping (Arabic, Devanagari render wrong), no bidi, no automatic font fallback (CJK needs a fallback font merged into the atlas; not shipped in v1), no colour emoji. Grapheme-correct cursor movement (s5.1) does not fix rendering; these are display limits, stated so nobody mistakes them for bugs.
- **Typing non-BMP characters** (emoji and other characters above U+FFFF). ImGui is built with a 16-bit `ImWchar`, so v1 drops such typed characters with a one-time warning (s5.1); opening, drawing and saving files that already contain them is unaffected. Switching to `IMGUI_USE_WCHAR32` is out of scope for v1 and an owed follow-up for the user to decide: it changes ImGui's font layout across the game-DLL boundary and may need an engine ABI bump.

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

- A balanced B+ tree of UTF-8 leaves. A **concrete rope written for Arcane, informed by Zed's `sum_tree`** (Apache-2.0; attribution kept in the repository's `NOTICE.md` as a courtesy). It is not a port of `sum_tree`'s generic `Summary`/`Dimension` machinery -- translating Rust generics is a rewrite either way, and one concrete summary is simpler. Zed's `rope`/`text` crates are GPL and are design reference only.
- **Leaf size is measured, not assumed.** Step 1 benchmarks leaf targets from 128 B to 1 KB on the s11.5 workloads (100 MB log open, random edits, row seeks, snapshot cost across threads) and fixes the constant from the results. Zed's 64-128 B suits Zed's file sizes; Arcane's large-log target may favour Ropey-style ~1 KB leaves (a 512 MB file at 1 KB is ~0.5 M leaves, not ~5 M). Node refcounts are atomic because snapshots cross threads, which is part of what the benchmark measures.
- Each node caches a summary (`TextSummary`): `{bytes, newlines, lastLineBytes, longestLineBytes, firstLineBytes}`. `firstLineBytes` is needed to combine `longestLineBytes` correctly when two summaries join mid-line. Byte offset <-> `Point{row, byteCol}` is O(log n) by seeking on one dimension while accumulating the other; no line-start table is rebuilt per edit.
- **Persistent:** an edit copies only the touched path. A `RopeSnapshot` is O(1) to take and immutable, so the highlighter worker and large-file search read a consistent snapshot while the user types. Every snapshot carries the buffer `version` it was taken at (s4.4).

### 3.2 Positions and anchors

- Internal positions are byte offsets, or `Point{row, byteCol}`. Display columns (tab width, wide glyphs) are computed only by the view.
- Anything that must survive edits -- selections, vim marks, folds, bookmarks -- is an **anchor**: `{offset, bias}`, shifted by every edit (left bias stays before an insertion at its offset; right bias moves after). Anchors live in an unsorted slot vector with stable ids per owner; shifting is O(anchors) per edit, which is fine because anchor counts stay small.
- **Search matches are never anchors.** A big log under `hlsearch` can hold hundreds of thousands of matches; shifting them per edit would be the dominant cost. Matches are recomputed per snapshot for the visible range (plus a background total count) and discarded on the next version; a background result (the count, a find) is instead mapped forward through the edit log to the current version when it lands (s5.2, rev 7) -- shifted once, never kept as anchors.
- Turning all matches into cursors (Alt+Enter, vim `gb` repeated) creates real selections; above `editor.text.maxCursors` (default 10 000) it asks first.

### 3.3 Edits, transactions, undo

- `TextBuffer::Edit(span<const EditOp>)` takes sorted, non-overlapping `{range, newText}` ops and applies them in one pass, producing `EditRecord{range, oldText, newText}`s. A batch's ops must have strictly increasing begin offsets and must not overlap; an invalid batch asserts in Debug and is refused (no change) in Release.
- Transactions nest (`Begin`/`End` depth; empty transactions are dropped). An undo entry is `{records[], selectionsBefore, selectionsAfter}` -- undo restores the cursors as well as the text.
- **Grouping:** consecutive typing transactions merge within `editor.text.undoGroupMs` (default 300) unless a boundary intervenes (cursor moved by a non-typing command, a save, an explicit break). Only transactions of the same kind merge (typing with typing, deleting with deleting). Vim groups a whole insert session explicitly (`GroupUntil`).
- History is per document, survives save, and is capped by memory (`editor.text.undoMemoryMB`, default 64), dropping oldest entries. `TextBuffer::ClearHistory()` drops it explicitly (used by Reopen with Encoding, s3.5).
- **Dirty:** `changeId != savedChangeId`. Undoing back to the saved state reads clean again; editing after undoing past the save point makes the saved state unreachable (dirty until saved).

### 3.4 Selections

- `Selection{id, anchor, head, goalColumn}`; `reversed` is derived from anchor > head. `goalColumn` keeps the visual column across up/down through short lines.
- `SelectionSet` keeps selections sorted and disjoint. Merge rule: overlapping, same start, or a cursor touching another selection's boundary. The **newest** selection is primary; the view follows it.
- A drag in progress is a separate `pending` selection merged on release.

### 3.5 Files, encodings, line endings

| Concern | Rule |
|---|---|
| Line endings | **Every line keeps its original ending.** The rope stores LF; an `EolMap` records the file's dominant style plus a sparse set of exception rows (CRLF in an LF file, LF in a CRLF file, lone `\r`), shifted by edits like anchors. A lone `\r` (classic Mac) is a line break. New lines take the dominant style. An unedited file saves back **byte-identical**; a one-character edit changes one line in version control. Converting is explicit: the status strip's line-ending menu (and a `text.convertLineEndings` action) rewrites every line in one undo step. Mixed files show a status-strip note but are never silently normalised. An **empty** LF row directly after a lone-CR row (a state only editing creates) is saved as `\r` + `\r\n`, because writing `\r\n` there would merge the two breaks into one and lose a line (rev 7, reconcile T3). |
| Encodings | UTF-8 (BOM preserved if present) and UTF-16 LE/BE with BOM are decoded and saved back in the same encoding. **Windows-1252** (common in old `.bat`/`.ini` files) is supported through **Reopen with Encoding** (status strip and Inspector): the file is decoded as 1252 and saved back as 1252. Reopen with Encoding replaces the buffer contents and **clears the undo history** (the old text was decoded under another encoding, so undoing into it would put raw bytes back under the new label). The chosen encoding is remembered per file in the view state. A UTF-16 file containing a lone surrogate opens **read-only** (reason `InvalidUtf16`), with the unpaired unit kept as WTF-8 bytes so nothing is lost. A UTF-16 file whose body has an **odd byte count** (a writer mid-unit) keeps its encoding and opens editable: the whole units are decoded and the stray last byte is **held** -- not in the text, not in the synced state -- so the next append that completes the unit brings it in; a save of the document does not write a held byte (rev 7, reconcile T3). |
| Invalid bytes / binary | Invalid UTF-8 or binary content (NUL bytes in the first 8 KB) opens **read-only** with a banner explaining why and offering **Reopen with Encoding -> Windows-1252** (which makes it editable when it is a legacy text file); saving can never corrupt the file. Pure binary formats with no text editor are refused with a message: a file with an explicit text extension that contains NUL bytes opens read-only; a file with an unknown extension and NUL bytes is refused. The NUL sniff applies to the UTF-8 path only; UTF-16 files are not sniffed. Defined end to end: <br>- the rope holds the file's **raw bytes** unchanged (never lossy-decoded, so nothing about the file is altered); <br>- each invalid byte **draws as a hex box** (`<E9>`), in the comment colour; <br>- grapheme iteration treats **each invalid byte as its own cluster**, so cursors, selection and vim motions step over them one at a time; <br>- PCRE2 is compiled with **`PCRE2_MATCH_INVALID_UTF`** everywhere, so find, vim search and grammars work on these files instead of returning an error; invalid bytes never match `.` or a class. |
| Size thresholds | Above `editor.text.syntaxMaxMB` (default 32), stateful grammars stop highlighting; **line-local grammars keep highlighting at any size** (s4.4), so logs never lose their level colours. Above `editor.text.openMaxMB` (default 512) the file is not opened ("open externally" notice). |
| Long lines | A line longer than `editor.text.longLineMaxBytes` (default 64 KB; e.g. one-line minified JSON) is tokenized only up to the cap; the rest of that line draws plain, and the **next** line restarts from the grammar's root state so nothing after it is held hostage. This is a **deliberate, named exception** to s4.4's no-heuristic-resync rule, and the tokenizer applies it identically in every mode (incremental, worker, full), so "incremental == full" (s11.2) still holds by construction. Drawing such a line clips to the visible horizontal window. |
| Append-only growth | Detected cheaply: the size grew **and** a hash of a small window (4 KB) just before the old end still matches. A shrink (truncation) or a window mismatch (rotation, rewrite) is a full reload under the s5.3 external-change rules; so is an old file ending in a lone `\r` whose appended tail starts with `\n` (the `\r\n` pair spans the boundary). An append is applied on the **first poll** that sees it (only the complete tail): a pending change that differs from the last synced state only by growth settles at once. Rewrites, truncations and deletions are debounced -- but never by waiting for two equal stamps, which a file written on every poll never produces (a game truncates its log at start, then writes every 100 ms): a rewrite, rotation or truncation settles when two consecutive polls agree on the bytes up to the old end's hash, or at worst after `editor.text.follow.maxDebounceMs` (rev 7); a deletion settles when two consecutive polls find the file missing. The document's synced state advances only when the buffer equals the bytes it records; the dirty banner keeps its own "acknowledged" stamp, so growth after a dismissed banner is still recognised as growth. **Appending happens only when the document is clean or read-only**; a dirty document whose file grows gets the s5.3 dirty banner instead. Appended tails are decoded as fragments: a NUL in an appended UTF-8 tail makes the document read-only (`Binary`) instead of appending; an odd-byte UTF-16 tail fragment is held back until it is complete; an appended UTF-16 tail with invalid UTF-16 (a lone surrogate) makes the document read-only (`InvalidUtf16`). An append: <br>- is **not an undo entry** and does not count toward `undoMemoryMB`; <br>- leaves `changeId` and `savedChangeId` both unchanged, so a clean document stays clean (the buffer still equals the file); existing undo history stays valid because the append only adds text after every recorded range; <br>- does bump the buffer `version` (s4.4), so caches and searches see it. <br>A **Follow** toggle (status strip) keeps the view at the end, like `tail -f`. |

### 3.6 Display map

One `DisplayMap` sits between the buffer and the view and owns the buffer-row <-> display-row mapping. In v1 it has a single layer, **folds**: a sorted list of folded row ranges, with display row = buffer row minus hidden rows above (prefix sums). Everything row-visual goes through it -- drawing, scrolling, vim `j`/`k` vs `gj`/`gk`, `H M L`, `scrolloff`, `zj`/`zk`. Soft wrap (the first post-v1 follow-up) becomes a second layer under the same interface instead of a retrofit through every caller. Tab expansion stays a draw-time computation per visible line.

---

## 4. Syntax highlighting

### 4.1 Grammar files: `.arcsyntax`

JSON, in the same style as `.arctheme`. Search path, later entries overriding earlier by `name`:
1. engine `data/Syntaxes/`
2. project `Config/Syntaxes/`
3. plugin-registered grammars: each active plugin root's `Syntaxes/` folder is loaded; `RegisterPluginGrammar` is the seam a future editor-plugin API will call (there is no plugin registration API today)

Saving a grammar reloads it live (and re-highlights open documents using it). Compiled grammars are **immutable, refcounted snapshots** with a `grammarVersion`: a reload publishes a new snapshot, the worker finishes or abandons work on the old one (it holds a reference, so compiled patterns never vanish under it), and cache entries carry the `grammarVersion` they were produced with, so entries from the old grammar are invalid on sight.

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
- `lineLocal: true`: a line's tokens never depend on earlier lines. The definition is **decidable by construction, not by proof**: in a line-local grammar every line is tokenized from the root state, and any `begin`/`end` region still open at the end of a line is **force-closed there** (its scope ends with the line). So `begin`/`end` stays usable for things like quoted strings inside a log line, and no validation of "can this region span lines" is needed. Line-local grammars -- Arcane log, INI/cfg, CSV-like logs, plain key/value files -- are tokenized on demand for visible lines only, at any file size (s4.4).

Context-dependent scopes use ordinary rules, not special syntax: the JSON key above is a single-line `match` with a lookahead, listed before the generic string region so it wins the tie at the same position.

Other format rules: an optional top-level `"notice"` string carries a grammar's licence/attribution text; `brackets` entries are single ASCII characters. **Known limits:** there are no `end`-to-`begin` back-references and no `include`/`repository`, so Lua long brackets are recognised only up to level 2, and heredocs and YAML block scalars are not recognised.

Detection order: the user's choice in the status-strip language picker, remembered per extension in `editor.text.languageByExtension` (checked first, so the picker can override an extension a grammar claims) -> file extension (`files`) -> `firstLine` regex -> Plain Text.

The format is validated on load; a bad grammar produces a Problems row naming the file and rule, and the language falls back to Plain Text.

### 4.2 Regex engine: PCRE2 + JIT

Vendored under `ThirdParty/pcre2` (BSD), built as a static lib via premake with JIT enabled, 8-bit code units. One engine for grammar rules, in-file find/replace, vim `/` `?` `:s` `:g` (vim syntax is translated first, s6.5). `std::regex` is too slow; RE2 would bring abseil.

- **Bounded everywhere.** Every match context sets `pcre2_set_match_limit` and `pcre2_set_depth_limit` (`editor.text.regexMatchLimit`, `editor.text.regexDepthLimit`). The depth limit protects the interpreter (non-JIT) path only; the JIT path is bounded by the match limit plus the JIT stack size. A limit hit is not a hang: a grammar rule that hits it is disabled for that line and reported once as a Problems row naming the grammar and rule; a find or `:s` pattern that hits it stops and says so in the find bar / vim message line.
- **Threading.** Compiled `pcre2_code` (inside a grammar snapshot or a compiled find query) is shared read-only across threads. `pcre2_match_data`, match contexts and JIT stacks are **per thread**: the UI thread and the worker each own their own set; nothing mutable from PCRE2 is shared.
- **Contiguous input.** PCRE2 needs a contiguous subject, a rope is not. Single-line patterns (the grammar tokenizer always; find/vim when the pattern cannot match `\n`) run line by line, copying each line into a reused scratch buffer -- cheap, and lines past `longLineMaxBytes` are matched in windows. Patterns that can span lines (contain `\n`, `\s` across lines in vim `\_` forms, or `(?s)`) run on the worker against a materialized copy of the snapshot, refused above `editor.text.multilineSearchMaxMB` (default 64) with a message.

### 4.3 Tokenizer

- **TextMate matching:** at the current position, among the active rule list, the **earliest** match wins; ties go to the first rule listed. Each rule's next match on the current line is cached, so a line is scanned about once rather than once per rule.
- **State:** the stack of open regions at a line boundary, interned to a small `StateId` (equal stacks share an id; equality is an integer compare).
- **Per-line cache:** `{startState, endState, runs[]}`, where a run is `{byteStart, scopeId}` (run-length, never per glyph -- ImGuiColorTextEdit's ~12 bytes/char model is what rules it out for large logs). Scopes are interned dotted strings.
- **A per-line time budget** (`editor.text.tokenizeLineBudgetUs`) yields a partial line that resumes later, so a pathological line never stalls the UI; long lines are additionally capped (s3.5). The resumable state carries everything a full run of that line would carry -- including the rules disabled by a regex-limit hit and the zero-width-match counter -- so a resumed line tokenizes exactly as an unbudgeted one and a pathological rule does not re-hit its limit on every resume (rev 7).

### 4.4 Scheduling and cache ownership

A line's start state depends on every line before it, so "tokenize on a cache miss" must never mean "tokenize from line 0 on the UI thread".

- **Cache entries are versioned.** Each entry is `{bufferVersion, grammarVersion, startState, endState, runs[]}`. The buffer bumps `version` on every edit (and every append) and records each edit in an edit log of `{version, firstRow, rowDelta}`; the cache shifts entries by `rowDelta` and invalidates from `firstRow` on. The edit log keeps the newest 4096 batches; a consumer older than that (`EditLogCovers(v)` is false) invalidates everything. Beside `grammarVersion` the cache keeps a **cache epoch**; a grammar reload or an edit-log-overflow invalidation bumps it, and entries from an older epoch are invalid on sight.
- **The worker owns tokenizing; its results are mapped forward, not thrown away.** It runs on a `RopeSnapshot` at version *v* and produces rows tagged *v*. When a result arrives at current version *c* > *v*, the UI thread consults the edit log for (*v*, *c*]: rows **above the earliest edited row** since *v* are still exact and are applied (shifted by nothing, since edits below them do not move them); rows **below** the edited rows are kept, shifted by the log's `rowDelta`, **if** re-tokenizing the edited rows converged (the end state after the edit equals the start state the worker assumed for the next row) -- the common case, since most edits do not open or close a region; otherwise they are discarded and the worker continues from there on a fresh snapshot. The edited rows themselves are always re-tokenized. Steady typing at the bottom of a file, or Follow appending every frame, therefore never starves the rows above -- the worker's progress always lands. Only the UI thread writes the shared cache; the worker hands results over through a queue. No locks on the cache itself.
- **Bounded UI-thread catch-up.** On a visible-line miss, the UI thread may tokenize synchronously only if the nearest valid cached line above is within `editor.text.syncCatchUpLines` (default 200) **and** only within `editor.text.syncCatchUpBudgetUs` of wall time per frame (dense lines can blow a line count); whatever does not fit draws plain this frame -- typing and nearby scrolling stay instant. Farther misses (Ctrl+End in a 30 MB file, a jump to a mark, a Goto) **draw plain text** (or line-local tokens, below) for those lines and raise the worker's priority to that region; colour appears when the worker gets there. There is no heuristic mid-file resync for stateful grammars -- correctness over guessing (the one named exception is the long-line cap, s3.5).
- **The worker works outward from the view:** from the earliest invalid row toward the visible end plus a margin, within `editor.text.highlightFrameBudgetMs` of wall time per frame.
- **Convergence stop:** after an edit, re-tokenizing starts at the edited line and **stops as soon as a recomputed end state equals the cached end state** for the next line. Typing inside a string usually re-tokenizes one line.
- **A partial row is not ready (rev 7).** A row cut short by the per-line budget is cached as *Partial*, never *Ready*, and the frontier of ready rows never moves past it. A completed result for that row is accepted at the frontier and replaces the Partial entry; the UI-thread catch-up and the worker's scheduling both resume a Partial row before anything else, the buffer's last row included. The free resolve pass (integer compares only, which runs outside the catch-up bounds) does not tokenize: it stops AT a Partial row and leaves it as the first unit of work for the catch-up or the worker (rev 7, reconcile S7). Otherwise the frontier stalls on the partial row, the worker busy-loops and nothing below it is ever coloured.
- **A change to the regex limits** (`regexMatchLimit`, `regexDepthLimit`) resets the cache, so no file is coloured by rows tokenized under two different limits (rev 7).
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

Resolution is longest dotted prefix (`string.quoted.double.json` -> `string.quoted` -> `string`), built once per theme into a flat `scopeId -> style` table. Dark and Light ship palettes derived from their existing tokens. A theme without a `syntax` block gets the built-in default. Applying a theme writes its rules to the `editor.theme.syntax.rules` setting (s8); the editor reads colours from there, not from the theme file. `bold`/`italic` are carried through but draw regular until more JetBrains Mono faces ship (only Regular ships today).

### 4.6 Brackets and folding

- Bracket pairs come from the grammar; brackets inside `string.*` or `comment.*` scopes are skipped. The primary cursor's enclosing pair is highlighted (only when the selection is empty, as Zed does).
- Folding is indentation-based by default (a row is foldable if the next non-blank row is indented more; the closing-bracket row stays visible), plus `foldMarkers` from the grammar and manual folds (vim `zf`, a gutter action).

### 4.7 v1 languages

JSON and the Arcane JSON assets (`.arcmat .arcproj .arcscene .arctheme .arcsyntax .meta` ...), HLSL, Lua (covers premake), Markdown (with embedded fenced code), INI/cfg, TOML, YAML, XML, **Arcane log** (timestamps, `[info]`/`[warn]`/`[error]` level colours matching the Console), batch, PowerShell, shell, **C and C++** (for users who choose Arcane for source files, D15; ported from lite-xl's `language_c` / `language_cpp`, MIT; `.h` is C++; C and C++ string and character literals end at the end of their line at the latest, and C++ raw strings are recognised -- `R"(...)"` exactly, and a delimited `R"d(...)d"` ends at the first `)d"` with any valid delimiter, because `.arcsyntax` format v1 has no back-references in `end` (it can end early on a nested `)x"`, never late) (rev 7, reconcile P4)), Plain Text (fallback).

Lua and Markdown start from lite-xl's `language_*.lua` content, translated into `.arcsyntax` with the MIT notice in the file header (the `"notice"` field, s4.1) and in `NOTICE.md` (lite-xl ships no JSON/HLSL/INI/TOML/YAML grammars; those are written fresh). No GPL Zed query file is used.

---

## 5. View and document

### 5.1 `TextEditorView`

- **Drawing:** a custom ImGui widget with a fixed line height, drawing coloured runs into the window draw list for the **visible rows only** (clipper row math). Font: JetBrains Mono (already shipped); `editor.text.font`, `editor.text.fontSize`; Ctrl+mouse wheel zooms per document.
- **Gutter:** line numbers (absolute / relative / hybrid -- `editor.text.lineNumbers`), fold arrows, bookmark and Problems markers.
- **Text area:** current-line highlight, per-row selection rectangles, multiple carets (`editor.text.caretBlink`), bracket-pair boxes, search-match backgrounds (active match distinct), indent guides, optional whitespace rendering, column rulers (`editor.text.rulers`).
- **Scrollbar annotations:** search matches, other cursors, Problems.
- **Long lines:** horizontal scroll; each visible line's glyph x-prefix is cached so column <-> x is O(log n), not O(line length) (a known lite-xl weak spot).
- **Unicode:** the cursor moves by grapheme cluster (a UAX #29 subset: combining marks, ZWJ emoji sequences, regional-indicator pairs), so accents and emoji are never split. Invalid UTF-8 bytes are single-byte clusters (s3.5). The subset does not cover Hangul jamo sequences (GB6-8), Prepend (GB9b) or Indic conjunct breaks (GB9c). Characters outside the BMP cannot be **typed** in v1 (16-bit `ImWchar`): they are dropped with a one-time warning (s1.2).
- **IME:** v1 positions the OS composition window at the primary caret through ImGui's platform IME data. Inline (underlined) composition is post-v1: it needs `SDL_HINT_IME_IMPLEMENTED_UI`, which is process-wide and would break every other ImGui text field. With vim on, the IME is **disabled in Normal, Visual and operator-pending modes** and re-enabled in Insert/Replace and the command line, so a Japanese IME never turns `j` into a composition. Disabling means disassociating the window's input context, done **before the next key** (when the mode is entered) and **re-applied every frame** after ImGui hands the platform its IME data, because SDL's text-input start re-associates the context and a disassociation made on `WM_KEYDOWN` comes after IMM has already taken the key (s5.5, rev 7).
- **Keyboard input and layouts (s5.5).**
- **Mouse:** click, drag, double-click word, triple-click line, Ctrl+click add/remove cursor, Alt+drag column selection, drag-select auto-scroll.
- **Split view (D8):** a second `TextEditorView` over the same `TextBuffer`, with its own scroll, `DisplayMap` (so folds are per view) and `SelectionSet`, docked beside the first. Edits in one view shift the other view's anchors through the buffer's edit notifications. Closing the primary view while split moves the secondary view into the primary slot. **Known limit:** vim manual folds (`zf`) in the moved view come back as indentation folds (lossy).

### 5.2 Built-in controls

- **Find bar** (Ctrl+F; Ctrl+H for replace), docked at the top of the document: regex, case, whole word, in-selection toggles; "3 of 17" count; Enter/Shift+Enter next/previous; **Alt+Enter turns every match into a cursor**; Replace All is one undo step; large files are searched on the worker against a snapshot. **Worker results are mapped forward, not thrown away (rev 7),** with the same edit-log mapping the highlight cache uses (s4.4; one mechanism): a result computed at version *v* applies at the current version when the edit log still covers the gap -- its matches are shifted, a match an edit overlapped is dropped, and the count is kept. A line-by-line count keeps no per-match list, so the worker tops it up by recounting the touched rows in the old and the new snapshot: the count is **exact** after that top-up and is never marked approximate (between the edit and the top-up, one poll, the kept total shows unmarked); a multi-line count carried across an edit shows `~` until its full recount (rev 7, reconcile F4). A Follow append applies the result and scans only the appended tail. The count shows "..." only until the first result has applied, so a log growing on every poll never leaves Find, Replace All or Alt+Enter re-scanning forever. When a regex hits its limits (s4.2) the bar shows `TextSearch`'s limit message. **Known limits:** a match that crosses a window boundary on an over-long line (s4.2) is cut at the window; a backward find scans from the document start on the worker.
- **Go to line** (Ctrl+G): a small popup taking `line` or `line:col`. Spec 2's Goto Anything `:` supersedes it.
- **Status strip** (document footer): Ln/Col, selection count, encoding, line endings, indentation ("Spaces: 4") -- each clickable to change -- language (opens the picker), vim mode + pending keys + "recording @q", Follow toggle for growing files, read-only badge.

### 5.3 `TextDocument`

- An `EditorDocument` (and `InspectorSource`). **Identity is the canonical path, always** -- `GetFinalPathNameByHandleW` on an open handle, which resolves case, 8.3 short names, junctions and symlinks. An asset's GUID is only a *lookup into* that path, never a second key, so opening the same file through the Asset Browser, a Console `file:line` link and (spec 2) Find in Files always focuses one tab. `DocumentHost` gains the canonical-path index; an asset move (`NoteMoved`) re-keys the document. **Known limitation:** two hard links to one file have different canonical paths and open as two tabs; a file-ID key would not help, because `ReplaceFileW` changes the file index on every save.
- **Registration:** `DocumentHost` gets a **fallback factory** used when no extension factory claims a path and the file sniffs as text (an explicit text extension with NUL bytes still opens, read-only; an unknown extension with NUL bytes is refused, s3.5). The fallback factory claims text files; whether a file reaches it is `FileOpener`'s decision (s5.6). Text extensions with no dedicated editor (`.md .ini .log .hlsl .lua .toml .yaml .xml .txt ...`) register this factory explicitly.
- **Reading:** files are opened with `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`, so a running game (or the editor's own logger) can keep writing, rotating or deleting the log being viewed. The undo policy (`undoGroupMs`, `undoMemoryMB`) is applied when the document opens, not only on its first reload.
- **Save** preserves encoding, BOM and per-line endings (s3.5). `editor.text.ensureFinalNewline` and `editor.text.trimTrailingWhitespace`, both off by default.
  - **Temp file:** written beside the target as `<name>.arctmp~<pid>-<n>`. The asset registry, project watchers and Content scanning **ignore the `.arctmp~` pattern** (one shared rule, tested), so a save never produces spurious create/delete or import events in Content or Config.
  - **Atomic replace:** flush the temp, then **`ReplaceFileW`** (not a plain rename). It preserves the original's ACLs, attributes, alternate data streams, creation time and object ID; it does **not** preserve the file index, and handles other processes hold do **not** follow to the new file. A file that does not exist yet is created with `MoveFileExW(MOVEFILE_WRITE_THROUGH)`.
  - **A writer would be orphaned.** If another process holds the file open for writing with delete sharing (common for loggers, and the editor's own reader does the same), `ReplaceFileW` succeeds but that process keeps writing into the replaced file, now pending deletion, and its output silently disappears. So before replacing, the save asks the Windows Restart Manager (`RmGetList`) which processes hold the file. If any do -- and always for a file that is under Follow or was modified externally in the last `editor.text.recentWriteSeconds` (default 10) -- the save shows a banner naming the process(es): **Overwrite in place** (truncate and rewrite through a write-shared handle, so the writer keeps writing into the same file; offered only when it can work, i.e. the holder allows write sharing), **Save anyway** (replace; the writer is detached), or **Cancel**.
  - **Fallback:** where `ReplaceFileW` is unsupported (some network shares and non-NTFS volumes return `ERROR_INVALID_FUNCTION`/`ERROR_NOT_SUPPORTED`), `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` is used; it loses ACL preservation, which the save notes once in the status strip.
  - **Partial-failure recovery:** `ERROR_UNABLE_TO_MOVE_REPLACEMENT` (original intact, temp still present) -> delete the temp, report, buffer stays dirty. `ERROR_UNABLE_TO_MOVE_REPLACEMENT_2` (the original now sits under the backup name) -> **move the backup back** to the original name before reporting; if that also fails, the banner names both files so nothing is lost. The save code handles these states; it does not just surface them.
  - **Read-only attribute -- VCS-aware (D14).** In game projects a read-only file usually means *locked by source control*, not *protected*: Perforce leaves files that are not checked out read-only, Git LFS makes `lockable` files read-only until you `git lfs lock` them, and Unity Version Control can do the same for locked files, but only when its opt-in read-only setting is on. Epic's Lore never sets files read-only. Removing the flag behind the VCS's back is a classic way to lose work. So the save is **refused** with a banner that:
    - names the likely reason, from a cheap `VcsProbe` that walks up from the file: a `.git` root with the path matching a `lockable` attribute in `.gitattributes` ("locked by Git LFS -- run `git lfs lock`"), Perforce ("not checked out in Perforce") -- a `.p4config` / `P4CONFIG` file found walking up, **or** `P4CLIENT` / `P4PORT` set in the environment, **or** set through `p4 set` (the `P4CLIENT` / `P4PORT` values under `HKEY_CURRENT_USER\Software\Perforce\environment`, then `HKEY_LOCAL_MACHINE\SOFTWARE\Perforce\environment` -- p4's own spelling; registry key names are case-insensitive), which is how most P4V installs configure a workspace (rev 7) -- a Lore workspace, a Unity VCS workspace, or none found ("may be under version control"). Detection is approximate: Perforce has no default `P4CONFIG` file name (`.p4config` is a convention), an environment or registry client says only that Perforce is configured on this machine, not that this file is in its workspace, and Lore SWFS workspaces cannot be detected by walking up (a known limitation);
    - offers **Save As...** first, and **Remove read-only and save** only behind an explicit confirmation that repeats the VCS warning.
    - ACL denials get the same banner without the remove option. Real VCS actions (**Lock and save**, **Check out and save**) wait for the source-control spec (s12.3); `VcsProbe` is the seam they will extend.
  - **Sharing violation** (the file is open without delete sharing, e.g. a log held by a running process): the save fails cleanly with a banner -- *"<file> is in use by another program"* -- offering **Retry**, **Save As...**, and, when the holder allows write sharing, **Overwrite in place (not atomic)**, which truncates and rewrites the file through a write-shared handle. `ReplaceFileW` on a target opened without delete sharing may report error 32, 1175 or 5; all three are treated as this sharing violation. The buffer stays dirty until a save succeeds; nothing is lost.
  - **Self-save suppression:** after a save the document records the written size, mtime and a content hash; watcher events matching them are ignored, so saving never triggers a "changed on disk" reload of itself.
- **External changes** (file watcher, debounced):
  - clean document -> silent reload keeping cursors and scroll (positions mapped by line/col), **recorded as one undoable transaction** (old text -> new text), so Ctrl+Z after a reload restores the previous version and the existing history stays valid behind it, as in VS Code; if the reload's undo entry (old and new text) exceeds `undoMemoryMB`, the history is cleared instead, with the s5.4 note (rev 7, reconcile T3);
  - dirty document -> banner: **Reload** / **Keep mine**; choosing Reload is recorded the same way (one undoable transaction holding the user's unsaved text), so even a mistaken Reload is one Ctrl+Z away;
  - append-only growth -> append, **only if the document is clean or read-only** (s3.5); a dirty document gets the dirty banner; truncation or rotation -> reload under the two rules above, settled as s3.5 says (never by waiting for two equal stamps; at worst after `editor.text.follow.maxDebounceMs`, rev 7);
  - deleted -> banner; Save recreates the file.
- **Crash safety:** the crash-window arc's autosave/recovery path does not exist yet, so text recovery is its own slice laid out per the crash-window spec's s8 file layout, under `Saved/Autosaves/Text/` (marker `RestoreData.json`), for that arc's plan 3 to fold in later. Recovered text takes the document's dominant line ending, and the recovery offer says so. Only **dirty** buffers are snapshotted; the write happens on a worker from a `RopeSnapshot` (never stalling the frame); buffers above `editor.text.recoveryMaxMB` (default 16) are skipped with a one-time note in the status strip; snapshots are offered back after a crash. **Snapshot files are session-scoped (rev 7):** each is named `<sessionId>-<docKey>.arcrecover`, where the session id is the writing process's start time plus its pid. A clean shutdown deletes only its own session's files. A crashed session's files survive until the user answers the recovery offer: **Recover** deletes them after the restore succeeds, **Discard** deletes them, and **Not now** (or dismissing the offer) keeps them and offers them again at the next launch. Nothing sweeps recovery files while an offer is pending, so a second editor instance or a clean exit can never delete another session's unanswered snapshots. There is still ONE marker, `RestoreData.json`: every entry carries its `sessionId`, each session writes the other sessions' unanswered entries plus its own, and `TextRecovery` is the marker's only writer (rev 7, reconcile G7). Every write is checked, flushed and renamed into place.
- **Session restore:** cursor, scroll, folds and split state per file, stored in user data (`editor.text.rememberViewState`).
- **Inspector page** (`TextDocumentInspectorPage`): path, size, encoding, language, line endings, indentation, read-only reason -- editable where meaningful (language, encoding, line endings, indentation). This keeps Arcane's one-Inspector model; the document draws no properties block of its own.

### 5.4 Open as Text on assets with a dedicated editor (D10)

- Asset context menu and Inspector gain **Open as Text** for any text-backed asset. (An "Open as text" entry already existed and opened the OS editor; its entry points are re-pointed to the Arcane Text Editor through `FileOpener`.)
- If the asset has a dedicated editor, the text document opens **read-only** with a banner and an **Enable editing** button.
- After unlocking, Save writes the file and triggers the registry's reload of that asset. An open dedicated editor for the same asset then:
  - **has unsaved changes** -> shows its own conflict banner instead of silently losing either side;
  - **is clean** -> reloads, and **its undo history for that asset is cleared**, with a visible note in its toolbar ("History cleared: the file was changed outside this editor"). Commands in a document-scoped stack may hold pointers into the old in-memory objects and must not survive the reload. The reload re-mints the document's undo anchor, `CommandStack::PruneExpired()` removes the steps that thereby expired from the editor's global `CommandStack` eagerly, and `InspectorHost::InvalidateSource` drops the Inspector history (the Inspector arc's prune-on-invalidate rule).
- The same rule applies to any external change that reloads an asset under a clean dedicated editor, not only Open as Text; the plan audits which dedicated editors already do this.

### 5.5 Keyboard input and layouts

Two kinds of input, never mixed -- but delivered as **one ordered stream** (rev 7): every key event and every character carries a sequence number, and the view and vim consume them front to back in the order they were typed, never grouped by kind. Outside vim's command-key mode the order is ImGui's input queue order (its input event trail, not the per-frame key-state sweep); in command-key mode the message hook owns the whole stream (named keys, Ctrl/Alt chords and characters, in `WM_KEYDOWN` / `WM_CHAR` order) and ImGui's text for that view is ignored for the frame. **Ctrl/Alt chords ride the same stream** as key events at their arrival position, named by their key (rev 7, reconcile A12): the plain view leaves them to the action table, and vim consumes them in order, so `ix<C-[>l` in one slow frame types `x`, leaves Insert, then moves. So `ix<Esc>` in one slow frame ends in Normal mode, and `f` followed by a dead key in the same frame is never reordered.
- **Chords** -- keys plus modifiers, through the action table's `KeyChord` (Labelled / Physical, already in `EditorActions`). **For letters, Labelled means the Latin letter of the key** (the virtual-key code), which is the Windows convention and what SDL3's default `latin_letters` keycode option gives: on Russian, Greek or other non-Latin layouts, `Ctrl+V` is the key whose VK is V, so every Ctrl-letter chord stays reachable. (The plan verifies Arcane's `KeyLayout` honours this.) Ctrl-letter chords in vim (`Ctrl+V`, `Ctrl+R`, ...) follow the same rule.
- **Characters** -- ImGui's input character queue (`WM_CHAR`), which is what the OS layout actually produced. Typed text comes from here, and so do **all vim keys that are characters**: `$ % { } " ^ ~ \` ' : / ? * #` and every letter. The vim keymap is keyed on characters plus named keys (`Esc`, `Enter`, `Tab`, `Insert`, arrows) plus Ctrl chords (`Insert` is a named text key, so vim's `<Insert>` Insert/Replace toggle works and is not a divergence); it never reads `ImGuiKey_4` to mean `$`.

Layout rules:
- **AltGr.** Windows delivers AltGr as Left Ctrl + Right Alt. Whether the active layout **has** AltGr is asked of the layout itself (`VkKeyScanExW` / `ToUnicodeEx` on the active HKL), not inferred from event timing. The active layout is **polled every frame** (`GetKeyboardLayout(0)` compared with the cached HKL) and re-queried when it changes: `WM_INPUTLANGCHANGE` and `WM_SETFOCUS` are *sent* messages, which SDL's message hook (posted messages only) never sees (rev 7). On such layouts, Right Alt held means AltGr: the key event is **text, never a chord** -- it fires no `text.*` action, no vim Ctrl handling, no Global action. So `{` on German (AltGr+7) or `@` on Polish never adds a cursor. Default `TextDocument` bindings use Ctrl+Alt only with non-printing keys (arrows).
- **Dead keys.** On US-International, German, French and similar layouts, `^ \` ' " ~` are dead keys that the OS composes with the next keystroke. That is right for typing (Insert, Replace, command line, find bar) and wrong for vim commands: on US-International `"a` composes to `ä`, so `"ayy` would never select register `a`. So **in Normal, Visual and operator-pending modes**, a hook on SDL3's Windows message hook (`SDL_SetWindowsMessageHook`, already used by `Platform/Window.cpp`, chained) turns `WM_DEADCHAR` into the bare symbol and **clears the layout's dead-key state** (a `ToUnicodeEx` call that consumes it), so the next key arrives uncomposed. In the text-entry modes the hook stays out of the way and composition works normally. **While a vim command waits for a literal character** -- a pending `f t F T r` -- command-key mode is off, so dead keys compose as typed text does: French `f^e` finds `ê`, and `rê` replaces with `ê` (rev 7). A pending **name** -- a mark (`m ' `` ` ``), a register (`" q @`, Insert-mode and command-line `Ctrl-R`) -- and Ctrl-W's command key keep command-key mode on, so names stay the Latin letters vim users expect on any layout (rev 7, reconcile D-R15). (Replace mode is text entry, where command-key mode is off anyway; Insert-mode `Ctrl-K` digraphs are outside v1's coverage.) This cannot be done from ImGui's character queue alone.
- **Non-Latin layouts in vim.** On a Cyrillic or Greek layout, Normal mode would receive Cyrillic/Greek characters and do nothing. `editor.text.vim.latinLettersInNormal` (default **auto**: on whenever the active layout's letter keys are non-Latin) makes letters in Normal, Visual and operator-pending modes come from the key's Latin letter (VK code); Insert mode always types the layout's characters. This covers the common case without a full `langmap`.
- **Control characters.** `Ctrl+letter` also produces a control character (0x01-0x1A) in `WM_CHAR`; the character stream **drops 0x00-0x1F except Tab and Enter**, so vim and the buffer never see both the chord and its character.
- **`Ctrl+[`** is a Labelled chord: on layouts where `[` needs AltGr it may be unreachable. `Esc` always works; vim users on such layouts typically remap (`jk` -> `Esc` is the documented example).
- **IME** is disabled outside Insert/Replace/command line when vim is on (s5.1): the context is disassociated from the focused window (the given one, else `GetFocus()`, else the window of the last key message) when the mode is entered, before the next key, and again twice every frame: right before the message pump -- ImGui runs its platform IME callback, through which SDL re-associates the context, in `EndFrame` -- and at the start of the view's input step (rev 7, reconcile E8). Tests check this at the key level through the real message queue: with a Japanese IME active, a posted `J` key-down in Normal mode (its `WM_CHAR` comes from `TranslateMessage`) arrives as the character `j`, with no composition (rev 7).

---

### 5.6 Openers: who opens what (D15)

Arcane never forces a tool. One `FileOpener` service decides how a file opens, and **every** route uses it: Asset Browser double-click and Enter, Console and Problems `file:line` links, `DocumentHost::OpenAt`, Open as Text, the IDE bridge's source rows, and spec 2's Goto Anything and Find in Files.

**Openers**

| Opener | How it opens | Line/column |
|---|---|---|
| **Arcane Text Editor** | a `TextDocument` tab | yes (`OpenAt`) |
| **Visual Studio** | the existing `IdeLaunch` path (late-bound DTE; solution-aware; opens in the instance that has the project's `.slnx`) | a running instance: line and column via `TextSelection.MoveToLineAndOffset`, falling back to DTE `ExecuteCommand("Edit.GoTo")`; a newly started instance: line only |
| **System default** | `ShellExecuteW(nullptr, L"open", path, ...)` -- whatever Windows associates with the extension (the existing `OsShell` helper) | no |
| **Custom program** | a user-defined entry `{name, exe, args}` launched with `CreateProcessW`; `args` is a template with `{file}`, `{line}`, `{col}`, `{project}`, `{solution}`, each quoted correctly | when the template uses them |

**Presets.** Custom entries can be added by hand or from detected presets: Visual Studio Code (`-g "{file}:{line}:{col}"`), Sublime Text (`"{file}:{line}:{col}"`), Notepad++ (`-n{line} -c{col} "{file}"`), JetBrains Rider (`--line {line} --column {col} "{file}"`), CLion (same), and Neovim in Windows Terminal (`wt nvim +{line} "{file}"`; Windows Terminal treats `;` as a command separator, so this preset breaks for paths containing `;`). Detection reads the registry's `App Paths`, the uninstall keys and `PATH`; a preset that is not installed is not offered. Arcane never installs or changes system file associations.

**Choosing**
- `editor.files.openWith` -- a map from extension (or the special keys `text`, `source`, `asset`) to an opener id. Scope `PreferencesProject`: a project may suggest defaults (e.g. a team that edits Lua in Arcane), the user's Preferences override it. Defaults: `.c .cc .cpp .cxx .h .hh .hpp .hxx .inl` -> `visualStudio`; other text -> `arcaneText`; assets with a dedicated editor -> their dedicated editor (`dedicated`; overridable per extension or through the `asset` key, but never to "nothing": Open as Text stays available). Stored as a `key=opener;...` string (there is no map cvar type). **A project may suggest only built-in openers (rev 7):** a value that comes from the project's own config (the Project rung) may name only `arcaneText`, `visualStudio`, `systemDefault` and `dedicated`. A custom-program id, or a detected-preset id (which resolves to a program), from the Project rung is refused: it is ignored, the user's or built-in default applies, and one Problems row names the project setting and the reason. Otherwise cloning a repository could make double-clicking any file run a program the repository chose. The user's own Preferences may name any custom or preset id.
- `editor.files.customEditors` -- the list of custom programs `{id, name, exe, args}`, stored as a JSON list (there is no list cvar type). Scope **Preferences only** (machine/user, `PreferencesMachine`; rev 7): a project can never define a program. The setting carries the settings arc's **`CVarFlags::LaunchesProgram`** (settings S7-SEC), so the settings registry itself refuses it from every rung a project can write -- the project's and a plugin's config, the engine config and the project's `Saved/Config` ("This project") -- and reports a config file that tries; it is honoured only from the user's machine-wide Preferences, `--set`, code and the editor's console. The opener service keeps the same rule as defence in depth (rev 7, reconcile SCOPE-ENFORCEMENT).
- A custom program that is a batch file (`.bat`, `.cmd`) is refused (`cmd.exe` re-parses its arguments, so the template's quoting cannot be trusted). The batch check strips trailing dots and spaces before comparing the extension, as Windows does when it resolves the file, so `x.cmd.` and `x.bat ` are refused too (rev 7).
- An **Editor > Files > Open With** Preferences page edits both: one row per extension group with an opener dropdown, a "Detect installed editors" button, and a custom-program editor with a live preview of the command line for a sample file.
- **Open With** submenu on every file context menu (Asset Browser, Inspector, Console/Problems rows, document tab): each opener, plus **Choose...**, plus an **Always use this for `.ext`** checkbox that writes `editor.files.openWith`. In the Inspector it is a right-click on the Open icon. The Console has no file rows today, so Open With (and `file:line` links) there are N/A until it does.

**Rules**
- A file already open in an Arcane `TextDocument` is focused there regardless of the opener setting (no second copy in another tool behind the user's back). An explicit **Open With** choice beats this rule: it opens with the chosen opener.
- An external opener that fails to launch (missing exe, Visual Studio not installed, `ShellExecuteW` error, a system-default open with no association) produces a Problems row naming the opener and the error, and offers **Open in Arcane Text Editor instead** for text files. It never fails silently.
- The C++ module rebuild flow is unchanged by the choice: saving a `.cpp` in Arcane is the same as saving it in any other editor (Tools -> Rebuild Game Module or the hot-reload watcher picks it up).
- Settings follow the user rule: every row above is a reflected setting, covered by the sweep tests and listed in the frozen names. The "no open bypasses `FileOpener`" rule is enforced first by access (the internal open entry points are private, with friend access for the opener wiring) and second by a source scan.


## 6. Vim layer

### 6.1 Activation and hooks

- `editor.text.vim.enabled` (Preferences, live). Off: no vim code runs.
- On: a `VimState` attaches to each `TextEditorView` and uses a small hook surface exposed by the view/core:
  - `SetCursorShape(block | bar | underline)`
  - `SetLineMode(bool)` (linewise selections)
  - `SetInputEnabled(bool)` (whether typed characters insert)
  - `SetClipAtLineEnds(bool)` (normal-mode cursor never past the last char)
  - events: `InputHandled`, `TransactionBegun`, `TransactionUndone`, `SelectionsChanged`, `OutsideChange` (rev 7, reconcile A9/A10: the document is about to change the buffer outside the view's input -- a save transform, a reload, a Follow append, Reopen with Encoding, converting line endings -- so vim ends an open Insert transaction first; the document fires it through an empty-safe hook vim subscribes to)
- **The cursor model, in one place.** The editor's cursors sit *between* characters; vim's sit *on* a character. `VimCursorModel` is the only code that converts, and every vim feature goes through it:
  - Normal/Visual: a vim cursor on character *c* at offset *o* is the editor caret at *o* with `SetClipAtLineEnds(true)` keeping *o* < line end (an empty line is the one exception: *o* = line start).
  - Inclusive motions (`e`, `$`, `f`, `t`, `%`) extend the editor range to *o* + length(*c*) before the operator runs; exclusive motions do not; linewise motions expand to whole lines including the line break.
  - Visual mode's selection always includes the character under the vim cursor: editor range = [min, max + length(char at max)).
  - `p` inserts after the vim cursor (editor offset *o* + length(*c*)), `P` at *o*; linewise registers insert below / above the line.
  - Entering Insert with `i` keeps *o*, with `a` moves to *o* + length(*c*); leaving Insert moves back one character (unless at line start), as vim does.
  - **The final newline ends the last line (rev 7).** When the text ends with a line terminator, that terminator ends the last line; vim sees no extra empty line after it, as Vim does with `eol` set: `"a\nb\n"` is 2 lines, `"a\nb"` is 2 lines (Vim would note `noeol`; no behaviour change), `""` and `"\n"` are each 1 empty line. Only the vim layer's line count and line addressing change -- `G`, `:$`, `:sort`, `:g`, `\%$`, the `j`/`k` limits, `o`/`O` and `dd` on the last line (`dd` deletes the line and its terminator; deleting the only line leaves `""`; `o` on the last line of `"a\nb\n"` gives `"a\nb\nnew\n"`). The rope keeps its bytes exactly and saving is unchanged. The shared view still draws its trailing empty row, but in Normal, Visual and operator-pending the vim cursor never lands on it.
  These rules are tested directly (s11.3) because they are where vim layers historically break.
- The editor knows nothing else about vim. (This is the shape of Zed's `vim` crate -- an addon over the editor -- reimplemented clean-room; Zed's vim code and `vim.json` are GPL and are not copied. Vim's key behaviour itself is not copyrightable.)

### 6.2 Modes and grammar

- Modes: Normal, Insert, Replace, Visual, VisualLine, VisualBlock, operator-pending, command line (`:` `/` `?`).
- Grammar: `[count]["reg][count]operator[count]{motion | textobject}` or `[count]action`. Effective count = pre x post.
- **Every operator = expand each selection by the motion/object, then run a normal `TextCommands` command** (delete, change, yank, indent ...). This is why vim and multi-cursor compose for free. `MotionKind` is linewise / exclusive / inclusive, per vim.
- VisualBlock is represented as one selection per line in the ordinary `SelectionSet`.
- **A failed command beeps and flushes only non-typed input (rev 7).** When a motion or command fails (`h` at column 0, `u` with nothing to undo, a missing mark), vim beeps and drops the rest of the input that did not come from the keyboard -- the macro being replayed, the `:normal` keys and the remap queue -- as Neovim's `beep_flush()` does (`flush_buffers(FLUSH_MINIMAL)`). Keys the user typed after it still run: `hl` at column 0 moves right, and `ux` with nothing to undo still deletes.
- In normal mode with "Vim handles Ctrl keys" on, `gb` adds the next match (Sublime's Ctrl+D) so multi-cursor stays reachable.

### 6.3 Coverage (v1)

| Area | Coverage |
|---|---|
| Motions | `h j k l w W b B e E ge gE 0 ^ $ g_ gg G \| f F t T ; , % ( ) { } H M L`, `/ ? n N * #`, `` ` `` / `'` marks, `Ctrl+D Ctrl+U Ctrl+F Ctrl+B Ctrl+E Ctrl+Y zz zt zb` |
| Operators | `d c y > < = g~ gu gU J gJ zf`, `gc` (comment toggle from the grammar's `comment`, padded with one space like Neovim's `gc`: `// text`), `Ctrl+A` / `Ctrl+X` |
| Text objects | `iw aw iW aW is as ip ap`, `i" a" i' a' i\` a\``, `i( a( ib i{ a{ iB i[ a[ i< a<`, `it at` (XML) |
| Registers | `"` unnamed, `0-9` (deletes shift), `a-z` (`A-Z` append), `-` `_` `+` `*` `/` `:` `.` `%`. Multi-cursor yank stores one entry per cursor; paste with an equal cursor count distributes. `editor.text.vim.useSystemClipboard` makes `"` alias `+`. |
| Repeat | `.` replays the last change (recorded commands + inserted text). `q{reg}` / `@{reg}` / `@@`: macros are **key text stored in the register**, so `"ap` pastes a macro for editing, as in vim. |
| Marks / jumps | `a-z` per file; `A-Z` global (opening the file through `DocumentHost`); special marks `` ` ' . ^ [ ] < > ``; jump list `Ctrl+O` / `Ctrl+I`; change list `g;` / `g,` |
| Undo | `u`, `Ctrl+R`; one insert session = one undo step |
| Folds | `zc zo za zR zM zf zd zj zk` over `FoldModel` |
| Insert mode | `Ctrl+W Ctrl+U Ctrl+R{reg} Ctrl+O Ctrl+T Ctrl+D`, `Esc` / `Ctrl+[` (per-mode chord table, s6.6) |
| Ex | ranges `% . $ N 'a /pat/ ?pat? +n -n '<,'>`; `:w :q :wq :x :q! :e :sav :r`, `:s` with `g i c` (`c` prompts per match) and `&`, `:g` / `:v`, `:normal`, `:d :y :m :t :co :> :< :j :sort` (with `n u i r` options), `:noh`, `:set`, `:map` family (`nmap vmap imap omap` + `noremap` forms), `:sp :vs :bn :bp :ls`, `:marks :reg :jumps` (popup listings) |

### 6.4 Options, remaps, persistence

- **Flavour (D13): Neovim defaults.** `Y` = `y$`, `hlsearch` and `incsearch` on, `startofline` off, `&` = `:&&`, `nrformats` = `bin,hex` (no octal), `scrolloff` 0, `wrapscan` on. Classic-Vim behaviour is one option away for each. Consequences the Neovim v0.12.5 oracle confirmed (rev 7): with `startofline` off, linewise `Vd`, `vX` and `>>` keep the cursor column; linewise `gu`, `g~` and `vY` leave the cursor at column 0, and `gcc` at the start of the comment leader; after a block `A` / `$A` the cursor returns to the block's start; recording a macro leaves the unnamed register as it was; `di(` with the cursor outside any parentheses works on the next `( )` block; a same-line `d%` does not fill `"1`; Insert-mode `Ctrl-R` inserts the register literally, without autoindent.
- Options are Arcane settings under `editor.text.vim.*`: `ignorecase`, `smartcase`, `hlsearch`, `incsearch`, `wrapscan`, `gdefault`, `timeoutlen`, `scrolloff`, `startofline`, `nrformats`, `yankToEol` (the `Y` choice), `leader`, `handleCtrlKeys` (default on while vim is on), `useSystemClipboard`.
- Tab width, spaces/tabs and relative numbers are **editor-wide** text settings (`editor.text.*`), not vim-only. Shift width **is** `editor.text.tabSize` (one indent width), so `:set sw` and `:set ts` move together.
- `:set` changes a value for the **session only**; the Preferences page changes it permanently.
- Remaps: a table on a **Vim** Preferences page (`editor.text.vim.remaps`: `{mode, from, to, recursive}`); `:map` adds a session-only remap. Remaps resolve through a key trie with `timeoutlen` for ambiguous prefixes.
- Marks (`A-Z` and per-file), registers, and search/command history persist in user data across sessions (`editor.text.vim.persistState`), capped as Neovim's `shada` defaults are (50 lines / 10 KB per register, 10000 history entries).

### 6.5 Vim patterns -> PCRE2

Vim's regex dialect differs from PCRE2 (`\<` `\>`, `\(` `\)`, `\|`, `\{n,m}`, magic levels, `\zs`/`\ze`, `~`). `VimPattern::Translate(vimPattern, options) -> PCRE2 pattern | error` is a pure function used by `/`, `?`, `*`, `#`, `:s`, `:g`, `:v` and `:sort /pat/`.

| Vim | PCRE2 |
|---|---|
| magic (default): `\(` `\)` `\|` `\{n,m}` `\+` `\?` `\=` | `(` `)` `|` `{n,m}` `+` `?` `?` |
| `\{-}` `\{-n,m}` `\{-n,}` `\{-,m}` (non-greedy) | `*?` `{n,m}?` `{n,}?` `{0,m}?` |
| `\%(` ... `\)` (non-capturing group) | `(?:` ... `)` |
| `\v` very magic, `\m` magic, `\M` nomagic, `\V` very nomagic | switch the translation table from that point on |
| `\<` `\>` | `(?<![K])(?=[K])` / `(?<=[K])(?![K])`, where `[K]` is a character class **generated from the grammar's `wordChars`** (vim's `iskeyword`), never PCRE's `\w` |
| `\k` `\K`, `\i` `\I` | `[K]` / `[K]` minus digits (keyword); `\i` uses the same class (identifier) in v1 |
| `\s` `\S` | `[ \t]` / `[^ \t]` -- **vim's `\s` is space/tab only**, unlike PCRE's |
| `\d \D \x \X \o \O \w \W \h \H \a \A \l \L \u \U` | `[0-9]` `[^0-9]` `[0-9A-Fa-f]` `[^0-9A-Fa-f]` `[0-7]` `[^0-7]` `[0-9A-Za-z_]` `[^0-9A-Za-z_]` `[A-Za-z_]` `[^A-Za-z_]` `[A-Za-z]` `[^A-Za-z]` `[a-z]` `[^a-z]` `[A-Z]` `[^A-Z]` (ASCII, as vim defines them) |
| `\zs` `\ze` | **top level only**: `\zs` -> `\K` emitted at the top level of the pattern (never inside a lookaround, so `PCRE2_EXTRA_ALLOW_LOOKAROUND_BSK` is never needed); `\ze` -> the rest of the top-level pattern wrapped in a lookahead. Either one inside a group or an alternation (`foo\zebar\|baz`) is a documented divergence with its error. |
| `\c` `\C` anywhere | `(?i)` / `(?-i)` for the whole pattern, overriding `ignorecase`/`smartcase` |
| `~` (last substitute string) | the escaped literal of the previous `:s` replacement |
| `\_s` `\_.` `\_x` (any `\_` class), `\n` | multi-line classes -> the multi-line search path (s4.2) |
| `\%^` `\%$` (start / end of **file**) | the pattern is **always routed to the multi-line path** (whole-snapshot subject), where `\A`/`\z` mean what vim means. The line-by-line path never sees them, so `\A` cannot match at every line. |
| `^` `$` (start / end of line) | on the line-by-line path the scratch subject *is* the line, so `^`/`$` are exact; on the multi-line path the pattern is compiled with `PCRE2_MULTILINE` |
| `\%V` `\%23l` `\%23c` `\%23v` | position predicates checked on each candidate match after PCRE2 returns it |
| `:s` replacement `\0`-`\9` `&` `\u` `\U` `\l` `\L` `\e` `\E` `\r` `\n` `~` | expanded by the substitute engine, not by PCRE2 |

Search-command details that are not translation:
- `*` and `#` search the word under the cursor wrapped in `\<` `\>` and **ignore `smartcase`** (they honour `ignorecase` only), as vim does; `g*`/`g#` drop the word boundaries.
- Patterns are compiled once per distinct `(translated text, flags)` and cached per document.
- Vim search on an over-long row (past `longLineMaxBytes`) copies the whole row into the scratch subject; it is not windowed as find is (s4.2).

Anything outside the table is a **documented divergence** (listed in the vim help page and in the oracle's divergence notes, s11.3), reported as "E: unsupported pattern item `\%[...]`" rather than silently misbehaving. Known v1 divergences: `\%[...]` optional sequences, `\@<=`-style vim lookbehind spellings beyond the PCRE-expressible subset, `\%d123` code-point items, and equivalence classes `[[=a=]]`.

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
- The documented divergence list (`VimDivergences()`) is the list on the vim help page and the one the oracle's divergence notes use (s11.3), including "an opened fold is forgotten" and "macro key text" (a macro register holds Arcane key notation such as `<Esc>`, not raw bytes; rev 7, reconcile G5).

---

## 7. Shortcuts: unified with the action table (D6)

The editor already has one action table (`Input/EditorActionTable.hpp`) with contexts ranked by specificity (Global < Document < panels < Text) and `PressedInWindow` for a focused window answering a Global action itself.

1. **Shared verbs reuse existing action ids.** `edit.undo`, `edit.redo`, `edit.redoAlt`, `edit.cut`, `edit.copy`, `edit.paste`, `document.save`, `document.close` are answered by the focused text document via `PressedInWindow` (the Settings window's local-undo precedent). Rebinding Undo rebinds it everywhere. Each text document owns its history: Ctrl+Z in a text file never touches the scene's `CommandStack`.
2. **Text-only commands are new rows** in the same table under a new `ActionContext::TextDocument` (specificity above Document, alongside panels). Initial set (ids `text.*`): word left/right (+select), line start/end, document start/end, page up/down, add next match (Ctrl+D), select all matches (Alt+F3), split selection into lines (Ctrl+Shift+L), add cursor above/below (Ctrl+Alt+Up/Down), column select by keyboard (Ctrl+Shift+Alt+arrows; rows shorter than the column are skipped), select line (Ctrl+L), expand selection to brackets (Ctrl+Shift+M), duplicate line (Ctrl+Shift+D), delete line (Ctrl+Shift+K), move line up/down (Alt+Up/Down), indent/outdent (Tab / Shift+Tab, Ctrl+] / Ctrl+[), toggle line comment (Ctrl+/), toggle block comment (Ctrl+Shift+/), join lines (Ctrl+J), find (Ctrl+F), replace (Ctrl+H), find next/previous (F3 / Shift+F3), go to line (Ctrl+G), fold / unfold (Ctrl+Shift+[ / ]), go to matching bracket (Ctrl+M), toggle bookmark (Ctrl+F2), next/previous bookmark (F2 / Shift+F2), zoom in/out/reset (Ctrl+= / Ctrl+- / Ctrl+0), toggle follow (`text.toggleFollow`) and convert line endings (`text.convertLineEndings`). The chords follow VS Code/Sublime; `text.toggleFollow` and `text.convertLineEndings` ship **unbound**. Split view has no row (it is the status-strip toggle and vim `:sp`/`:vs`). They appear on the Shortcuts page, are rebindable, get conflict detection, and are listed by spec 2's palette for free. The `TextDocument` context is exempt from the typing gate (it is the typing surface), and the text area sets `ImGuiWindowFlags_NoNavInputs` so ImGui keyboard navigation never eats editing keys. A pre-existing settings defect, `ParseKeyChord("Ctrl+[")` failing, is fixed by the plan so the `Ctrl+[` rows can be stored.
3. **Ctrl+D** in a text document is add-next-match (Sublime); the Global `edit.duplicate` is shadowed there by context specificity, exactly as `graph.duplicate` shadows it in the graph today. Duplicate line is Ctrl+Shift+D.
4. **Vim keys are not chords** and live in vim's own sequence keymap (s6.4); vim's commands still call `TextCommands`, so `u` and Ctrl+Z share one history. Which Ctrl chords vim claims, per mode, is the s6.6 table; with `handleCtrlKeys` off, every Ctrl chord keeps its action-table meaning.
5. **New global row:** `document.closeAlt` (Ctrl+F4), so closing a document never depends on Ctrl+W, which vim repurposes.

---

## 8. Settings

All under `editor.text.*` (Preferences scope unless noted), registered through the settings arc's reflection (`ARC_REFLECT_TYPE_ATTR(Settings, ...)`) so they appear on the Preferences window, the sweep tests and the inventory:

`font`, `fontSize`, `tabSize`, `insertSpaces`, `detectIndentation`, `lineNumbers`, `rulers`, `renderWhitespace`, `indentGuides`, `caretBlink`, `highlightCurrentLine`, `undoGroupMs`, `undoMemoryMB`, `syntaxMaxMB`, `openMaxMB`, `longLineMaxBytes`, `multilineSearchMaxMB`, `maxCursors`, `tokenizeLineBudgetUs`, `highlightFrameBudgetMs`, `syncCatchUpLines`, `syncCatchUpBudgetUs`, `recoveryMaxMB`, `regexMatchLimit`, `regexDepthLimit`, `ensureFinalNewline`, `trimTrailingWhitespace`, `languageByExtension`, `rememberViewState`, `autoClosePairs` (defaults: VS Code's default auto-closing pairs), `autoIndent`, `defaultLineEnding` (`PreferencesProject`), `recentWriteSeconds`, `watchPollSeconds`, `bracketScanMaxLines`, `detectIndentationRows`, `followPollSeconds`, `followChunkKB`, `recoveryIntervalSeconds`, `viewStateMaxFiles`, `follow.maxDebounceMs` (rev 7: the cap after which a pending rotation or truncation settles even while the file keeps changing, s3.5; Dev, measured default), and the `vim.*` group from s6.4. Outside `editor.text.*`: `editor.theme.syntax.rules` (s4.5) and the `editor.files.*` group (`openWith` `PreferencesProject` with the Project-rung restriction, `customEditors` Preferences only and `LaunchesProgram`, settings S7-SEC; s5.6). Budgets and limits are Dev-flagged (advanced) rows; their defaults are set from the s11.5 measurements, not guessed. Project scope may override indentation and line-ending defaults (`PreferencesProject`).

---

## 9. Hooks for spec 2 and the rest of Arcane

- `FileOpener::Open` / `FileOpener::OpenWith(path, opener, FilePosition{line, column})` (1-based; column in characters) is the open hook for spec 2 and for Console/Problems `file:line` links: it honours the user's opener choice (s5.6), and for the Arcane Text Editor it opens or focuses, moves the primary cursor and centres the line. `DocumentHost::OpenAt` is internal.
- `ISymbolProvider` on documents: `Symbols() -> vector<{name, kind, range}>`. `TextDocument` implements it from the grammar's `symbols` rules (Markdown headings, JSON keys, HLSL/Lua functions). Symbols are not extracted for documents above `syntaxMaxMB`. Spec 2's Goto Anything `@` queries the active document through it; other documents (shader graph, input actions) may implement it later.
- `TextCommands` are registered as actions (s7), so the palette lists them without extra wiring.
- `TextSearch` exposes a pure `Search(snapshot, query) -> matches` used by the find bar and reusable by spec 2's Find in Files.

---

## 10. Third-party and licensing

| Source | Licence | Use |
|---|---|---|
| Zed `sum_tree` | Apache-2.0 | Informs the concrete `Rope` (design + attribution in `NOTICE.md`; no generic machinery ported) |
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
- **Incremental == full:** property test that after random edits, the incremental highlight cache equals a full re-tokenize. It runs with a fake clock and **randomised** per-line and per-frame budgets (tiny ones included), and with an extra grammar holding an `(a+)+$` rule under a small match limit, and it compares Partial rows' final results too -- with budgets of 0 and no limit-hitting rule it could not catch a stalled frontier or a resume that differs from an unbudgeted run (rev 7).
- **Version discipline:** with a held worker and interleaved edits (deterministic): rows above the earliest edit since the result's version are applied; rows below are applied shifted only when the edited rows converged; nothing stale is ever applied. **No-starvation test:** Follow appending every frame and steady typing at the end of a file both still colour every row above within a bounded number of frames. A far jump draws plain lines and fills in when the worker arrives; UI-thread catch-up never exceeds `syncCatchUpLines` or `syncCatchUpBudgetUs`. A grammar reload invalidates entries by `grammarVersion` while the worker holds the old snapshot safely.
- **Line-local grammars:** a visible line deep in a file above `syntaxMaxMB` is coloured; a `begin`/`end` region left open at end of line is force-closed there.
- **Long lines:** a 40 MB single-line JSON opens, draws, and the following line highlights from the root state; the incremental == full property test includes over-cap lines (the cap and resync are part of the tokenizer, so both sides apply them).
- **Regex limits:** a catastrophic pattern (grammar rule and find query) hits the match limit, is reported, and the worker and UI keep running.
- Grammar validation: malformed grammars produce a Problems row and fall back to Plain Text.

### 11.3 Vim
- Table-driven cases `{start text + cursor, keys, expected text + cursor, expected mode/register state}`, hundreds of them, pure C++.
- **Cursor-model tests** for every rule in s6.1 (inclusive motions, visual's extra character, `$`, `p`/`P`, `i`/`a`, leaving Insert, empty lines).
- **Pattern translator tests:** every row of the s6.5 table (including non-greedy `\{-}`, `\%(`, the ASCII classes, vim's space/tab `\s`, `\<` against a grammar `wordChars` with non-`\w` characters, and `\%^`/`\%$` never matching mid-file), `*`/`#` ignoring `smartcase`, plus the divergence list producing its error.
- **Keyboard tests (s5.5):** AltGr characters never fire chords; vim keys arrive from the character queue on German, French and US-International layouts, including dead-key compositions; keys, Ctrl chords and characters typed in one frame are consumed in typed order (`ix<Esc>` and `ix<C-[>` end in Normal; rev 7, reconcile A12); a real pending dead key (US-International `'`) is consumed so the next key arrives uncomposed; the IME is disabled in Normal/Visual/operator-pending and enabled in Insert, checked at the key level through the real message queue (rev 7).
- **Chord table tests:** each row of s6.6 in each mode, and with `handleCtrlKeys` off.
- **Neovim oracle (D9, D13):** `scripts/vim-oracle.ps1` runs each case in its **own** `nvim --headless -u NONE -i NONE -n` process, with `filetype off`, so no state (`.`, `@@`, the last `f`, `~`) carries between cases and no plugin loads: `--clean --noplugin` still loads matchit in v0.12.5 (which replaces `%`), and only `-u NONE` gives Neovim's built-in `%`, which is Arcane's; Neovim's built-in `_defaults.lua` mappings still apply -- measured on v0.12.5: `gc` / `gcc` in Normal, Visual and operator-pending, `Y` = `y$`, `&` = `:&&<CR>`, Ctrl-L, Insert Ctrl-U / Ctrl-W, Visual `*` `#` `@` `Q`; the only mapping that differs from `--clean` is matchit's `%` (rev 7, reconcile D-R13). It feeds the keys and reads the end state from a timer callback, so a case may end in Insert, Replace, operator-pending, the command line or a pending `f` (`nvim_feedkeys(..., 'tx!')` blocks in those). Comparisons are case-sensitive. It writes or verifies the expected columns. Dev-time only; tests never require Neovim (rev 7).
  - **Pinned version.** The script pins one Neovim release (checked at start; it refuses another) and installs it only from the release asset with the pinned SHA-256. The pin is **Neovim v0.12.5** (latest stable, 2026-08-23; `nvim-win64.zip` SHA-256 `de8625ba8cf65ebf40eb80a388ba1ec8e9c15b30218821e2c639119b05920de1`), the release the plan's first oracle run moved to (rev 7). Defaults change between minor versions, so bumping the pin is a deliberate re-bless with a reviewed diff of changed expectations.
  - **Neovim's defaults are the target**, including the ones beyond D13's list that change outcomes, and Arcane emulates them: `formatoptions` contains `j` (`J` removes comment leaders, using the grammar's `comment` tokens), `autoindent` on, `nojoinspaces`.
  - **No filetype is ever set.** Neovim enables `filetype plugin indent on` by default, so setting `filetype=lua` would load `ftplugin/lua` and `indent/lua.vim` and change `indentexpr`, `formatoptions` and `iskeyword` -- every `o`, `O`, `cc` and Insert-mode Enter case would compare against Neovim's Lua indenter instead of Arcane's grammar rules. Instead, each case declares the Arcane grammar it runs with, and the oracle sets **`commentstring` directly from that grammar's `comment`** (with Neovim's padding convention, `// %s`), sets **`comments`** from the same grammar (what `J`, `:j` and `formatoptions+=j` read; Neovim's default has no `--` leader, for instance) and keeps indentation options fixed (`autoindent` on, no `indentexpr`) (rev 7 adds `comments`).
  - **Word characters are aligned.** Neovim's default `iskeyword` is `@,48-57,_,192-255` and treats characters above 255 by Unicode class. Arcane's vim word class is the grammar's `wordChars` for ASCII, Neovim's default `iskeyword` for Latin-1, and a port of Neovim's `utf_class` above U+00FF (per-script classes, emoji, the CJK punctuation blank) -- measured equal to Neovim v0.12.5's `charclass()` for every code point up to U+3FFFF -- so `w` over `café` or between kanji and kana matches Neovim (rev 7, reconcile A9). The editor's own whole-word search stays the grammar's `wordChars` plus `\p{L}\p{N}` (s5.2). The oracle sets `iskeyword` to Neovim's default **plus the grammar's extra ASCII word characters** (INI's `.` and `-` as `46,45`), so `w`, `e`, `cw` and `diw` over `key.name-x` agree without a divergence note; grammars that narrow the class say so, and such cases carry a divergence note (rev 7).
  - **Final newlines are covered.** A case's `in:` / `out:` strings are the Vim buffer's LINES joined by `\n`; the rope is that string plus a final `\n` (Vim's `'eol'`, the default for files) unless the case says `eol: off`, which builds the rope without it and runs the oracle with `:setlocal noeol nofixeol`. The final-newline cases come in pairs, the same keys on the same text with and without `eol` (`:sort`, `:g/^$/d`, `G`, `dd` and `o` at the end, `\%$`, `J` on the last line), so the s6.1 rule is checked against Neovim (rev 7, reconcile D-R14).
  - **Known divergences up front:** `=` (Arcane's `=` uses the grammar's `indent` rules; the oracle has no `indentexpr`), `\ze` inside a group or alternation (s6.5), and the s6.5 pattern items. Each divergent case carries a `divergence` note instead of an oracle expectation; a case of a divergent command whose result happens to equal Neovim's (two `=` cases do) carries none and is verified like any other. The full list is `VimDivergences()` (s6.6), the same list the vim help page shows.
- **Multi-cursor vim cases are hand-written.** Neovim has no multi-cursor, so the oracle cannot produce them. They live in their own table file, are reviewed as their own task, and are derived from the rule "each cursor behaves as a single-cursor vim would, then selections merge" (s2.1, s6.2).

### 11.4 View and document
- **Goldens:** the headless editor opens sample files (JSON, HLSL, Markdown, log; vim normal/visual; find bar open; split view) and screenshots them -- re-blessed through the established golden procedure. The vim goldens are blessed with the Experimental badge showing and re-blessed by the vim-only gate (VIM-GATE) when the badge drops.
- **Witness scenarios:**
  - open a log another process holds open for writing (no delete sharing), follow it while it grows, survive a rotation and a truncation -- with the writer still writing (every 100 ms, polled more slowly than it writes, so no two polls see the same file state; rev 7);
  - save into a file locked by another process -> the in-use banner, buffer still dirty, Retry succeeds after release; with write sharing allowed, Overwrite in place succeeds;
  - a save does not trigger a self-reload, and the `.arctmp~` temp never reaches the asset registry or project watchers;
  - `ReplaceFileW` keeps a normal file's ACLs and attributes; a **read-only** file refuses with the banner, and Remove read-only and save succeeds;
  - injected `ERROR_UNABLE_TO_MOVE_REPLACEMENT` / `_2` leave the original intact (restored from the backup name in the `_2` case) and the buffer dirty; the unsupported-`ReplaceFileW` fallback saves;
  - one file opened through the Asset Browser, a Console `file:line` link and a different-case / 8.3 path focuses a single tab;
  - a dirty document whose file grows shows the banner, not an append; an append to a clean document leaves it clean with its undo history intact;
  - Reopen with Encoding -> Windows-1252 makes a legacy `.bat` editable and saves it back as 1252, byte-identical when unedited (an in-process `[text-doc]` test case, TE2-15, rather than a witness run);
  - external change with and without unsaved edits;
  - Open as Text read-only -> unlock -> save -> asset reload, with the dedicated editor clean (history cleared, note shown) and dirty (conflict banner);
  - crash recovery restores an unsaved buffer; a dirty buffer above `recoveryMaxMB` is skipped with its note, and snapshotting never stalls a frame; a crashed session's snapshots survive a clean exit of another session while their offer is unanswered (rev 7).

### 11.5 Performance budgets (`[perf]`, baselined)
- Open a 100 MB log (line-local grammar): time to first coloured frame.
- Ctrl+End in a 30 MB stateful-grammar file: frame time stays within budget (plain lines first, colour later).
- Keystroke: edit + retokenize + layout under 1 ms on a 10k-line JSON file.
- Scroll: frame cost at 60 lines/frame on a large file.
- Highlight-on-open of a 5 MB file to the visible range.

Baselines live with the existing automation baselines, as a `perf[]` table (per configuration) in `scripts/automation-baselines.json`, checked by `check-baselines.ps1 -PerfReportPath`; regressions fail the suite.

---

## 12. Delivery

### 12.1 Order (each step ends green; the plan breaks them into tasks)

1. **Core:** leaf-size benchmark, `Rope`, `TextBuffer` + undo, `EolMap`, `SelectionSet`, `TextCommands`, `DisplayMap` (folds), encodings. No UI; fully tested.
2. **First visible editor:** `TextDocument` with canonical-path identity, basic `TextEditorView` (draw, edit, multi-cursor, mouse, the s5.5 keyboard/layout input layer, save incl. `ReplaceFileW` + fallback + recovery paths + sharing rules + the registry's `.arctmp~` ignore rule, external change incl. clean-only append), Windows-1252 reopen, fallback registration in `DocumentHost`, shared-verb actions + `document.closeAlt`. Usable from here.
3. **Syntax:** PCRE2 vendored (with limits, per-thread match data); grammar engine with refcounted grammar snapshots, versioned cache with forward-mapped worker results, line-local grammars, long-line cap; v1 grammars; `.arctheme` `syntax` block; grammar tests.
4. **Editing features:** find/replace, go to line, folding, brackets, split view, status strip, Inspector page, text-only actions in the table.
5. **Vim** (behind **Experimental**, D12), four passes: (a) cursor model, modes, motions, operators, text objects, chord table; (b) registers, dot-repeat, macros, marks; (c) ex commands + the pattern translator; (d) options, remaps, persistence, Vim Preferences page. Oracle tables grow with each pass.
6. **Polish:** log follow, crash recovery, session restore, Open as Text (with the undo-history rule), perf budgets, goldens, witness scenarios.

### 12.2 Release gate (D12)

- **The text editor is released** when steps 1-4 and the step-6 essentials -- log follow, crash recovery, Open as Text, the perf budgets, goldens and witness scenarios -- are green, through a gate task like the settings arc's. Session restore may trail.
- **Vim ships behind an "Experimental" label** (a badge on the Vim Preferences page and in the status strip while vim is on). Each pass widens what Experimental covers; the label drops when pass (d) and the full oracle suite are green, through a second, vim-only gate.
- Spec 2 (palette, Goto Anything, Find in Files) can start after the first gate; it does not wait for vim.

### 12.3 After v1 (not in this spec)
Minimap; soft wrap (likely first, for Markdown and logs -- a second `DisplayMap` layer); word completion; diff view; tree-sitter for structural folding/outline/syntax-aware text objects behind the scope interface; shader editor snippet fields and crash log tail adopting the core; CJK fallback font; digraphs; opt-in confirmed `:!`; **source-control integration** (its own spec: git first, then Lore; adds Lock/Check out and save on top of `VcsProbe`).

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

---

## 14. Revision 3 (2026-10-07): second independent review folded in

All findings accepted. Items 1-4 changed the shape of the code and were required before planning; 5-8 and the smaller points were cheap enough to settle here rather than carry as plan tasks.

| Finding | Resolution |
|---|---|
| Discard-on-version-mismatch starves the worker under steady typing or Follow | Results mapped forward through the edit log: rows above the earliest edit always land; rows below land shifted when the edit converged (s4.4); no-starvation test (s11.2) |
| Appended tail had no undo/dirty rules; dirty-and-growing ambiguous | Append only when clean or read-only; not an undo entry; `changeId`/`savedChangeId` unchanged; history stays valid (s3.5, s5.3) |
| Keyboard layouts: AltGr, characters vs keys, dead keys, `Ctrl+[`, IME in Normal | New s5.5: chords vs character queue, AltGr is text, dead-key behaviour, IME off outside Insert (s5.1) |
| Translator bugs: `\<` used `\w`; `\%^` broken per line; missing `\{-}`, `\%(`, classes; vim `\s`; `*`/`#` smartcase | Table rewritten (s6.5) |
| Oracle diverges beyond D13; version drift; `gc` needs filetype; `=` | Pinned Neovim, Neovim defaults emulated (`formatoptions+=j`, `autoindent`, `nojoinspaces`), per-case filetype, `=` a declared divergence (s11.3) |
| Long-line resync contradicts no-resync rule and breaks incremental == full; `lineLocal` undecidable | Named exception applied identically in all modes; `lineLocal` = force-close regions at end of line (s3.5, s4.1, s11.2) |
| `ReplaceFileW` contradictions and gaps | Read-only refused with Remove-read-only-and-save; `_REPLACEMENT`/`_2` recovery; unsupported-volume fallback; overwrite-in-place option; `.arctmp~` ignored by Arcane's watchers (s5.3) |
| GUID-or-path identity can split one file into two tabs | Canonical path (`GetFinalPathNameByHandleW`) is the only key; GUID is a lookup (s5.3) |
| Catch-up bounded only in lines | Also `syncCatchUpBudgetUs` (s4.4) |
| PCRE2 per-thread state unstated | Match data, contexts and JIT stacks per thread (s4.2) |
| Grammar hot-reload under the worker | Refcounted grammar snapshots + `grammarVersion` on cache entries (s4.1, s4.4) |
| Windows-1252 files stuck read-only | Reopen with Encoding -> Windows-1252 in v1 (s3.5) |
| Crash recovery could stall on big buffers | Dirty-only, worker-written from snapshots, `recoveryMaxMB` cap (s5.3) |
| Multi-cursor vim expectations can't come from the oracle | Hand-written table file with its own review (s11.3) |

---

## 15. Revision 4 (2026-10-07): third independent review folded in

The reviewer recommended stopping spec revisions after this round and moving to the plan; agreed.

| Finding | Resolution |
|---|---|
| Setting a filetype in the oracle loads Neovim's ftplugins and indenters | No filetype; `commentstring` set from the Arcane grammar; fixed indent options; `gc` padding matches Neovim; word class aligned with Neovim's `iskeyword` (s6.3, s11.3) |
| Dead keys break `"a` on US-International | `WM_DEADCHAR` -> bare symbol + dead state cleared in Normal/Visual/operator-pending, via the existing SDL3 Windows message hook (s5.5) |
| Non-Latin layouts: Ctrl-letter chords unreachable, Normal mode inert | Letters match on the Latin VK letter; `vim.latinLettersInNormal` (auto) (s5.5) |
| `ReplaceFileW` silently orphans a process still writing the file; identity claim overstated | Restart Manager check + Overwrite in place / Save anyway / Cancel; claim corrected (s5.3) |
| Undo history after a non-append reload unspecified | Reload recorded as one undoable transaction (VS Code behaviour); cleared with a note only above `undoMemoryMB` (s5.3) |
| Invalid UTF-8 not defined end to end | Raw bytes kept, hex-box drawing, one cluster per invalid byte, `PCRE2_MATCH_INVALID_UTF` (s3.5, s5.1) |
| Read-only usually means VCS-locked (Perforce) | D14: VCS-aware banner over git/Git LFS, Perforce, Lore, Unity VCS; integration is a future spec, git first then Lore (s5.3, s1.2) |
| Nits: AltGr by timing, control characters, `\ze` in groups, `\K` in lookarounds, hard links | Layout-queried AltGr; 0x00-0x1F dropped (except Tab/Enter); `\zs`/`\ze` top level only; hard links a known limitation (s5.5, s6.5, s5.3) |

---

## 16. Revision 5 (2026-10-07): user-chosen openers (D15)

The user asked that Arcane never force tooling: people choose whether `.cpp`/`.hpp` open in the Arcane Text Editor or Visual Studio, and likewise whether any text opens in Arcane, the system default, or another program. D1's C++ exclusion is lifted (C++ language services remain a non-goal). New s5.6 defines the `FileOpener` service, the four opener kinds, detected presets, the `editor.files.openWith` / `editor.files.customEditors` settings and Preferences page, the Open With menu, and failure handling; s4.7 adds C and C++ grammars.

---

## 17. Revision 6 (2026-10-07): plan reconciliation

Writing the implementation plan (`docs/superpowers/plans/2026-10-07-text-editor.md`) checked every section against the code it has to touch; the plan's reconciliation produced the amendments below. Items 1-32 are the plan's consolidated spec amendments; items 33-45 are further rulings recorded during reconciliation. Each names the section changed and, where the plan names one, the owning task or chunk. None changes a decision; they correct facts, close gaps and name known limits.

1. **D1 (s1.1):** the plan skeleton's "never opens C/C++" constraint is superseded by D15; `FileOpener` decides and C/C++ default to Visual Studio. (TE7)
2. **s3.1, s4.7, s10:** `TextSummary` also caches `firstLineBytes` (to combine `longestLineBytes` across a join); attribution lives in `NOTICE.md`, not `ThirdParty/NOTICES` (wording fixed in all three sections). (TE1)
3. **s3.2:** anchors live in an unsorted slot vector with stable ids per owner (was "sorted vectors"). (TE1)
4. **s3.3:** undo grouping merges only transactions of the same kind (typing with typing, deleting with deleting); `TextBuffer::ClearHistory()` exists for Reopen with Encoding. (TE1)
5. **s3.5 (encodings):** Reopen with Encoding replaces the buffer contents and clears undo history; a UTF-16 lone surrogate makes the file read-only (`InvalidUtf16`), kept as WTF-8 bytes. (TE1; TE2-15 tests the history rule)
6. **s3.5 (append):** an old file ending in a lone `\r` whose append starts with `\n` is a full reload; appends apply on the first poll that sees them (complete tail only); rewrites, truncations and deletions are debounced over two equal polls. (TE2-17)
7. **s3.5 / s5.3 (binary):** an explicit text extension with NUL bytes opens read-only; an unknown extension with NUL bytes is refused. (TE2)
8. **s4.1 (detection):** order is now remembered per-extension choice -> `files` -> `firstLine` -> Plain Text, so the picker can override a claimed extension. (TE3)
9. **s4.1 (plugins):** plugin grammars load from each active plugin root's `Syntaxes/` folder; `RegisterPluginGrammar` is the seam for a future editor-plugin API. (TE3)
10. **s4.1 (format):** optional top-level `"notice"` string; `brackets` entries are single ASCII characters; no `end`-to-`begin` back-references and no `include`/`repository` (known limits: Lua long brackets beyond level 2, heredocs, YAML block scalars). (TE3)
11. **s4.2:** the depth limit protects the interpreter path only; the JIT path is bounded by the match limit plus the JIT stack size. (TE3)
12. **s4.4:** the edit log keeps the newest 4096 batches; a consumer older than that invalidates everything (`EditLogCovers`). (TE1, consumed by TE3)
13. **s4.5:** theme apply writes `editor.theme.syntax.rules`; bold/italic are carried but draw regular until more JetBrains Mono faces ship. (TE3)
14. **s4.7:** `.h` is C++; C++ raw string literals are not recognised. (TE7)
15. **s5.1 (IME):** v1 positions the OS composition window at the primary caret; inline (underlined) composition is post-v1 (needs process-wide `SDL_HINT_IME_IMPLEMENTED_UI`). (TE2)
16. **s5.1 / s1.2 (non-BMP):** with 16-bit `ImWchar`, characters outside the BMP cannot be typed in v1 (dropped with a one-time warning); `IMGUI_USE_WCHAR32` is an owed user decision. New s1.2 bullet, shared with item 45. (TE2)
17. **s5.1 (grapheme subset):** Hangul jamo sequences (GB6-8), Prepend (GB9b) and Indic conjunct breaks (GB9c) are not covered. (TE1)
18. **s5.3 (registration):** "C++ extensions stay routed to IdeLaunch" -> "the fallback factory claims text files; whether a file reaches it is `FileOpener`'s decision (s5.6)". (TE7-9)
19. **s5.3 (overwrite in place):** offered only when it can work (the holder allows write sharing); `ReplaceFileW` on a no-delete-share target may report 32, 1175 or 5, all treated as a sharing violation. (TE2-7)
20. **s5.3 (crash safety):** the crash-window autosave/recovery path does not exist yet; text recovery is its own slice in the crash-window s8 layout under `Saved/Autosaves/Text/` (marker `RestoreData.json`) for that arc's plan 3 to fold in; recovered text takes the dominant line ending and the offer says so. (TE6-13..TE6-15)
21. **s5.3 / D14 (VCS):** Lore never sets files read-only; Unity Version Control only via an opt-in setting; Perforce detection is approximate (`P4CONFIG`, `.p4config` by convention); Lore SWFS workspaces cannot be detected by walking up. D14's row gained a short note too. (TE6-21, TE6-22)
22. **s5.4:** "global `CommandStack` entries targeting that asset are pruned" -> "the reload re-mints the document's undo anchor, `CommandStack::PruneExpired()` removes the expired steps eagerly, and `InspectorHost::InvalidateSource` drops the Inspector history"; the existing "Open as text" (OS editor) entries are re-pointed. (TE6-6..TE6-12)
23. **s5.6 (Visual Studio row):** a running instance jumps with `TextSelection.MoveToLineAndOffset` (line and column), falling back to `Edit.GoTo`; a new instance gets the line only. (TE7)
24. **s5.6 (rules):** an explicit Open With beats "already open"; `dedicated` is overridable per extension or via the `asset` key; the Preferences path is **Editor > Files > Open With**; `openWith` is a `key=opener;...` string and `customEditors` a JSON list (no map/list cvar type); a system-default open with no association produces the Problems row and the offer. (TE7)
25. **s5.6 (menus):** the Console has no file rows today (Open With there is N/A until it does); the Inspector's Open With is a right-click on the Open icon; Windows Terminal's `;` breaks the `nvim` preset for paths containing `;`. (TE7)
26. **s6.4:** shift width is `editor.text.tabSize` (`:set sw` and `:set ts` move together); persisted vim state follows Neovim's `shada` caps (50 lines / 10 KB per register, 10000 history entries). (TE5)
27. **s6.6 / s11.3:** the divergence list (`VimDivergences()`) is the vim help page's list, including "an opened fold is forgotten". New s6.6 bullet; s11.3 points to it. (TE5)
28. **s7:** `text.toggleFollow` and `text.convertLineEndings` ship unbound; column select (Ctrl+Shift+Alt+arrows), expand selection to brackets (Ctrl+Shift+M) and zoom (Ctrl+= / Ctrl+- / Ctrl+0) use VS Code/Sublime chords; split view has no row. (TE4; TE6-4 owns `text.toggleFollow`)
29. **s8:** added `defaultLineEnding` (`PreferencesProject`), `recentWriteSeconds`, `watchPollSeconds`, `bracketScanMaxLines`, `detectIndentationRows`, `followPollSeconds`, `followChunkKB`, `recoveryIntervalSeconds`, `viewStateMaxFiles`, plus `editor.theme.syntax.rules` (s4.5) and `editor.files.*` (s5.6). (TE1, TE2, TE3, TE4, TE6, TE7)
30. **s9:** "`DocumentHost::OpenAt(path, Point)` ... C++ still goes to IdeLaunch" -> "`FileOpener::Open/OpenWith(path, opener, FilePosition{line, column})` (1-based, column in characters) is the open hook for spec 2; `DocumentHost::OpenAt` is internal". (TE7)
31. **s11.4:** the Windows-1252 scenario is an in-process `[text-doc]` case (TE2-15); vim goldens are blessed with the Experimental badge and re-blessed by VIM-GATE. (TE2-15, TE6 goldens, VIM-GATE)
32. **s11.5:** baselines are a `perf[]` table (per configuration) in `scripts/automation-baselines.json`, checked by `check-baselines.ps1 -PerfReportPath`. (TE6-18)
33. **s3.3:** an edit batch's ops have strictly increasing begin offsets and do not overlap; an invalid batch asserts in Debug and is refused (no change) in Release. (TE1)
34. **s3.5:** the binary (NUL) sniff applies to the UTF-8 path only; UTF-16 files are not sniffed; a NUL in an appended UTF-8 tail makes the document read-only (`Binary`) instead of appending; an odd-byte UTF-16 tail fragment is held back until complete; an appended tail with invalid UTF-16 makes the document read-only (`InvalidUtf16`). Placed in the "Invalid bytes / binary" and "Append-only growth" rows. (TE1 `DecodeFragment`, TE2-17)
35. **s3.5 / s5.3:** the undo policy (`undoGroupMs`, `undoMemoryMB`) is applied when a document opens, not only on the first reload. s3.5 has no undo-policy text, so the sentence was added to s5.3's **Reading** bullet only. (TE2-15)
36. **s4.4:** the highlight cache keeps a cache epoch beside `grammarVersion`; a grammar reload or an edit-log-overflow invalidation bumps it. (TE3)
37. **s5.2 (find):** known limits -- a cross-window match on an over-long line is cut at the window; a backward find scans from the document start on the worker; the find bar shows `TextSearch`'s limit message when a regex hits its limits. (TE4)
38. **s9:** symbols are not extracted for documents above `syntaxMaxMB`. (TE4)
39. **s3.2 / s7 (keyboard column select):** rows shorter than the column are skipped. s3.2 does not describe column select, so the statement went into s7's column-select entry only. (TE4)
40. **s8:** `autoClosePairs` defaults to VS Code's default pairs. (TE4)
41. **s6.5:** vim search on an over-long row copies the whole row (no window). (TE5)
42. **s5.1 / D8 (split):** closing the primary view while split moves the secondary into it; vim manual folds come back as indentation folds (lossy known limit). Added to s5.1's split-view bullet; the D8 row is unchanged. (TE4-17)
43. **s7 (item 2, text-only commands):** the `TextDocument` action context is exempt from the typing gate; the text area sets `ImGuiWindowFlags_NoNavInputs`; the pre-existing settings defect `ParseKeyChord("Ctrl+[")` is fixed by the plan. s7 has no numbered subsection 7.2, so this went into its numbered item 2. (TE4-1)
44. **s5.5 / s6:** `Insert` is a named text key, so vim's `<Insert>` (Insert/Replace toggle) works; it is not a divergence. Placed in s5.5's character/named-key paragraph. (TE2-10)
45. **s1.2:** `IMGUI_USE_WCHAR32` (typing non-BMP characters) is out of scope for v1 and an owed follow-up that may need an engine ABI bump; complements item 16 in the same new non-goal bullet. (TE2; owed user decision)

---

## 18. Revision 7 (2026-10-07): external plan review

An external review of the implementation plan (`docs/superpowers/plans/2026-10-07-text-editor.md` at commit `2b61addd`) was checked claim by claim against the plan, the code and Neovim; the plan's revision 2 records the binding decisions (D-R1..D-R12). The items below change this spec. Finding ids are the review's (X = cross-cutting, 2.x TE2, 3.x TE3, 4.x TE4, 5x.y TE5, 6.x TE6, 7.x TE7). None changes a decision D1-D15; D15 gains one restriction (item 10).

1. **s5.5, s5.1 (one ordered input stream; D-R1; findings 2.3, 5a.17):** keys and characters reach the view and vim as one stream in typed order, each carrying a sequence number; outside command-key mode the order is ImGui's input-queue order, in command-key mode the message hook owns the whole stream. (TE2-10; TE5a-22)
2. **s5.5 (layout polling; D-R1; finding 2.1):** `WM_INPUTLANGCHANGE` / `WM_SETFOCUS` are sent messages that SDL's hook never sees, so the active layout is polled every frame (`GetKeyboardLayout(0)`) and re-queried on change. (TE2-10)
3. **s5.1, s5.5 (IME off before the next key, every frame; D-R1; finding 2.2):** the input context is disassociated when a vim command mode is entered, before the next key, and re-applied every frame after ImGui's platform IME callback (SDL's text-input start re-associates it), falling back to `GetFocus()`; the test is key-level through the real message queue. (TE2-10; TE5a-22)
4. **s5.5 (dead keys while a command waits for a literal; D-R1 as refined by D-R15; finding 5a.20):** command-key mode is off while `f`, `t`, `F`, `T` or `r` wait for their character (a pending mark or register NAME keeps it on -- corrected at the reconcile, D-R15), so `f^e` and `rê` work on French layouts. s11.3's keyboard tests add typed order and a real pending dead key (finding 2.4). (TE5a-22; TE2-10)
5. **s4.3 (resumable state; D-R3; finding 3.2):** a resumed line carries the rules disabled by a limit hit and the zero-width counter, so it tokenizes exactly as an unbudgeted line. (TE3-11)
6. **s4.4 (a partial row is not ready; D-R3; finding 3.1):** a budget-cut row is Partial, never Ready; the frontier never passes it; a completed result replaces it at the frontier; catch-up and the worker resume it first, the last row included. (TE3-14, TE3-16)
7. **s4.4 (limits change; D-R3; finding 3.4):** changing `regexMatchLimit` / `regexDepthLimit` resets the highlight cache. (TE3-14)
8. **s11.2 (incremental == full; D-R3; finding 3.3):** the property runs with a fake clock, randomised budgets and an `(a+)+$` rule under a small match limit, and compares Partial rows. (TE3-17)
9. **s5.2, s3.2 (find results mapped forward; D-R4; finding 4.3):** worker results apply through the edit-log mapping the highlight cache uses; overlapped matches dropped, the count kept (a line-path count is exact after the worker's top-up; a carried multi-line count shows `~` -- corrected at the reconcile, F4); a Follow append tops up only the tail; "..." only before the first result. (TE4-10)
10. **s1.1 D15, s5.6, s8 (project settings never name a program; D-R6; finding 7.2, security):** `editor.files.customEditors` is Preferences only and carries `CVarFlags::LaunchesProgram`, so the settings registry refuses it from every rung inside a project (settings S7-SEC; added at the reconcile); a Project-rung `editor.files.openWith` value may name only `arcaneText`, `visualStudio`, `systemDefault`, `dedicated` (custom and preset ids refused with a Problems row); the batch-file refusal strips trailing dots and spaces (`x.cmd.`); the no-bypass rule is enforced by access first, by a source scan second (finding 7.8). (TE7-3, TE7-10)
11. **s6.1 (final newline; D-R5; finding 5c.4):** a terminator at the end of the text ends the last line; vim's line count and addressing change, the rope and saving do not; the view's trailing empty row is never a Normal-mode cursor position. s11.3: every case group gets a `\n`-terminated input. (TE5a-2; TE5c-3/10/11)
12. **s6.2 (beep rule; D-R12; finding 5a.2):** a failed command flushes only non-typed input (macro, `:normal`, remap queue); typed keys after it still run. (TE5a-5; the 8 beep cases renamed)
13. **s6.4 (Neovim-confirmed behaviours; D-R12; findings 5a.13, 5a.24, oracle run):** linewise `Vd` / `vX` / `>>` keep the column (`nostartofline`); linewise `gu` / `g~` / `vY` go to column 0 and `gcc` to the start of the comment leader; block `A` / `$A` return to the block start; a recording leaves the unnamed register; `di(` outside parentheses seeks forward; a same-line `d%` fills `"1` with Neovim's built-in `%` (the earlier "does not fill" was a matchit artifact -- corrected at the reconcile, D-R13); Insert `Ctrl-R` inserts literally, without autoindent. (TE5a, TE5b)
14. **s11.3 (oracle; D-R12; findings 5a.1, 5a.3, 5a.4, 5a.8):** one Neovim process per case with `-u NONE -i NONE -n` and `filetype off` (corrected at the reconcile, D-R13: `--clean --noplugin` still loads matchit); the end state is read from a timer, not `nvim_feedkeys(..., 'tx!')`; case-sensitive comparisons; `comments` set from the grammar beside `commentstring`; `iskeyword` = Neovim's default plus the grammar's extra ASCII word characters; `=` cases that equal Neovim carry no divergence note. (TE5a-7)
15. **s11.3 (pin; D-R12):** the pin moves to Neovim v0.12.5 (2026-08-23), installed only from the asset with SHA-256 `de8625ba8cf65ebf40eb80a388ba1ec8e9c15b30218821e2c639119b05920de1`; the plan's 562 cases were run against it, one process per case, and its tables corrected to the results. (TE5a-7; every TE5 table)
16. **s5.3 (recovery is session-scoped; D-R9; finding 6.11):** snapshot files are `<sessionId>-<docKey>.arcrecover`; a clean shutdown deletes only its own session's files; a crashed session's files live until the offer is answered (Recover / Discard delete them, Not now keeps them); no sweep while an offer is pending. s11.4 adds the scenario. (TE6-13, TE6-15)
17. **s3.5, s5.3, s8 (Follow under continuous writes; D-R10; findings 2.8, 2.9, 6.9):** growth settles at once; a rewrite, rotation or truncation settles when two consecutive polls agree on the bytes up to the old end's hash, or at worst after the new `editor.text.follow.maxDebounceMs`; the synced state advances only when the buffer equals those bytes and the dirty banner keeps its own acknowledged stamp. s11.4's follow witness writes every 100 ms and is polled more slowly. (TE2-17; TE6-5)
18. **s5.3 (Perforce detection; finding 6.15):** besides a `P4CONFIG` / `.p4config` file, `VcsProbe` reads `P4CLIENT` / `P4PORT` from the environment and from the `p4 set` registry values (`HKCU`, then `HKLM`, `Software\Perforce\environment`, p4's own spelling). (TE6-21)

**Plan revision 2 reconcile (2026-10-07).** The controller's reconcile of the revision-2 fixers' work (queue items in brackets) changed this spec further:

19. **s3.5 (odd UTF-16 body; [T3]):** a UTF-16 file with an odd byte count opens editable in its encoding; the stray byte is held for the next append and is not written by a save. (TE1-11, TE2-15, TE2-17)
20. **s3.5 (CR + empty LF row; [T3]):** an empty LF row right after a lone-CR row saves as `\r` + `\r\n`, so no line is lost. (TE1-11)
21. **s5.3 (reload undo entry; [T2], [T3]):** a reload clears the history when its undo entry -- old AND new text -- exceeds `undoMemoryMB`, not when the old text alone does. (TE1-10, TE2-17)
22. **s4.4 (the free resolve pass; [S7]):** the integer-compare pass stops at a Partial row; the bounded catch-up or the worker resumes it. (TE3-14)
23. **s4.7 (C/C++ strings; [P4], finding 7.14):** C and C++ strings end at the end of the line; C++ raw strings are recognised (`R"(...)"`; `R"d(...)d"` ends at the first `)d"`). Supersedes s17 item 14's "raw string literals are not recognised". (TE7-16, TE7-17)
24. **s5.2 (find count; [F4]):** a line-path count is exact after the worker's top-up; only a carried multi-line count shows `~`. Refines item 9. (TE4-10)
25. **s5.3 (Perforce registry spelling; [H7]):** the `p4 set` keys are `Software\Perforce\environment` under `HKCU` and `HKLM` (case-insensitive), as TE6-21 reads them. (TE6-21)
26. **s5.3 (one recovery marker; [G7]):** session-scoped snapshot files share one `RestoreData.json` whose entries carry their `sessionId`; `TextRecovery` is its only writer. (TE6-13)
27. **s5.5 (chords in the stream; [A12]):** Ctrl/Alt chords ride the ordered stream as key events named by their key, so vim sees a chord between two typed keys in order; the plain view leaves chords to the action table. (TE2-10, TE5a-22)
28. **s5.5 (when the IME is re-disabled; [E8], [H2]):** twice a frame -- before the message pump (ImGui calls its platform IME callback in `EndFrame`) and at the view's input step; the key-level test posts only the key-down. (TE2-10)
29. **s5.5 (literal vs name arguments; D-R15):** only a pending literal argument (`f t F T r`) turns command-key mode off; mark and register names (Insert and command-line `Ctrl-R` included) keep it on; Insert `Ctrl-K` digraphs are outside v1. Corrects item 4. (TE5a-4, TE5a-22, TE5c-5)
30. **s5.6, s8, s1.1 D15 (programs and the registry; [SCOPE-ENFORCEMENT], [H4], [P6]):** `editor.files.customEditors` carries `CVarFlags::LaunchesProgram` (settings S7-SEC, on `main` once the settings arc merges): the registry refuses it from the EngineConfig, Plugin, Project and User rungs and reports a config file that tries; the opener service's reader keeps the same rule as defence in depth; a Plugin rung is treated like Project, and the User rung (a project's committed `Saved/Config`) cannot name programs either. Refines item 10. (TE7-3)
31. **s6.1 (OutsideChange event; [A9], [A10]):** the document notifies before it changes the buffer outside a view's input (save transforms, reload, Follow append, Reopen with Encoding, convert line endings) through an empty-safe hook TE5a-22 subscribes to, so vim ends an open Insert transaction first. (TE2-15/16/17, TE4-19, TE5a-22)
32. **s11.3 (vim word classes; [A9]):** vim's word class ports Neovim's `utf_class` above U+00FF and uses Latin-1 `iskeyword`; the editor's whole-word search keeps `\p{L}\p{N}`. (TE5a-3)
33. **s11.3 (oracle without matchit; D-R13):** the oracle runs `nvim --headless -u NONE -i NONE -n` with `filetype off`; a one-line `d%` fills `"1`. Corrects items 13 and 14. (TE5a-7, TE5a-10)
34. **s11.3, s6.1 (case strings are Vim lines; D-R14):** `in:` / `out:` are Vim lines joined by `\n`; the rope gets a final `\n` unless `eol: off`; the final-newline cases come in pairs. Refines item 11. (TE5a-2, TE5a-6, TE5a-7)
35. **s6.6 (macro key text; [G5]):** the divergence list names "macro key text": a macro register holds Arcane key notation, not raw bytes. (TE5c-16)

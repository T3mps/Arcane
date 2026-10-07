# Arcane Text Editor -- design

**Status:** Proposed (brainstorm 2026-10-07; sections 1-6 approved in conversation; awaiting written-spec review)
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
| D11 | Regex engine: **PCRE2 with JIT**, vendored; the one regex flavour for grammars, find/replace and vim. |

### 1.2 Non-goals

- C++ editing, debugging, IntelliSense/LSP, build-error squiggles from the compiler.
- Vimscript, vim plugins, `:!` shell filters, `:terminal`, digraphs, spell check. (Digraphs and an opt-in `:!` that confirms before running are cheap later additions if wanted.)
- Loading other editors' grammar/theme/config files (D5).
- A hex/binary viewer. Binary files are refused with a message.
- Minimap, soft wrap, word completion, diff view: later steps, not v1 (s12.2).
- Collaborative editing. (Zed's CRDT machinery -- Lamport clocks, fragment/locator anchors, version vectors -- is deliberately not reproduced.)

---

## 2. Architecture

All code lives under `ArcaneEditor/src/TextEditor/`. Four layers; each depends only on the layers above it.

| Layer | Units | Depends on |
|---|---|---|
| **Core** (pure C++, no ImGui) | `Rope`, `TextBuffer`, `SelectionSet`, `TextCommands`, `FoldModel`, `TextSearch` | nothing (PCRE2 for `TextSearch`) |
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

- A balanced B+ tree of UTF-8 leaves of **64-128 bytes**, ported from Zed's `sum_tree` (Apache-2.0; NOTICE + attribution kept, changes marked). Zed's `rope`/`text` crates are GPL and are **not** ported -- design reference only.
- Each node caches a summary: `{bytes, newlines, lastLineBytes, longestLineBytes}` (plus `chars` if a consumer needs it). Byte offset <-> `Point{row, byteCol}` is O(log n) by seeking on one dimension while accumulating the other; no line-start table is rebuilt per edit.
- **Persistent:** an edit copies only the touched path (refcounted nodes). A `RopeSnapshot` is O(1) to take and immutable, so the highlighter worker and large-file search read a consistent snapshot while the user types.

### 3.2 Positions and anchors

- Internal positions are byte offsets, or `Point{row, byteCol}`. Display columns (tab width, wide glyphs) are computed only by the view.
- Anything that must survive edits -- selections, vim marks, folds, search matches, bookmarks -- is an **anchor**: `{offset, bias}`, shifted by every edit (left bias stays before an insertion at its offset; right bias moves after). Anchors live in sorted vectors per owner; shifting is O(anchors) per edit, which is fine at editor scale.

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
| Line endings | Detected on load (LF / CRLF / mixed); stored internally as LF; saved in the file's dominant style. Mixed files show a status-strip note and are normalised on save. |
| Encodings | UTF-8 (BOM preserved if present) and UTF-16 LE/BE with BOM are decoded and saved back in the same encoding. |
| Invalid bytes / binary | Invalid UTF-8 or binary content (NUL bytes in the first 8 KB) opens **read-only** with a banner explaining why; saving can never corrupt the file. Pure binary formats with no text editor are refused with a message. |
| Size thresholds | Above `editor.text.syntaxMaxMB` (default 32) highlighting is off. Above `editor.text.openMaxMB` (default 512) the file is not opened ("open externally" notice). |
| Append-only growth | If an open file on disk only grew (prefix unchanged), the new tail is appended instead of reloading. A **Follow** toggle (status strip) keeps the view at the end, like `tail -f`. |

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
  "patterns": [
    { "begin": "\"", "end": "\"", "escape": "\\\\.", "scope": "string.quoted.double.json",
      "when": { "followedBy": "\\s*:", "scope": "entity.name.key.json" } },
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

Detection order: file extension (`files`) -> `firstLine` regex -> the user's choice in the status-strip language picker, remembered per extension in `editor.text.languageByExtension`.

The format is validated on load; a bad grammar produces a Problems row naming the file and rule, and the language falls back to Plain Text.

### 4.2 Regex engine: PCRE2 + JIT

Vendored under `ThirdParty/pcre2` (BSD), built as a static lib via premake with JIT enabled, 8-bit code units. One engine for grammar rules, in-file find/replace, vim `/` `?` `:s` `:g`. `std::regex` is too slow; RE2 would bring abseil.

### 4.3 Tokenizer

- **TextMate matching:** at the current position, among the active rule list, the **earliest** match wins; ties go to the first rule listed. Each rule's next match on the current line is cached, so a line is scanned about once rather than once per rule.
- **State:** the stack of open regions at a line boundary, interned to a small `StateId` (equal stacks share an id; equality is an integer compare).
- **Per-line cache:** `{startState, endState, runs[]}`, where a run is `{byteStart, scopeId}` (run-length, never per glyph -- ImGuiColorTextEdit's ~12 bytes/char model is what rules it out for large logs). Scopes are interned dotted strings.
- **A per-line time budget** (`editor.text.tokenizeLineBudgetUs`) yields a partial line that resumes next frame, so a pathological line never stalls the UI.

### 4.4 Scheduling

- Visible lines are tokenized synchronously when drawn (cache miss).
- A worker thread advances on a `RopeSnapshot`, ahead of the view up to the visible end plus a margin, within `editor.text.highlightFrameBudgetMs`.
- After an edit, invalidation starts at the edited line and **stops as soon as a line's recomputed end state equals its cached end state** (convergence). Typing inside a string usually re-tokenizes one line.
- Files above the syntax threshold are not tokenized; files below it are tokenized lazily only as far as the user has scrolled.

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
- **Split view (D8):** a second `TextEditorView` over the same `TextBuffer`, with its own scroll, folds-visible state and `SelectionSet`, docked beside the first. Edits in one view shift the other view's anchors through the buffer's edit notifications.

### 5.2 Built-in controls

- **Find bar** (Ctrl+F; Ctrl+H for replace), docked at the top of the document: regex, case, whole word, in-selection toggles; "3 of 17" count; Enter/Shift+Enter next/previous; **Alt+Enter turns every match into a cursor**; Replace All is one undo step; large files are searched on the worker against a snapshot.
- **Go to line** (Ctrl+G): a small popup taking `line` or `line:col`. Spec 2's Goto Anything `:` supersedes it.
- **Status strip** (document footer): Ln/Col, selection count, encoding, line endings, indentation ("Spaces: 4") -- each clickable to change -- language (opens the picker), vim mode + pending keys + "recording @q", Follow toggle for growing files, read-only badge.

### 5.3 `TextDocument`

- An `EditorDocument` (and `InspectorSource`). Identity: the asset GUID when the file is a registered asset; otherwise the **normalized absolute path**. `DocumentHost` gains a path-keyed lookup for GUID-less documents so a second open focuses the existing tab.
- **Registration:** `DocumentHost` gets a **fallback factory** used when no extension factory claims a path and the file sniffs as text; C++ extensions stay routed to `IdeLaunch`. Text extensions with no dedicated editor (`.md .ini .log .hlsl .lua .toml .yaml .xml .txt ...`) register this factory explicitly.
- **Save:** atomic (write temp beside the file, flush, rename over), preserving encoding, BOM and line endings. `editor.text.ensureFinalNewline` and `editor.text.trimTrailingWhitespace`, both off by default.
- **External changes** (file watcher, debounced):
  - clean document -> silent reload keeping cursors and scroll (positions mapped by line/col);
  - dirty document -> banner: **Reload** / **Keep mine**;
  - append-only growth -> append (s3.5);
  - deleted -> banner; Save recreates the file.
- **Crash safety:** unsaved text documents join the editor's existing autosave/recovery path (crash-window arc): buffers are snapshotted to the recovery store and offered back after a crash.
- **Session restore:** cursor, scroll, folds and split state per file, stored in user data (`editor.text.rememberViewState`).
- **Inspector page** (`TextDocumentInspectorPage`): path, size, encoding, language, line endings, indentation, read-only reason -- editable where meaningful (language, encoding, line endings, indentation). This keeps Arcane's one-Inspector model; the document draws no properties block of its own.

### 5.4 Open as Text on assets with a dedicated editor (D10)

- Asset context menu and Inspector gain **Open as Text** for any text-backed asset.
- If the asset has a dedicated editor, the text document opens **read-only** with a banner and an **Enable editing** button.
- After unlocking, Save writes the file and triggers the registry's reload of that asset; an open dedicated editor for the same asset refreshes, or -- if it has unsaved changes -- shows its own conflict banner instead of silently losing either side.

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
| Insert mode | `Ctrl+W Ctrl+U Ctrl+R{reg} Ctrl+O Ctrl+T Ctrl+D`, `Esc` / `Ctrl+[` |
| Ex | ranges `% . $ N 'a /pat/ ?pat? +n -n '<,'>`; `:w :q :wq :x :q! :e :sav :r`, `:s` with `g i c` (`c` prompts per match) and `&`, `:g` / `:v`, `:normal`, `:d :y :m :t :co :> :< :j :sort` (with `n u i r` options), `:noh`, `:set`, `:map` family (`nmap vmap imap omap` + `noremap` forms), `:sp :vs :bn :bp :ls`, `:marks :reg :jumps` (popup listings) |

### 6.4 Options, remaps, persistence

- Options are Arcane settings under `editor.text.vim.*`: `ignorecase`, `smartcase`, `hlsearch`, `incsearch`, `wrapscan`, `gdefault`, `timeoutlen`, `scrolloff`, `startofline`, `leader`, `handleCtrlKeys` (default on while vim is on), `useSystemClipboard`.
- Tab width, spaces/tabs, shift width and relative numbers are **editor-wide** text settings (`editor.text.*`), not vim-only.
- `:set` changes a value for the **session only**; the Preferences page changes it permanently.
- Remaps: a table on a **Vim** Preferences page (`editor.text.vim.remaps`: `{mode, from, to, recursive}`); `:map` adds a session-only remap. Remaps resolve through a key trie with `timeoutlen` for ambiguous prefixes.
- Marks (`A-Z` and per-file), registers, and search/command history persist in user data across sessions (`editor.text.vim.persistState`).

---

## 7. Shortcuts: unified with the action table (D6)

The editor already has one action table (`Input/EditorActionTable.hpp`) with contexts ranked by specificity (Global < Document < panels < Text) and `PressedInWindow` for a focused window answering a Global action itself.

1. **Shared verbs reuse existing action ids.** `edit.undo`, `edit.redo`, `edit.redoAlt`, `edit.cut`, `edit.copy`, `edit.paste`, `document.save`, `document.close` are answered by the focused text document via `PressedInWindow` (the Settings window's local-undo precedent). Rebinding Undo rebinds it everywhere. Each text document owns its history: Ctrl+Z in a text file never touches the scene's `CommandStack`.
2. **Text-only commands are new rows** in the same table under a new `ActionContext::TextDocument` (specificity above Document, alongside panels). Initial set (ids `text.*`): word left/right (+select), line start/end, document start/end, page up/down, add next match (Ctrl+D), select all matches (Alt+F3), split selection into lines (Ctrl+Shift+L), add cursor above/below (Ctrl+Alt+Up/Down), column select by keyboard, select line (Ctrl+L), expand selection to brackets, duplicate line (Ctrl+Shift+D), delete line (Ctrl+Shift+K), move line up/down (Alt+Up/Down), indent/outdent (Tab / Shift+Tab, Ctrl+] / Ctrl+[), toggle line comment (Ctrl+/), toggle block comment (Ctrl+Shift+/), join lines (Ctrl+J), find (Ctrl+F), replace (Ctrl+H), find next/previous (F3 / Shift+F3), go to line (Ctrl+G), fold / unfold (Ctrl+Shift+[ / ]), go to matching bracket (Ctrl+M), toggle bookmark (Ctrl+F2), next/previous bookmark (F2 / Shift+F2), zoom in/out/reset, toggle follow. They appear on the Shortcuts page, are rebindable, get conflict detection, and are listed by spec 2's palette for free.
3. **Ctrl+D** in a text document is add-next-match (Sublime); the Global `edit.duplicate` is shadowed there by context specificity, exactly as `graph.duplicate` shadows it in the graph today. Duplicate line is Ctrl+Shift+D.
4. **Vim keys are not chords** and live in vim's own sequence keymap (s6.4); vim's commands still call `TextCommands`, so `u` and Ctrl+Z share one history. With `handleCtrlKeys` on, vim claims Ctrl+V (visual block), Ctrl+R (redo), Ctrl+D/U/F/B/E/Y/O/I/A/X/W in their vim meanings; with it off, those chords keep their action-table meanings.

---

## 8. Settings

All under `editor.text.*` (Preferences scope unless noted), registered through the settings arc's reflection (`ARC_REFLECT_TYPE_ATTR(Settings, ...)`) so they appear on the Preferences window, the sweep tests and the inventory:

`font`, `fontSize`, `tabSize`, `insertSpaces`, `detectIndentation`, `lineNumbers`, `rulers`, `renderWhitespace`, `indentGuides`, `caretBlink`, `highlightCurrentLine`, `undoGroupMs`, `undoMemoryMB`, `syntaxMaxMB`, `openMaxMB`, `tokenizeLineBudgetUs`, `highlightFrameBudgetMs`, `ensureFinalNewline`, `trimTrailingWhitespace`, `languageByExtension`, `rememberViewState`, `autoClosePairs`, `autoIndent`, and the `vim.*` group from s6.4. Project scope may override indentation and line-ending defaults (`PreferencesProject`).

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
| Zed `sum_tree` | Apache-2.0 | Ported into `Rope` (NOTICE + attribution, changes marked) |
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
- **Selections:** merge rules, anchor bias on edits at boundaries.
- **`TextCommands` tables:** every command with one and several cursors: `{start text with cursor markers, command, expected text with cursor markers}`.
- Encoding/line-ending round trips: every supported encoding and EOL style loads and saves byte-identical when unedited.

### 11.2 Syntax
- **Grammar scope tests:** each language ships sample files with comment-annotated expectations (`// ^^^^ constant.numeric.json`), run by ArcaneTests. A grammar cannot regress silently.
- **Incremental == full:** property test that after random edits, the incremental highlight cache equals a full re-tokenize.
- Grammar validation: malformed grammars produce a Problems row and fall back to Plain Text.

### 11.3 Vim
- Table-driven cases `{start text + cursor, keys, expected text + cursor, expected mode/register state}`, hundreds of them, pure C++.
- **Neovim oracle (D9):** `scripts/vim-oracle.ps1` runs each case through `nvim --headless --clean` and writes or verifies the expected columns. Dev-time only; tests never require Neovim. Cases where Arcane deliberately differs carry an explicit `divergence` note.

### 11.4 View and document
- **Goldens:** the headless editor opens sample files (JSON, HLSL, Markdown, log; vim normal/visual; find bar open; split view) and screenshots them -- re-blessed through the established golden procedure.
- **Witness scenarios:** open a log and follow it while it grows; external change with and without unsaved edits; Open as Text read-only -> unlock -> save -> asset reload; crash recovery restores an unsaved buffer.

### 11.5 Performance budgets (`[perf]`, baselined)
- Open a 100 MB log (no syntax): time to first frame.
- Keystroke: edit + retokenize + layout under 1 ms on a 10k-line JSON file.
- Scroll: frame cost at 60 lines/frame on a large file.
- Highlight-on-open of a 5 MB file to the visible range.

Baselines live with the existing automation baselines; regressions fail the suite.

---

## 12. Delivery

### 12.1 Order (each step ends green; the plan breaks them into tasks)

1. **Core:** `Rope`, `TextBuffer` + undo, `SelectionSet`, `TextCommands`, encodings/EOL. No UI; fully tested.
2. **First visible editor:** `TextDocument`, basic `TextEditorView` (draw, edit, multi-cursor, mouse, save, external change), fallback registration in `DocumentHost`, shared-verb actions. Usable from here.
3. **Syntax:** PCRE2 vendored; grammar engine; v1 grammars; `.arctheme` `syntax` block; grammar tests.
4. **Editing features:** find/replace, go to line, folding, brackets, split view, status strip, Inspector page, text-only actions in the table.
5. **Vim**, four passes: (a) modes, motions, operators, text objects; (b) registers, dot-repeat, macros, marks; (c) ex commands; (d) options, remaps, persistence, Vim Preferences page. Oracle tables grow with each pass.
6. **Polish + gate:** log follow, crash recovery, session restore, Open as Text, perf budgets, goldens, witness scenarios; gate.

Then spec 2 (palette, Goto Anything, Find in Files).

### 12.2 After v1 (not in this spec)
Minimap; soft wrap (likely first, for Markdown and logs); word completion; diff view; tree-sitter for structural folding/outline/syntax-aware text objects behind the scope interface; shader editor snippet fields and crash log tail adopting the core; digraphs; opt-in confirmed `:!`.

# Arcane Hub Slices 2+3: the Hub app — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An installed, per-user Arcane Hub that registers engine builds, lists and
opens projects, and creates new ones with a probed (never hardcoded) plugin ABI.

**Architecture:** A Tauri 2 + Rust + SvelteKit desktop app at `Arcane/Hub/`, modelled
on `Tools/setup-wizard/`. All decision logic lives in **pure Rust modules that
`cargo test` covers**; the Tauri command layer only does process spawning, file IO and
IPC — the same split `setup-wizard` uses (`orchestrator.rs` pure + unit-tested,
`lib.rs` does the spawning). The Hub never links the engine: it talks to
`ArcaneEditor.exe` through two flags, `--print-engine-info` and `--project`.

**Tech Stack:** Tauri 2, Rust 2021, SvelteKit 2 + Svelte 5 (adapter-static), Vite 6,
`serde`/`serde_json`, `vitest` (front-end), `cargo test` (Rust). NSIS installer.

## Scope

Slices 2 and 3 of `docs/superpowers/specs/2026-07-26-arcane-hub-launcher-design.md`.
Slice 1 (the engine seam) is **already built and merged** — `--print-engine-info` and
the no-project gate exist on `main` (`a45cfa03`). This plan consumes them.

## Global Constraints

- **Source lives at `Arcane/Hub/`.** NOT `Tools/` — that directory is slated for
  deletion. `Tools/setup-wizard/` is a read-only TEMPLATE to copy patterns from;
  never modify it.
- **Not a member of `Arcane.slnx`.** Different toolchain; its own CI job.
- The Hub is **installed per-user**, not portable. It must NEVER assume an engine
  sits next to it, and must NEVER derive a repo root from `current_exe()` — that
  is `setup-wizard`'s repo-root-portable assumption and it is wrong here.
- **An empty engine list is a normal first-run state**, not an error.
- **Never hardcode a plugin ABI.** Every `.arcproj` the Hub writes stamps the value
  returned by `--print-engine-info`. This is the whole reason slice 1 exists.
- Windows-only for now (the engine is). Use `CREATE_NO_WINDOW` (`0x0800_0000`) on
  every spawned process so no console flashes.
- ASCII-only comments, consistent with the rest of the repo.
- Rust tests run with `cargo test` from `Arcane/Hub/src-tauri/`.

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/Hub/package.json`, `vite.config.js`, `svelte.config.js`, `tsconfig.json` | Front-end scaffold (copy setup-wizard's, renamed) |
| `Arcane/Hub/src-tauri/Cargo.toml`, `build.rs`, `tauri.conf.json`, `capabilities/default.json` | Tauri scaffold + NSIS per-user bundle |
| `Arcane/Hub/src-tauri/src/paths.rs` | Where `%APPDATA%\Arcane\hub\` is. Pure + tested |
| `Arcane/Hub/src-tauri/src/state.rs` | `Recents`/`Engines` types, load/save, pure list ops. Tested |
| `Arcane/Hub/src-tauri/src/engine.rs` | `EngineInfo` + **pure** probe-output parser. Tested |
| `Arcane/Hub/src-tauri/src/project.rs` | **Pure** `.arcproj` manifest generation. Tested |
| `Arcane/Hub/src-tauri/src/lib.rs` | Tauri commands: spawning, file IO, IPC. Not unit-tested |
| `Arcane/Hub/src/routes/+page.svelte` | Projects screen |
| `Arcane/Hub/src/routes/settings/+page.svelte` | Engines screen |
| `.github/workflows/build-arcane-hub.yml` | CI (model on `build-setup-wizard.yml`) |

---

### Task 1: Scaffold, packaging, CI

**Files:** everything under `Arcane/Hub/` listed above except the `src/*.rs` logic
modules; plus `.github/workflows/build-arcane-hub.yml`.

**Interfaces:**
- Consumes: nothing.
- Produces: a buildable Tauri app. Later tasks add modules to `src-tauri/src/`.

- [ ] **Step 1: Read the template**

Read `Tools/setup-wizard/package.json`, `src-tauri/Cargo.toml`,
`src-tauri/tauri.conf.json`, `src-tauri/capabilities/default.json`,
`svelte.config.js`, `vite.config.js`, and `.github/workflows/build-setup-wizard.yml`.
Copy their structure; do not invent a different one.

- [ ] **Step 2: Create the scaffold**

Mirror the template with these deliberate differences:

`Arcane/Hub/src-tauri/tauri.conf.json`:
```json
{
  "$schema": "https://schema.tauri.app/config/2",
  "productName": "Arcane Hub",
  "version": "0.1.0",
  "identifier": "com.starworks.arcanehub",
  "build": {
    "beforeDevCommand": "npm run dev",
    "devUrl": "http://localhost:1420",
    "beforeBuildCommand": "npm run build",
    "frontendDist": "../build"
  },
  "app": {
    "windows": [
      { "title": "Arcane Hub", "width": 1000, "height": 680, "resizable": true, "center": true }
    ],
    "security": { "csp": null }
  },
  "bundle": {
    "active": true,
    "targets": ["nsis"],
    "icon": ["icons/32x32.png", "icons/128x128.png", "icons/128x128@2x.png", "icons/icon.ico"],
    "windows": { "nsis": { "installMode": "currentUser" } }
  }
}
```

`installMode: "currentUser"` is the load-bearing line: it installs to
`%LOCALAPPDATA%\Programs\Arcane Hub\` with no admin prompt, which is the decision
recorded in the spec. `resizable: true` differs from the wizard on purpose — a
project list is browsed, not stepped through.

`Cargo.toml`: same shape as the wizard's, package name `arcane-hub`, lib name
`arcane_hub_lib`. Dependencies: `tauri`, `serde`, `serde_json`. Add
`tauri-plugin-dialog = "2"` (needed for the native folder picker in Task 4) and
`directories = "5"` **only if** `tauri`'s own path API cannot resolve
`%APPDATA%` — prefer the built-in and add no dependency.

Copy the wizard's icons into `Arcane/Hub/src-tauri/icons/` as placeholders; note in
the commit message that they are placeholders.

- [ ] **Step 3: Verify it builds and runs**

```
cd D:\dev\starworks\Gacha\Arcane\Hub
npm install
npm run tauri build
```
Expected: an NSIS installer under `src-tauri/target/release/bundle/nsis/`. Run it,
confirm it installs to `%LOCALAPPDATA%\Programs\` **without an admin prompt**, and
that launching shows an empty window titled "Arcane Hub".

If `npm run tauri build` is too slow for the loop, `npm run tauri dev` is fine for
iteration, but the installer must be produced at least once in this task.

- [ ] **Step 4: CI**

Create `.github/workflows/build-arcane-hub.yml` modelled on
`build-setup-wizard.yml`. It must run `cargo test` (Task 2 onward depends on that
being wired) and `cargo clippy -- -D warnings`, then build the bundle. Do not
copy the wizard's artifact-commit step if it commits a binary to the repo root —
the Hub ships as an installer artifact, not a repo-root binary.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Hub .github/workflows/build-arcane-hub.yml
git commit -m "feat(hub): Tauri scaffold for the Arcane Hub (per-user NSIS install)"
```

---

### Task 2: Hub state — recents and engines (pure Rust + tests)

**Files:**
- Create: `Arcane/Hub/src-tauri/src/paths.rs`, `state.rs`
- Modify: `Arcane/Hub/src-tauri/src/lib.rs` (declare the modules)

**Interfaces:**
- Produces (Tasks 3-5 use these exact names):
  - `paths::hub_dir() -> PathBuf` (`%APPDATA%\Arcane\hub`), `paths::recents_file()`, `paths::engines_file()`
  - `struct state::RecentProject { path: String, name: String, last_opened_utc: String, engine_abi: u32 }`
  - `struct state::EngineEntry { id: String, path: String, engine_abi: u32, build: String }`
  - `struct state::HubState { recents: Vec<RecentProject>, engines: Vec<EngineEntry> }`
  - `state::touch_recent(&mut Vec<RecentProject>, RecentProject)` — insert-or-move-to-front, dedupe by normalised path
  - `state::remove_recent(&mut Vec<RecentProject>, &str) -> bool`
  - `state::upsert_engine(&mut Vec<EngineEntry>, EngineEntry)` — dedupe by normalised path
  - `state::normalise_path(&str) -> String` — lowercase + forward slashes, for comparison only

- [ ] **Step 1: Write the failing tests**

Create `Arcane/Hub/src-tauri/src/state.rs` with a `#[cfg(test)] mod tests` containing
these, before the implementation:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    fn rp(path: &str, when: &str) -> RecentProject {
        RecentProject {
            path: path.to_string(),
            name: "N".to_string(),
            last_opened_utc: when.to_string(),
            engine_abi: 7,
        }
    }

    #[test]
    fn touch_recent_inserts_newest_first() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        assert_eq!(v.len(), 2);
        assert_eq!(v[0].path, "C:/b");
    }

    #[test]
    fn touch_recent_moves_existing_to_front_without_duplicating() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        touch_recent(&mut v, rp("C:/a", "3"));
        assert_eq!(v.len(), 2, "re-opening must not duplicate the entry");
        assert_eq!(v[0].path, "C:/a");
        assert_eq!(v[0].last_opened_utc, "3", "timestamp must refresh");
    }

    #[test]
    fn touch_recent_dedupes_case_and_separator_variants() {
        // Windows paths: the SAME project reached two ways must be one entry.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/Dev/MyGame", "1"));
        touch_recent(&mut v, rp("c:\\dev\\mygame", "2"));
        assert_eq!(v.len(), 1);
    }

    #[test]
    fn remove_recent_reports_whether_it_removed() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        assert!(remove_recent(&mut v, "C:/a"));
        assert!(!remove_recent(&mut v, "C:/a"), "second remove is a no-op");
        assert!(v.is_empty());
    }

    #[test]
    fn upsert_engine_replaces_same_path_rather_than_appending() {
        let mut v = Vec::new();
        upsert_engine(&mut v, EngineEntry {
            id: "e1".into(), path: "C:/eng/ArcaneEditor.exe".into(),
            engine_abi: 7, build: "old".into(),
        });
        upsert_engine(&mut v, EngineEntry {
            id: "e2".into(), path: "C:/ENG/arcaneeditor.exe".into(),
            engine_abi: 8, build: "new".into(),
        });
        assert_eq!(v.len(), 1, "re-registering the same engine must not append");
        assert_eq!(v[0].engine_abi, 8, "re-registering must refresh the probed data");
    }

    #[test]
    fn empty_state_is_valid_not_an_error() {
        // An installed Hub with no engine registered is a NORMAL first run.
        let s = HubState::default();
        assert!(s.engines.is_empty());
        assert!(s.recents.is_empty());
    }

    #[test]
    fn state_round_trips_through_json() {
        let mut s = HubState::default();
        touch_recent(&mut s.recents, rp("C:/a", "1"));
        let text = serde_json::to_string(&s).unwrap();
        let back: HubState = serde_json::from_str(&text).unwrap();
        assert_eq!(back.recents.len(), 1);
        assert_eq!(back.recents[0].path, "C:/a");
    }

    #[test]
    fn missing_or_corrupt_state_file_parses_as_default_not_a_crash() {
        // A hand-edited or truncated file must not brick the Hub.
        let back: Result<HubState, _> = serde_json::from_str("{ not json");
        assert!(back.is_err(), "caller is expected to fall back to default");
    }
}
```

- [ ] **Step 2: Run them to verify they fail**

```
cd D:\dev\starworks\Gacha\Arcane\Hub\src-tauri
cargo test
```
Expected: compile errors — the types and functions do not exist yet.

- [ ] **Step 3: Implement `paths.rs`**

```rust
// Where the Hub keeps its per-user state. This is HUB-OWNED: the engine never
// reads it. Keeping user-scope state out of the engine preserves the rule that
// Core and the runtime carry no host/user vocabulary.
//
// NOTE: deliberately NOT derived from current_exe(). The Hub is INSTALLED to
// %LOCALAPPDATA%\Programs\, so there is no repo root and no sibling engine to
// find -- that is setup-wizard's assumption, not ours.
use std::path::PathBuf;

pub fn hub_dir() -> PathBuf {
    let base = std::env::var("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|_| std::env::temp_dir());
    base.join("Arcane").join("hub")
}

pub fn recents_file() -> PathBuf { hub_dir().join("recents.json") }
pub fn engines_file() -> PathBuf { hub_dir().join("engines.json") }
```

- [ ] **Step 4: Implement `state.rs`**

Write the `serde`-derived structs (`#[serde(rename_all = "camelCase")]` so the Svelte
side sees `lastOpenedUtc`/`engineAbi`) plus:

```rust
// Compare-only normalisation: Windows paths are case-insensitive and accept
// either separator, so the SAME project reached two ways must collapse to one
// entry. The ORIGINAL string is what gets stored and displayed.
pub fn normalise_path(p: &str) -> String {
    p.replace('\\', "/").trim_end_matches('/').to_lowercase()
}

pub fn touch_recent(v: &mut Vec<RecentProject>, entry: RecentProject) {
    let key = normalise_path(&entry.path);
    v.retain(|e| normalise_path(&e.path) != key);
    v.insert(0, entry);
}

pub fn remove_recent(v: &mut Vec<RecentProject>, path: &str) -> bool {
    let key = normalise_path(path);
    let before = v.len();
    v.retain(|e| normalise_path(&e.path) != key);
    v.len() != before
}

pub fn upsert_engine(v: &mut Vec<EngineEntry>, entry: EngineEntry) {
    let key = normalise_path(&entry.path);
    v.retain(|e| normalise_path(&e.path) != key);
    v.push(entry);
}
```

`HubState` derives `Default`. Loading reads the two files and falls back to
`Default` on any IO or parse error (a hand-edited file must not brick the Hub).

- [ ] **Step 5: Run the tests**

```
cd D:\dev\starworks\Gacha\Arcane\Hub\src-tauri
cargo test
```
Expected: all 8 pass.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Hub/src-tauri/src
git commit -m "feat(hub): per-user recents + engines state with pure, tested list ops"
```

---

### Task 3: Engine registration via the slice-1 probe

**Files:**
- Create: `Arcane/Hub/src-tauri/src/engine.rs`
- Modify: `Arcane/Hub/src-tauri/src/lib.rs`

**Interfaces:**
- Consumes: `state::EngineEntry`, `state::upsert_engine`.
- Produces:
  - `struct engine::EngineInfo { engine_abi: u32, build: String, exe_path: String }`
  - `engine::parse_probe_output(&str) -> Result<EngineInfo, String>` — **pure, tested**
  - `engine::resolve_editor_exe(&Path) -> PathBuf` — accepts either the exe itself or a directory containing it
  - Tauri command `register_engine(path: String) -> Result<EngineEntry, String>`

- [ ] **Step 1: Write the failing tests**

In `engine.rs`, before implementing:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    // A real payload, copied verbatim from a slice-1 run of
    // `ArcaneEditor.exe --print-engine-info`.
    const REAL: &str = r#"{"build":"Arcane 0.1 (M5) [Debug]","engineAbi":7,"exePath":"D:/x/ArcaneEditor.exe"}"#;

    #[test]
    fn parses_a_real_probe_payload() {
        let info = parse_probe_output(REAL).expect("should parse");
        assert_eq!(info.engine_abi, 7);
        assert_eq!(info.build, "Arcane 0.1 (M5) [Debug]");
        assert_eq!(info.exe_path, "D:/x/ArcaneEditor.exe");
    }

    #[test]
    fn tolerates_surrounding_whitespace_and_a_trailing_newline() {
        let info = parse_probe_output(&format!("  {REAL}\r\n")).expect("should parse");
        assert_eq!(info.engine_abi, 7);
    }

    #[test]
    fn rejects_non_json_rather_than_panicking() {
        assert!(parse_probe_output("not json at all").is_err());
        assert!(parse_probe_output("").is_err());
    }

    #[test]
    fn rejects_a_payload_missing_engine_abi() {
        // An older engine, or the wrong exe entirely. Must be a clean error --
        // the Hub stamps engine.abi into every project it creates, so a
        // defaulted 0 here would mint broken projects.
        let r = parse_probe_output(r#"{"build":"x","exePath":"y"}"#);
        assert!(r.is_err(), "missing engineAbi must not silently default");
    }

    #[test]
    fn ignores_unknown_future_fields() {
        // Forward-compat: a newer engine may add keys. That must not break an
        // older Hub.
        let info = parse_probe_output(
            r#"{"engineAbi":9,"build":"b","exePath":"p","futureThing":true}"#
        ).expect("unknown fields must be ignored");
        assert_eq!(info.engine_abi, 9);
    }

    #[test]
    fn resolve_editor_exe_accepts_a_directory_or_the_exe_itself() {
        use std::path::Path;
        assert!(resolve_editor_exe(Path::new("C:/eng"))
            .ends_with("ArcaneEditor.exe"));
        assert!(resolve_editor_exe(Path::new("C:/eng/ArcaneEditor.exe"))
            .ends_with("ArcaneEditor.exe"));
    }
}
```

- [ ] **Step 2: Run to verify failure**

`cargo test` from `Arcane/Hub/src-tauri`. Expected: compile errors.

- [ ] **Step 3: Implement the pure half**

`EngineInfo` derives `Deserialize` with `#[serde(rename_all = "camelCase")]` and NO
`#[serde(default)]` on `engine_abi` — a missing ABI must be an error, never a
defaulted 0, because that number is stamped into every project the Hub creates.
`parse_probe_output` trims and calls `serde_json::from_str`, mapping the error to a
`String`. `resolve_editor_exe` appends `ArcaneEditor.exe` when the input has no
`.exe` extension.

- [ ] **Step 4: Implement the command (not unit-tested)**

In `lib.rs`, `register_engine` spawns the resolved exe with `--print-engine-info`,
`Stdio::piped()`, and `.creation_flags(0x0800_0000)` (`CREATE_NO_WINDOW` — the editor
is a ConsoleApp, so without this a console flashes). Read stdout to a string, wait
with a timeout, `parse_probe_output`, build an `EngineEntry`, `upsert_engine`, save,
return it. Map every failure to a human-readable `Err(String)` the UI can show
verbatim — the most likely one is "this path is not an Arcane engine".

- [ ] **Step 5: Verify against the REAL engine, by hand**

With this repo built, register
`D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor` in the Hub.
Expected: it registers, showing abi 7 and the Debug build string, and **no console
window flashes**. Then register a junk path and confirm a clean error, no crash.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Hub/src-tauri/src
git commit -m "feat(hub): register engines by probing --print-engine-info"
```

---

### Task 4: Projects screen — list, open, locate, remove

**Files:**
- Create: `Arcane/Hub/src/routes/+page.svelte`, `src/routes/settings/+page.svelte`, `src/lib/api.ts`
- Modify: `Arcane/Hub/src-tauri/src/lib.rs`

**Interfaces:**
- Consumes: Tasks 2 and 3.
- Produces: commands `load_state()`, `open_project(projectPath, enginePath)`,
  `forget_project(path)`, `pick_folder()`.

- [ ] **Step 1: Implement `open_project`**

Spawns `<enginePath> --project <projectPath>` with `creation_flags(0x0800_0000)`,
**does not wait**, and returns immediately. The Hub stays running and the editor is
independent of it — closing the Hub must not close editors, and multiple editors may
run at once. Before spawning, verify both paths still exist and return a clear error
if not.

- [ ] **Step 2: Build the Projects screen**

A list of `recents` showing name, path, last opened, engine ABI. A prominent
**Open…** (native folder dialog via `tauri-plugin-dialog`) and **New Project…**
(Task 5). Per row: Open / Locate… / Remove from list. A row whose path no longer
resolves renders greyed with Locate…/Remove and is **never silently dropped** — a
missing project is usually a moved folder, not an abandoned one.

Empty state explains what a project is and offers New Project….

**If no engine is registered**, the screen must say so and link to Settings rather
than erroring — an empty engine list is a normal first run for an installed Hub.

- [ ] **Step 3: Build the Settings screen**

Lists registered engines (path, ABI, build string) with Add… and Remove. Add… opens
a folder dialog and calls `register_engine`, showing the probe result inline so a bad
path is obvious immediately.

Must support registering a **source/dev build** out of this repo's
`bin/<Config>-windows-x86_64-md/ArcaneEditor/` — that is the day-one case, not a
later convenience, because it is the only kind of engine that exists today.

- [ ] **Step 4: Verify by hand**

Register the repo's Debug engine, add a project via Open…, confirm the editor
launches with no console flash and the Hub stays up. Close the Hub; the editor must
survive. Rename the project folder on disk and confirm the row greys out with
Locate…/Remove rather than vanishing.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Hub
git commit -m "feat(hub): projects + settings screens, launch editor by spawn"
```

---

### Task 5: New Project (slice 3)

**Files:**
- Create: `Arcane/Hub/src-tauri/src/project.rs`
- Modify: `Arcane/Hub/src-tauri/src/lib.rs`, `Arcane/Hub/src/routes/+page.svelte`

**Interfaces:**
- Consumes: `engine::EngineInfo`, `state::touch_recent`.
- Produces: `project::manifest_json(name: &str, engine_abi: u32) -> String` (pure,
  tested); command `create_project(dir, name, enginePath)`.

- [ ] **Step 1: Write the failing tests**

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manifest_has_the_three_required_fields() {
        // ProjectManifest::FromJson REQUIRES formatVersion > 0, name, engine.abi.
        // Everything else is optional -- a content-only project omits gameModule.
        let v: serde_json::Value =
            serde_json::from_str(&manifest_json("MyGame", 7)).unwrap();
        assert!(v["formatVersion"].as_i64().unwrap() > 0);
        assert_eq!(v["name"], "MyGame");
        assert_eq!(v["engine"]["abi"], 7);
    }

    #[test]
    fn manifest_stamps_the_PROBED_abi_not_a_constant() {
        // The whole reason slice 1's probe exists. A hardcoded ABI mints
        // projects that crash on open the moment the engine bumps.
        let v: serde_json::Value =
            serde_json::from_str(&manifest_json("G", 42)).unwrap();
        assert_eq!(v["engine"]["abi"], 42);
    }

    #[test]
    fn manifest_omits_gameModule_for_a_content_only_project() {
        let v: serde_json::Value =
            serde_json::from_str(&manifest_json("G", 7)).unwrap();
        assert!(v.get("gameModule").is_none() || v["gameModule"] == "");
    }

    #[test]
    fn manifest_escapes_names_safely() {
        let v: serde_json::Value =
            serde_json::from_str(&manifest_json("My \"Game\"", 7)).unwrap();
        assert_eq!(v["name"], "My \"Game\"");
    }
}
```

- [ ] **Step 2: Run to verify failure**, then implement `manifest_json` with
`serde_json` (never string concatenation — that is what the escaping test pins).
Use `formatVersion: 1` unless reading
`Arcane/Arcane/src/Arcane/Project/ProjectManifest.cpp` shows a different current
value; check rather than assume.

- [ ] **Step 3: Implement `create_project`**

Order matters: **probe the engine FIRST**, and abort if the probe fails — never fall
back to a guessed ABI. Then create `<dir>/<name>/`, write `<name>.arcproj`, create
`Content/`, `touch_recent`, save, and spawn the editor on it. Refuse if the target
directory already exists and is non-empty.

- [ ] **Step 4: Verify end to end, by hand**

New Project → pick a directory → the editor opens on it with the Outliner empty and
the Assets browser present. Close it, reopen from the Hub's recents. Open the
generated `.arcproj` in a text editor and confirm `engine.abi` is **7**, matching
what `ArcaneEditor.exe --print-engine-info` reports.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Hub
git commit -m "feat(hub): create content-only projects with a probed engine ABI"
```

---

### Task 6: Record

- [ ] **Step 1** Mark slices 2 and 3 BUILT in
`docs/superpowers/specs/2026-07-26-arcane-hub-launcher-design.md`.
- [ ] **Step 2** Ledger in `.superpowers/sdd/progress.md`: commits, `cargo test`
counts, the hand-verified steps, and explicitly **which behaviour has no automated
coverage** (all Svelte UI, all process spawning, the installer itself).
- [ ] **Step 3** Note the follow-ups this plan deliberately does NOT do: engine
*downloads/installs*, a templates gallery, accounts, absorbing `Setup.exe`'s
doctor/vcpkg flow, and retiring `Tools/`.
- [ ] **Step 4** Commit.

## Verification Summary

| What | How | Where |
|---|---|---|
| Recents dedupe / ordering / case+separator collapse | `cargo test` | Task 2 |
| Empty state is valid; corrupt file falls back | `cargo test` | Task 2 |
| Probe parsing incl. missing-ABI rejection + forward-compat | `cargo test` | Task 3 |
| Manifest required fields, probed ABI, escaping | `cargo test` | Task 5 |
| Registering the REAL repo engine, no console flash | Hand | Task 3 |
| Editor launches, survives Hub close | Hand | Task 4 |
| Missing project greys out, is not dropped | Hand | Task 4 |
| Created `.arcproj` has abi 7 matching the probe | Hand | Task 5 |
| Per-user install, no admin prompt | Hand | Task 1 |
| **All Svelte UI, all spawning, the installer** | **NOT automated** | — |

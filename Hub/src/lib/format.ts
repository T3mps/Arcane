// The Hub's only branching logic, kept out of markup so it can be unit-tested.
// Everything else in src/lib is presentational.

/**
 * Whether `projectAbi` will open under `engineAbi`.
 *
 * Exact parity with the rule this replaces. Two permissive cases are
 * deliberate: abi 0 means the manifest did not state one (we cannot prove a
 * conflict, so we must not brand it broken), and a null engine means nothing
 * is selected to conflict with -- the UI gates launching separately.
 */
export function isCompatible(projectAbi: number, engineAbi: number | null): boolean {
  if (engineAbi === null) return true;
  if (projectAbi === 0) return true;
  return projectAbi === engineAbi;
}

/** Case-insensitive substring over BOTH name and path; blank query passes all. */
export function filterProjects<T extends { name: string; path: string }>(
  items: T[],
  query: string,
): T[] {
  const q = query.trim().toLowerCase();
  if (!q) return items.slice();
  return items.filter(
    (i) => i.name.toLowerCase().includes(q) || i.path.toLowerCase().includes(q),
  );
}

/** The sortable columns, matching Rust's settings::SORT_* vocabulary. */
export type SortKey = "opened" | "name" | "engine" | "abi";

/**
 * Column-header click semantics: clicking the active column flips its
 * direction; clicking a new column starts at that column's natural direction
 * -- newest first for Opened (a launcher's recents), A-to-Z / low-to-high
 * for everything else.
 */
export function nextSort(
  key: SortKey,
  desc: boolean,
  clicked: SortKey,
): { key: SortKey; desc: boolean } {
  if (clicked === key) return { key, desc: !desc };
  return { key: clicked, desc: clicked === "opened" };
}

// "never opened" (unparseable stamp) sorts as older than everything real.
function openedNum(s: string): number {
  const n = Number(s);
  return Number.isFinite(n) ? n : 0;
}

/**
 * Display order: favourites always above the rest, whatever the sort (the
 * Godot model -- starring visibly promotes, where a sortable star column
 * only matters once you sort by it), with the chosen column applied within
 * each half. Non-mutating; ties keep their incoming (recency) order because
 * Array.prototype.sort is stable.
 *
 * `engineLabelOf` is injected because which engine a project resolves to --
 * pin, dangling pin, default -- is the caller's per-view knowledge, and
 * duplicating resolveEngine here would be a second brain.
 */
export function sortProjects<
  T extends { name: string; lastOpenedUtc: string; engineAbi: number; favorite: boolean },
>(items: T[], key: SortKey, desc: boolean, engineLabelOf: (p: T) => string): T[] {
  const cmp = (a: T, b: T): number => {
    switch (key) {
      case "name":
        return a.name.localeCompare(b.name, undefined, { sensitivity: "base" });
      case "engine":
        return engineLabelOf(a).localeCompare(engineLabelOf(b), undefined, { sensitivity: "base" });
      case "abi":
        return a.engineAbi - b.engineAbi;
      case "opened":
        return openedNum(a.lastOpenedUtc) - openedNum(b.lastOpenedUtc);
    }
  };
  return [...items].sort((a, b) => {
    if (a.favorite !== b.favorite) return a.favorite ? -1 : 1;
    const d = cmp(a, b);
    return desc ? -d : d;
  });
}

/**
 * The identity a project is keyed on, independent of how it was recorded.
 *
 * Recents holds two shapes for the same project: a FOLDER (how opens were
 * recorded before the Open dialog asked for a `.arcproj`) and the MANIFEST
 * FILE (what it yields now). Both must collapse to one key, or a project's
 * cover art would change the first time it is re-opened -- exactly the
 * "reads as a bug" failure the cover is supposed to avoid.
 *
 * Case and separator folding mirrors `state::normalise_path` in Rust, which
 * dedupes the recents list on the same basis.
 */
/**
 * Mirror of Rust's `state::normalise_path` -- the fold RunningEditors keys
 * live under. Change BOTH or the running badge stops matching its project.
 */
export function normalisePath(p: string): string {
  return p.replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase();
}

export function projectKey(path: string): string {
  const norm = normalisePath(path);
  const folder = norm.replace(/\/[^/]+\.arcproj$/, "");
  return folder === "" ? norm : folder;
}

/**
 * The FOLDER a project lives in: the recorded path with a trailing
 * `<name>.arcproj` segment removed.
 *
 * What a project list should show is where the project IS, not the name of one
 * file inside it repeated after the name already in the row. Distinct from
 * `projectKey`, which folds the same shape but also lowercases and rewrites
 * separators -- that output is an identity, and showing it would display a path
 * the user never typed.
 *
 * Paired with `project_dir` in src-tauri/src/lib.rs, which resolves the same
 * folder for Show in Explorer.
 */
export function projectDir(path: string): string {
  const trimmed = path.replace(/[/\\]+$/, "");
  // The separator right before the manifest goes with it; a path that is
  // nothing but a manifest name has no folder to show, so it stays as-is.
  const cut = trimmed.replace(/[/\\][^/\\]+\.arcproj$/i, "");
  return cut === "" ? trimmed : cut;
}

// Characters Windows rejects in a file or folder name. Spaces and hyphens are
// deliberately absent -- "My Game" and "3d-demo" are ordinary folder names.
const ILLEGAL_NAME_CHARS = /[<>:"/\\|?*]/;
// A code-point test rather than a regex range: an escaped range is easy to
// write as literal control bytes by accident, and this file is ASCII source.
function hasControlChar(s: string): boolean {
  for (let i = 0; i < s.length; i++) {
    if (s.charCodeAt(i) < 0x20 || s.charCodeAt(i) === 0x7f) return true;
  }
  return false;
}

// Reserved DOS device names, alone or with an extension.
const RESERVED_NAMES = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\.|$)/i;

// The name lands in the path TWICE (<dir>/<name>/<name>.arcproj), so a long one
// eats the 260-char Windows limit fast. Capping here turns an unhelpful raw
// "could not create <path>" from the OS into a sentence next to the field.
const MAX_NAME_LEN = 64;

/**
 * Why a project name is not usable as a Windows folder name, or null if it is.
 *
 * Catching this in the dialog beats letting `create_project` fail: the Rust
 * error surfaces as a raw `could not create <path>` banner after the user has
 * already picked a location.
 */
export function projectNameError(name: string): string | null {
  const n = name.trim();
  if (!n) return "Enter a project name.";
  const bad = n.match(ILLEGAL_NAME_CHARS);
  if (bad) return `A project name cannot contain "${bad[0]}".`;
  if (hasControlChar(n)) return "A project name cannot contain a control character.";
  // Windows silently strips a trailing dot or space, so the folder on disk
  // would not match the name the user typed.
  if (/[. ]$/.test(n)) return "A project name cannot end with a space or a dot.";
  if (RESERVED_NAMES.test(n)) return `"${n}" is a name Windows reserves.`;
  if (n.length > MAX_NAME_LEN) {
    return `Keep the name under ${MAX_NAME_LEN} characters (this one is ${n.length}).`;
  }
  return null;
}

/**
 * The `.arcproj` path `create_project` will produce, for display in the
 * new-project dialog. Cosmetic: Rust joins the real path with `PathBuf`.
 */
export function projectPathPreview(dir: string, name: string): string {
  const d = dir.trim().replace(/[/\\]+$/, "");
  const n = name.trim();
  if (!d || !n) return "";
  // Match the separator already in use so the preview looks like the path the
  // user picked; a bare drive ("D:") is a Windows path even with no separator.
  const sep = d.includes("\\") || /^[a-z]:$/i.test(d) ? "\\" : "/";
  return `${d}${sep}${n}${sep}${n}.arcproj`;
}

export type Resolved<E> = {
  /** The engine that will actually launch this project; null if none can. */
  engine: E | null;
  /** An explicit per-project choice is in effect (not the default). */
  pinned: boolean;
  /** A pin exists but names an engine that is no longer registered. */
  dangling: boolean;
};

/**
 * Which engine opens a given project: its own pin if it has a live one, else
 * the Hub default.
 *
 * A pin that no longer resolves falls back to the default rather than refusing
 * to launch -- an unregistered engine is a Hub-state problem, not a reason to
 * strand the project -- but it reports `dangling` so the UI can say so instead
 * of silently pretending the choice was never made.
 *
 * Structurally typed rather than importing EngineEntry: this module stays
 * dependency-free so it can be unit-tested without any Tauri surface.
 */
export function resolveEngine<E extends { id: string }>(
  engineId: string | null,
  engines: E[],
  fallback: E | null,
): Resolved<E> {
  if (engineId === null || engineId === undefined) {
    return { engine: fallback, pinned: false, dangling: false };
  }
  const found = engines.find((e) => e.id === engineId);
  if (found) return { engine: found, pinned: true, dangling: false };
  return { engine: fallback, pinned: false, dangling: true };
}

/** How the project list is laid out. Persisted in Settings. */
export type ProjectView = "grid" | "list";

/**
 * Label for a project's engine chip.
 *
 * Lives here rather than inline in markup because the tile and the list row
 * both render it, and because this copy has been wrong once already: it said
 * "the selected engine" after engines became per-project. Copy that states a
 * fact about the system is logic, and gets tested like logic.
 */
export function engineChipText(engineLabel: string, pinned: boolean, dangling: boolean): string {
  if (dangling) return "Engine missing";
  return pinned ? engineLabel : `Default: ${engineLabel}`;
}

/** Long-form explanation for the engine chip's tooltip. */
export function engineChipTitle(engineLabel: string, pinned: boolean, dangling: boolean): string {
  if (dangling) {
    return "This project was pinned to an engine that is no longer registered. " +
      "It will open with the default instead. Click to choose one.";
  }
  return pinned
    ? `Always opens with ${engineLabel}. Click to change.`
    : `Follows the Hub default (${engineLabel}). Click to pin an engine.`;
}

/**
 * The launch target's tooltip for a project that is not on disk any more.
 *
 * Shared by the tile and the list row like the chip copy above, and tested for
 * the same reason: it states a fact ("this path does not resolve") and names
 * the two controls that can fix it, so drifting copy here would misdirect the
 * user at exactly the moment something already looks broken.
 */
export function missingNote(path: string): string {
  return `Nothing is at ${path} any more. ` +
    "Locate… points the Hub at where it moved; Remove from list forgets it.";
}

/**
 * What to say about a project's compatibility with the engine that will open
 * it. Names the engine, because for a pinned project the sidebar selection is
 * not the one being talked about.
 */
export function compatibilityNote(
  compatible: boolean,
  path: string,
  projectAbi: number,
  engineLabel: string,
  engineAbi: number | null,
): string {
  if (compatible) return path;
  return `Built against abi ${projectAbi}; ${engineLabel} is abi ${engineAbi}. ` +
    "It will refuse to open.";
}

/**
 * One entry of the per-project action menu. `kind` doubles as the key into
 * ProjectActions (ProjectMenu.svelte), so the menu dispatches by lookup with
 * no hand-written mapping to drift. `sep` opens the below-the-line group;
 * `danger` is the red hover reserved for the one action that touches disk.
 */
export type MenuItemKind = "reveal" | "rename" | "args" | "forget" | "locate" | "delete";
export type MenuItem = { kind: MenuItemKind; label: string; danger?: boolean; sep?: boolean };

/**
 * The action-menu vocabulary as data, so it is tested like the chip copy.
 *
 * A MISSING project gets exactly the two items that still mean something --
 * repair the row or retire it; everything else acts on disk state that is not
 * there, and a menu of disabled items would make the user hunt for the one
 * that works. Locate is hidden on healthy rows for the same reason Unity
 * hides it: repointing a project that resolves is a mistake waiting to be
 * offered. Delete's ellipsis follows the confirmation setting -- a label
 * promising a dialog that will not appear is the worst place to be inaccurate.
 */
export function menuItemsFor(missing: boolean, confirmDelete: boolean): MenuItem[] {
  if (missing) {
    return [
      { kind: "locate", label: "Locate…" },
      { kind: "forget", label: "Remove from list", sep: true },
    ];
  }
  return [
    { kind: "reveal", label: "Show in Explorer" },
    { kind: "rename", label: "Rename project…" },
    { kind: "args", label: "Command-line arguments…" },
    { kind: "forget", label: "Remove from list", sep: true },
    {
      kind: "delete",
      label: confirmDelete ? "Delete project…" : "Delete project",
      danger: true,
    },
  ];
}

export type Cover = { monogram: string; angle: number };

/**
 * Cover art inputs for a project card: a monogram from the NAME and a gradient
 * angle from the PATH.
 *
 * Keyed on path, not name, because the path is the identity (two projects may
 * share a display name) and because it must be stable: a card that changed
 * appearance between launches would read as a bug. FNV-1a is used purely
 * because it is short, deterministic, and dependency-free -- no hash quality
 * is required here.
 *
 * The path is folded through `projectKey` first, so folder-recorded and
 * manifest-recorded entries for one project produce the same cover.
 */
export function coverFor(name: string, path: string): Cover {
  const ch = [...name].find((c) => /[\p{L}\p{N}]/u.test(c));
  const monogram = ch ? ch.toUpperCase() : "?";

  const key = projectKey(path);
  let h = 0x811c9dc5;
  for (let i = 0; i < key.length; i++) {
    h ^= key.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  // 100..200 degrees keeps every gradient diagonal and on-brand; a full 0..360
  // sweep would put some covers in flat vertical/horizontal bands.
  const angle = 100 + (h % 101);
  return { monogram, angle };
}

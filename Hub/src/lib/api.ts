// Typed wrappers over the Rust commands. Keeping the invoke strings in one
// place means a renamed command breaks here, not in a component.
import { invoke } from "@tauri-apps/api/core";
import type { ProjectView } from "$lib/format";

export type RecentProject = {
  path: string;
  name: string;
  lastOpenedUtc: string;
  engineAbi: number;
  /** Pinned engine id, or null to follow the Hub default. See resolveEngine. */
  engineId: string | null;
  /** Extra launch arguments, as typed. Empty = none. Split Rust-side at launch. */
  args: string;
  /**
   * The recorded path no longer resolves on disk. Stamped Rust-side on every
   * load, so it is a fact about right now, not about when the file was saved.
   * The row renders greyed with Locate/Remove instead of vanishing.
   */
  missing: boolean;
};

export type EngineEntry = {
  id: string;
  path: string;
  engineAbi: number;
  build: string;
  /**
   * The exe no longer resolves on disk. Stamped Rust-side on every load, like
   * RecentProject.missing. The row stays (a rebuild restores it in place);
   * the pre-launch probe refuses it with a reason if launched anyway.
   */
  missing: boolean;
};

export type HubState = {
  recents: RecentProject[];
  engines: EngineEntry[];
  /**
   * Problems found while loading state -- currently, a state file that existed
   * but could not be parsed and has been set aside. Recovering from that used
   * to be silent, so the user just found an empty project list.
   */
  warnings: string[];
};

/** Mirrors `settings::Settings`. Every field is read somewhere. */
export type Settings = {
  /** Starting directory for the New Project and Open dialogs; "" = OS default. */
  defaultProjectDir: string;
  /**
   * Hide the Hub while a launched editor runs, restored when the last one
   * exits. Read RUST-side (open_project hides, the wait thread restores);
   * the frontend only edits it. Replaced closeAfterLaunch 2026-07-29.
   */
  hideWhileRunning: boolean;
  /**
   * Project list layout. Rust normalises this through `clean_view` on both
   * load and save, so it is always one of the two -- never a stray string.
   */
  projectView: ProjectView;
  /** Show the confirmation dialog before deleting. Read in the delete handler. */
  confirmDelete: boolean;
};

export const loadState = () => invoke<HubState>("load_state");
export const registerEngine = (path: string) => invoke<EngineEntry>("register_engine", { path });
/**
 * Re-probe every registered engine and return state with refreshed abi/build.
 * Once per launch, after the first paint: registration caches the probe, and a
 * dev-loop engine rebuilt in place makes that cache lie about compatibility.
 */
export const refreshEngines = () => invoke<HubState>("refresh_engines");
export const forgetEngine = (path: string) => invoke<void>("forget_engine", { path });
/**
 * Delete the project's folder to the RECYCLE BIN, then drop it from the list.
 * A project whose folder is already gone is simply unlisted.
 */
export const deleteProject = (path: string) => invoke<void>("delete_project", { path });
/** Remove one project from the list. Hub state ONLY -- nothing touches disk. */
export const forgetProject = (path: string) => invoke<void>("forget_project", { path });
/** Hub state ONLY -- unlike deleteProject, nothing is removed from disk. */
export const clearRecents = () => invoke<void>("clear_recents");
/** Pin a project to an engine, or pass null to send it back to the default. */
export const setProjectEngine = (path: string, engineId: string | null) =>
  invoke<void>("set_project_engine", { path, engineId });
export const suggestEngine = () => invoke<EngineEntry | null>("suggest_engine");
/** Extra arguments appended after `--project <path>` for this project only. */
export const setProjectArgs = (path: string, args: string) =>
  invoke<void>("set_project_args", { path, args });
/** Open the project's folder in Explorer, with its .arcproj selected. */
export const revealProject = (path: string) => invoke<void>("reveal_project", { path });
/**
 * Rename the folder, the `.arcproj`, the name inside it, and the Hub entry.
 * Returns the new manifest path.
 */
export const renameProject = (path: string, newName: string) =>
  invoke<string>("rename_project", { path, newName });
/**
 * Repoint a moved project at the `.arcproj` the user located. The engine pin
 * and launch arguments survive; name and ABI refresh from the new manifest.
 */
export const relocateProject = (path: string, newPath: string) =>
  invoke<void>("relocate_project", { path, newPath });

export const loadSettings = () => invoke<Settings>("load_settings");
export const saveSettings = (settings: Settings) => invoke<void>("save_settings", { settings });
/** The configured start folder, or null when unset or no longer on disk. */
export const defaultDialogDir = () => invoke<string | null>("default_dialog_dir");
export const hubDataDir = () => invoke<string>("hub_data_dir");
export const revealHubDataDir = () => invoke<void>("reveal_hub_data_dir");
export const hubVersion = () => invoke<string>("hub_version");

/** `projectPath` is a `.arcproj` file or a project folder; the engine takes both. */
export const openProject = (projectPath: string, enginePath: string) =>
  invoke<void>("open_project", { projectPath, enginePath });

/** Returns the path of the `.arcproj` it wrote, ready to hand to openProject. */
export const createProject = (dir: string, name: string, enginePath: string) =>
  invoke<string>("create_project", { dir, name, enginePath });

// "2m ago" style. Seconds-since-epoch is what the Rust side stores.
export function since(utcSeconds: string): string {
  const then = Number(utcSeconds);
  if (!Number.isFinite(then) || then <= 0) return "never";
  const d = Math.max(0, Math.floor(Date.now() / 1000) - then);
  if (d < 60) return "just now";
  if (d < 3600) return `${Math.floor(d / 60)}m ago`;
  if (d < 86400) return `${Math.floor(d / 3600)}h ago`;
  return `${Math.floor(d / 86400)}d ago`;
}

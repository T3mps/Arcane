// Typed wrappers over the Rust commands. Keeping the invoke strings in one
// place means a renamed command breaks here, not in a component.
import { invoke } from "@tauri-apps/api/core";

export type RecentProject = {
  path: string;
  name: string;
  lastOpenedUtc: string;
  engineAbi: number;
};

export type EngineEntry = {
  id: string;
  path: string;
  engineAbi: number;
  build: string;
};

export type HubState = {
  recents: RecentProject[];
  engines: EngineEntry[];
};

export const loadState = () => invoke<HubState>("load_state");
export const registerEngine = (path: string) => invoke<EngineEntry>("register_engine", { path });
export const forgetEngine = (path: string) => invoke<void>("forget_engine", { path });
export const forgetProject = (path: string) => invoke<void>("forget_project", { path });
export const suggestEngine = () => invoke<EngineEntry | null>("suggest_engine");

export const openProject = (projectPath: string, enginePath: string) =>
  invoke<void>("open_project", { projectPath, enginePath });

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

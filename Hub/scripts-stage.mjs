// Copy the built Hub exe next to the other Arcane build outputs, so the dev
// loop matches every C++ project in the workspace:
//   Arcane/bin/<Config>-windows-x86_64-md/Hub/arcane_hub.exe
//
// This is a DEV convenience only. It does NOT mean the Hub may assume an
// adjacent engine -- in production the Hub is installed to
// %LOCALAPPDATA%\Programs\Arcane Hub\ and engines are registered by path.
// Adjacency is used as a first-run SUGGESTION, never an assumption.
import { copyFileSync, mkdirSync, existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const profile = process.argv[2] === "debug" ? "debug" : "release";
const config = profile === "debug" ? "Debug" : "Release";

const src = resolve(here, "..", "bin-int", "hub-cargo", profile, "arcane_hub.exe");
const outDir = resolve(here, "..", "bin", `${config}-windows-x86_64-md`, "Hub");
const dst = join(outDir, "arcane_hub.exe");

if (!existsSync(src)) {
  console.error(`stage: no build at ${src} -- run the tauri build first`);
  process.exit(1);
}
mkdirSync(outDir, { recursive: true });
copyFileSync(src, dst);
console.log(`stage: ${dst}`);

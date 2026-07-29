// Copy the built Hub exe over the INSTALLED one, so a dev rebuild updates
// the copy the .arcproj file association and the Start menu actually launch:
//   %LOCALAPPDATA%\Arcane Hub\arcane_hub.exe
// (User call 2026-07-29, replacing the old Arcane/bin/<Config>/Hub/ staging:
// once the NSIS install existed, staging beside the C++ outputs just meant
// two Hubs, and the one Windows launches was always the stale one.)
//
// The exe must not be running when this copies -- the build scripts stop it
// first. A debug stage ("stage:debug") lands in the same place: there is one
// installed Hub, whichever profile built it.
import { copyFileSync, mkdirSync, existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const profile = process.argv[2] === "debug" ? "debug" : "release";

const src = resolve(here, "..", "bin-int", "hub-cargo", profile, "arcane_hub.exe");
const localAppData = process.env.LOCALAPPDATA;
if (!localAppData) {
  console.error("stage: LOCALAPPDATA is not set -- cannot find the install folder");
  process.exit(1);
}
const outDir = join(localAppData, "Arcane Hub");
const dst = join(outDir, "arcane_hub.exe");

if (!existsSync(src)) {
  console.error(`stage: no build at ${src} -- run the tauri build first`);
  process.exit(1);
}
mkdirSync(outDir, { recursive: true });
copyFileSync(src, dst);
console.log(`stage: ${dst}`);

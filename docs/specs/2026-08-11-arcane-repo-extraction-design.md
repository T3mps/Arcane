# Arcane Repo Extraction -- Design

**Date:** 2026-08-11
**Decisions (user-ratified):** engine -> `github.com/T3mps/Arcane` (already
created, PUBLIC) with FULL FILTERED HISTORY; checkout at
`D:\dev\starworks\Arcane` with the repo root AS the workspace root (no
`Arcane/Arcane` nesting); engine docs COPIED into the new repo; the sibling
game project MERGES INTO the org monorepo (StarworksDev/Aphelyon stays the
one game repo); engine CI = new Jenkins pipeline + a GitHub Actions lane.

## End state

**`T3mps/Arcane`** (public) -- repo root IS the premake workspace:
`premake5.lua`, `GenerateProjects.bat`, `ArcaneCore/ ArcaneClient/
ArcaneRuntime/ ArcaneEditor/ ArcaneHub/ ArcaneServer/ ArcaneTests/
ReferenceProject/ data/ build/ scripts/`, plus `ThirdParty/` (the engine's
subset), `docs/` (engine copy of docs/superpowers), README, `.github/
workflows/`, `Jenkinsfile`. `ARCANE_SDK=D:\dev\starworks\Arcane`.

**`StarworksDev/Aphelyon`** -- tip DROPS `Arcane/`, `Client/`, `Tools/`
(history keeps them); GAINS the sibling game project as `Game/` via subtree
merge (its own history grafts in -- the sibling repo at
`D:\dev\starworks\Aphelyon` currently has NO remote and retires after the
merge). `Server/` stays; its ArcaneCore consumption moves from the in-tree
`Arcane/ArcaneCore/src` to `$ARCANE_SDK/ArcaneCore/src` (the same
SDK-consumption pattern game modules use). Jenkinsfile slims to
Server(+Game) stages.

## Phase 1 -- extract the engine (non-destructive to Gacha)

1. **Safety push first:** push Gacha main (85+ ahead) to origin.
2. Fresh clone of the LOCAL Gacha repo -> `git filter-repo` keeping:
   - `Arcane/` with path-rename `Arcane/` -> `` (flatten to root),
   - `ThirdParty/<engine subset>`: nlohmann, picosha2 (ArcaneCore Crypto),
     spdlog, Catch2, rapidcheck, glm, stb, miniaudio, Astra, enkiTS, tracy,
     freetype, msdfgen, nvrhi, Vulkan-Headers, DirectX-Headers, imgui,
     imgui-node-editor, Manifold2D, Mosaic, premake5, tools (dxc), README.
     EXCLUDED (game-only): sqlpp23, aphelyon-sql-types, XoshiroCpp, love2d.
   - `docs/superpowers/` (full copy -- engine and game history are
     intertwined; duplication ratified).
3. **Flatten fixups** (one commit in the new repo; `%{wks.location}` is now
   the repo root):
   - `premake5.lua`: every `../ThirdParty` -> `ThirdParty` (includes,
     IncludeDir table, Dependencies group includes, dxc copy paths, the
     Client font copy for ArcaneTests -> replace with an in-repo font from
     `data/font/`).
   - `build/arcane.lua`: `ARCANE_TP = ARCANE_SDK .. "/../ThirdParty"` ->
     `ARCANE_SDK .. "/ThirdParty"`. External consumers change NOTHING but
     the env var value.
   - `GenerateProjects.bat` / `scripts/setup-vcpkg-deps.bat`:
     `..\ThirdParty\premake5` -> `ThirdParty\premake5`, etc.
   - `data/shaders/compile-shaders.bat`: DXC path loses one `..\`.
   - `ReferenceProject/premake5.lua`: `path.getabsolute("..")` still equals
     the repo root -- UNCHANGED.
   - `.gitignore` (engine-relevant subset, paths de-prefixed), fresh
     `CLAUDE.md`/`AGENTS.md` for the engine repo, README, LICENSE +
     third-party notices (the repo is PUBLIC; vendored deps carry their
     licenses).
4. **Verify at `D:\dev\starworks\Arcane`:** GenerateProjects, Debug+Release
   builds 0/0, ArcaneTests `~[gpu]` green from the exe dir, ReferenceProject
   SDK build -> `Binaries/ReferenceGame.dll`, Hub `cargo test`.
5. **Repoint + verify the real consumer:** `setx ARCANE_SDK
   D:\dev\starworks\Arcane`; regenerate + rebuild Aphelyon (editor Rebuild
   button or premake+msbuild) against the new SDK. Re-register the engine
   path in the Hub (its engines list stores exe paths).
6. Push to `T3mps/Arcane` (`main`).

## Phase 2 -- reshape the game repo (only after Phase 1 verifies)

1. In Gacha: `git rm -r Arcane Client Tools` (+ the engine's ThirdParty
   subset that Server does not use), keep Server + shared scripts + docs +
   Setup wizard; Server premake: compile ArcaneCore from
   `os.getenv("ARCANE_SDK") .. "/ArcaneCore/src"` (fail loudly when unset,
   arcane.lua-style).
2. Subtree-merge `D:\dev\starworks\Aphelyon` -> `Game/`
   (`git remote add game <local path>` + `git merge -s ours --no-commit
   --allow-unrelated-histories` + `git read-tree --prefix=Game/`), then
   retire the sibling working copy.
3. Jenkinsfile: drop Arcane stages (a separate multibranch job now covers
   the engine); keep Server tiers; CLAUDE.md/AGENTS.md rewritten for the
   new shape; Setup wizard scope reduced to Server (+Game).
4. Push org main.

## Phase 3 -- CI

- **Jenkins:** new multibranch pipeline over `T3mps/Arcane`
  (StarworksBuilder needs collaborator access or a deploy key on the
  personal repo -- user grants); engine Jenkinsfile = both configs +
  ArcaneTests incl. `[gpu]` on windows-1 + the `--frames` scripted verify.
- **GitHub Actions:** Manifold2D-style windows workflow -- generate, build
  Debug+Release, run `~[gpu]` (hosted runners have no GPU).

## Risks / notes

- filter-repo REWRITES hashes -- the engine repo shares no commit identity
  with the org repo; memory/docs citing old short hashes remain valid only
  in the org repo. Accepted.
- The org repo's history (and the public engine repo's, via the docs copy +
  filtered history) is being published under T3mps/Arcane as PUBLIC --
  vendored third-party licenses ride along; add a NOTICE file.
- `vcpkg-triplets/` stays in the game repo (libpq); the engine's SDL3 vcpkg
  overlay triplet copies into the engine repo (its setup script needs it).
- Aphelyon's `engine.abi` / SDK flow are untouched -- only the env var value
  changes. The editor's Rebuild button works unchanged.
- Order is load-bearing: Phase 2 must not start until Phase 1's Aphelyon
  rebuild verifies, so there is never a moment with no working engine SDK.

## Testing

Phase 1: the four gates in step 4 + the Aphelyon rebuild in step 5.
Phase 2: Server workspace builds + fast suites green with ArcaneCore sourced
from the SDK; AccountTests optional (17 min, user's call). Desk: Hub opens
both projects; editor boots Aphelyon from the new SDK.

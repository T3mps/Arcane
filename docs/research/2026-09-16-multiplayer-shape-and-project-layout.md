# Multiplayer shape, the next game, and the project repo layout — decision record

**Date:** 2026-09-16 · **Status:** user rulings, binding on the replication/transport
arc's spec and on the Aphelyon repo relocation · **Extends:**
`2026-09-14-engine-ceiling-deadlock-and-box3d.md` (the north star; this record
adds the networking and repo-shape half) and
`../specs/2026-09-15-core-dll-split-design.md` §4/§6/§10 (the vocabulary this
record builds on). **Read this before the replication arc's brainstorm.**

Sources are PUBLIC material only: the Source SDK 2013 tree, Valve's
GameNetworkingSockets / Steam Datagram Relay releases and talks, Bungie's GDC
networking talks and Destiny 2 launch interviews, CIG's server-meshing
presentations, CCP's EVE architecture talks, Improbable's SpatialOS docs. No
internal repo layout of any of those studios is known; where this record says
"same shape" it means the shape their public material shows.

---

## 1. The two games this engine must serve

| | Aphelyon (now) | The next game (after Aphelyon) |
|---|---|---|
| Genre | Gacha; isometric overworld; Wakfu-style in-place turn-based combat | Massively multiplayer space / solar-system survival sim, Rust-like |
| Players per world | 1–4 (co-op combat at most) | **All players in one world — one mesh of servers presenting a single shard** |
| Net load | Small ordered command streams | Continuous entity replication, interest management, authority handoff between servers |
| Backend | Auth / Account / Combat services (TCP JSON, Postgres) — exist today | Gateway, persistence, economy, chat — the same *kind* of thing, more of it |

The ruling that follows from the table: **design the general replication model
and let Aphelyon be its thin first consumer.** A turn-based command relay would
serve Aphelyon and be worthless for the next game (§4). This is option 4 of the
2026-09-16 brainstorm ("engine-generic first").

## 2. The shape, and how the named AAA titles are shaped

**Arcane's shape after the Core-DLL split (Plans 1–3, 2026-09-15/16):**

- **One game module** (`Aphelyon.dll`) loaded by every host — the windowed
  clients *and* `ArcaneServer.exe`. Its systems carry role masks; a `Runtime`
  in `DedicatedServer` mode instantiates the server half of the same DLL.
- **`ArcaneServer.exe`** = the sim server: a Core-only host that loads the
  module and ticks an authoritative world headless, at a fixed step.
- **The services** (Auth / Account / Combat) = the identity, economy and lobby
  backend. They link `ArcaneCore.dll` for Net/Crypto/Log/Cli and know nothing
  about scenes or worlds (Core-DLL spec §6).

**CS2 and Deadlock (Source 2), from the public Source SDK and Valve's SDR/GNS
material:** one game codebase compiled into `client.dll` and `server.dll`, a
`shared/` tree compiled twice under `CLIENT_DLL` / `GAME_DLL`; authoritative
dedicated servers; snapshot delta replication against an acked baseline; client
prediction and lag compensation; transport = Steam Datagram Relay, whose
open-source core is **GameNetworkingSockets**; matchmaking / inventory / the
Game Coordinator are a separate service layer. **Same shape.** Arcane's role
masks are the runtime half of Valve's compile-time split; the Core-DLL spec §10
already names the compile-out as the later step.

**Destiny 2 (Bungie, Tiger), from GDC talks and launch interviews:** a hybrid —
dedicated Bungie hosts own encounter and combatant state, player movement is
client-authoritative and replicated peer-to-peer with server validation, and
the world is instanced into activity "bubbles", never one continuous space. A
very large services backend beside it. Same host-vs-services split; a
deliberately weaker authority model chosen for co-op-shooter feel. **Not the
model for the next game** (instanced, not single-shard).

## 3. What "all players in one mesh server" actually is

No shipped game runs all players in one process. Single shard is a cluster
wearing one address, and every public example has the same three pieces:

| Piece | EVE Online (CCP) | Star Citizen (CIG, "server meshing") | SpatialOS / Dual Universe |
|---|---|---|---|
| **Spatial partitioning** | solar systems assigned to server nodes; time dilation on saturation | game servers each simulate a region | workers own spatial chunks |
| **Authority handoff** | at system jumps (node boundaries) | entity authority migrates between servers as it crosses region bounds | authority delegation between workers |
| **Replication layer** | proxy layer in front of the nodes | a replication layer OWNS entity state, streams interest sets to clients and servers; persistence behind it (graph store) | interest-based entity streaming |
| Transport | plain TCP | custom UDP | custom |

Mapped onto Arcane: **many `ArcaneServer` processes, each a `Runtime` in
`DedicatedServer` mode owning a spatial partition; a replication layer between
them and the clients; services beside.** The first and third pieces exist
today. The second is the replication arc. The shape built for Aphelyon is the
substrate for the mesh, not a detour from it.

## 4. Four things the replication arc must make true (so the mesh stays possible)

1. **Authority is per ENTITY, not per world.** The Core-DLL spec §4 already
   predicts this (its survey: UE `ENetRole`, O3DE `NetEntityRole`, DOTS
   `GhostOwner`). If the first design makes "this Runtime owns everything in
   it" the only notion of authority, a mesh is a rewrite. If authority is a
   per-entity field from day one, a mesh is a *policy* for moving it.
2. **Replication is interest-based from day one.** A client subscribes to a
   set, not to a world. Aphelyon's co-op fight is a trivial interest set; the
   solar system is a spatial one; same mechanism.
3. **The net driver is a transport SEAM, not a transport.** GNS behind it for
   client links is a strong first driver (it is SDR's core; CS2/Deadlock ship
   on it; encryption, reliable + unreliable channels, fragmentation; no
   browser/WASM path — a later driver). Server-to-server links inside a
   datacenter are a *different* driver (EVE uses plain TCP). Choosing GNS
   decides the driver layer only; it never decides the shape above it.
4. **Serialization is engine-owned and quantized**, with delta compression
   against an acked baseline (Valve's snapshot model). Core, not the game.

None of these change the repo layout (§5). The replication layer and the mesh
coordinator are ENGINE features — the directional rule (generic capability in
Arcane; the game contributes vocabulary) applied to networking.

**Combat's role, still open (2026-09-16 brainstorm):** either Combat.exe hosts
a Core `Runtime` per match (one process: lobby + sim; reuses its SessionCache
token validation) or Combat stays a matchmaker that brokers to `ArcaneServer`
instances (per-match isolation; a handoff protocol). Either way the player's
session token is the credential for the sim connection, and the services keep
their TCP protocol; GNS, if chosen, sits under the sim connection only.

## 5. The Aphelyon repo relocation and layout (ruled 2026-09-16)

- `D:\dev\starworks\Gacha` is kept as-is for archaeology. The Aphelyon repo
  moves to **`D:\dev\starworks\Aphelyon`**, which inherits the
  `StarworksDev/Aphelyon` remote (Jenkins tracks it).
- **The repo root IS the Arcane project** (`Aphelyon.arcproj` at the root,
  like a `.uproject`): an Arcane project root is wherever the manifest lives
  (project-format spec §5 skeleton: `<Name>.arcproj`, `Source/`, `Content/`,
  `Config/`, `Plugins/`, derived `Binaries/ Intermediate/ Saved/`); arcbuild
  takes `--project <dir|.arcproj>`; nothing requires a subdirectory.
- **`Source/` is load-bearing in the engine** (arcane.lua compiles
  `Source/**` into the game DLL; the editor's Asset Browser lists `Source/` as
  the C++ rows; Create C++ Class writes there), so the services cannot simply
  be dropped under it today. **Layout chosen — option A, Unreal-style
  `Source/<Module>/`:**
  - `Source/Game/` — the game module, every net mode, → `Aphelyon.dll`
  - `Source/Services/` — Auth, Account, Combat, Common, their tests, `data/`,
    and their own premake workspace (→ `AphelyonServer.slnx`)
  - `Content/`, `Config/`, `Plugins/`, `ThirdParty/` (service vendored deps),
    `ci/`, `docs/`, `scripts/`, `Tools/`, `vcpkg-triplets/`, `Aphelyon.arcproj`,
    `premake5.lua` (the game workspace, arcbuild-driven)
- **Naming ruling:** NOT `Source/Client` + `Source/Server`. The game module is
  not client code — the same DLL is the dedicated server's simulation, and
  "Client" would lie about its most important file, worse over time. And
  "Server" for the TCP services collides with the spec's "sim server" =
  `ArcaneServer`. `Game` / `Services` stay honest as the shape evolves; if
  Combat later hosts a `Runtime`, it is still a service that hosts a Runtime.
- **SDK change required first (small):** `arcane_game_module(name, opts)`
  takes an explicit source directory (`{ source = "Source/Game" }`) instead of
  globbing `Source/`; ReferenceProject adopts `Source/Game/` to match; the
  arcbuild spec and the project-format spec §5 record it; the editor's Source
  rows / Create C++ Class learn the module subdirectory. Then the move is one
  commit. Path coupling in the Gacha tree is four files (`scripts/setup.ps1`,
  the wizard's Result screen, `ci/docker-compose.ci.yml`, `Jenkinsfile`);
  the git pack is ~111 MB, the rest of the tree is gitignored build output.
- **Still open (user's call):** fresh history vs `git filter-repo` copy;
  whether Plan 3's eight unpushed commits are pushed before the move (Arcane
  first) or ride the new repo's first push.

## 6. Rulings ledger (2026-09-16)

| # | Ruling | Rejected |
|---|---|---|
| N1 | Design the general replication model; Aphelyon is its thin first consumer | a turn-based command relay sized to Aphelyon |
| N2 | Per-entity authority + interest-based replication + a driver seam + engine-owned quantized delta serialization are REQUIRED of the replication arc's v1 | "the Runtime owns everything" as the only authority notion |
| N3 | GNS is a candidate first CLIENT transport driver, decided at the driver layer, after the shape | "use GNS" as the multiplayer design |
| N4 | The next game's single shard = many `ArcaneServer` partitions + a replication layer + services; the mesh coordinator and replication layer are engine features | a per-game networking stack |
| L1 | Aphelyon repo → `D:\dev\starworks\Aphelyon`, repo root = the Arcane project; `Gacha/` kept for archaeology | a monorepo wrapper (`Game/` + `Server/` subdirs) |
| L2 | `Source/Game/` + `Source/Services/` | `Source/Client/` + `Source/Server/`; `Source/Aphelyon/` |
| L3 | SDK gains an explicit game-module source dir before the move | a name-derived `Source/<name>/` convention |

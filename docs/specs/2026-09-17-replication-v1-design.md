# Replication v1 — per-entity authority, interest-scoped snapshots, a driver seam

**Date:** 2026-09-17
**Status:** Design, approved in brainstorm 2026-09-17. SPEC ONLY by the binding
order (`docs/research/2026-09-16-direction-and-sequencing.md`, step 1);
implementation is step 5 of that order (after F4, F3, F5, the hygiene wave and
cvars). Plans are outlined in §11 and get written when step 5 opens.
**Requirements this satisfies:** `docs/research/2026-09-16-multiplayer-shape-and-project-layout.md`
§4 (N1–N4: general model first, per-entity authority, interest-based from day
one, a driver SEAM, engine-owned quantized delta serialization).
**Prior law this builds on:** `docs/specs/2026-09-15-core-dll-split-design.md`
§4 (`NetMode`, `RoleMask`, `HasAuthority()`; "replication and transport are not
this arc"), §5 (hot reload refused while a driver is active), §6
(`ArcaneServer.exe` is the sim server, the services are not), §10 (compile-out
is a later follow-on); `docs/specs/2026-09-13-game-module-boilerplate-design.md`
(`ARCANE_GAME_MODULE`, `RegisterSystem(mask, phase)`); the project-format spec
(`.arcproj`, GUID assets).
**Standing rule obeyed:** every design states what Source 2 / Deadlock does and
why we match or diverge (§9).

---

## 0. What this arc is, in one paragraph

The engine gains entity replication for **multiplayer sessions**: a session is
one `ArcaneServer` process running one authoritative `Runtime`, clients connect
with a ticket, and entities carrying a `NetIdentity` component are mirrored to
every connection whose interest set includes them, as quantized deltas against a
per-connection acked baseline, at a send rate below the sim rate. Authority is a
**per-entity peer id**, so "the server owns everything" is the default policy,
"the owner owns its avatar" is an opt-in policy for housing and co-op, and
moving an entity between server partitions later is the same message. The
transport is a seam (`INetDriver`) modeled on GameNetworkingSockets' semantics
with Loopback and TCP drivers first and GNS second. Nothing here touches the
single-player base game, which needs no replication (§1.2).

## 1. Scope and boundary

### 1.1 The consumer (user ruling 2026-09-17)

Aphelyon is **single-player by default**, the feel of Honkai: Star Rail.
Multiplayer is **instanced sessions**: housing (invite friends, walk around),
duels requested at a house, PvP queues, and many farmable game modes, several
multiplayer (a Clash-Royale-style lane mode, an autochess, a 2D fighting game,
the base isometric grid combat with a friend). The next game after Aphelyon is
a single-shard massively multiplayer survival sim (shape doc §1, §3).

So the general model serves two very different loads from one mechanism:
small instanced sessions now, spatial partitions later. Ruling N1 stands:
design the general model; Aphelyon's housing session is its thin first consumer.

### 1.2 The single-player base game is OUT of this arc (the HSR route)

Research into the genre (sources in Appendix B) settled how the always-online
gachas actually split authority:

| Game | Movement | Combat | Per-entity authority | PvP |
|---|---|---|---|---|
| Honkai: Star Rail | client reports its position; server records it | server sends the full battle setup (stage, waves, avatars with stats, buffs, round limit, `logic_random_seed`); the **client simulates the whole fight** and reports end status + statistics | none — no co-op, nothing replicated | none |
| Genshin Impact | client-authoritative (`CombatInvocationsNotify` entity moves) | client-authoritative hits, relayed to peers; selective server computation (fall damage) | **yes**: `AuthorityChange { entity_id, authority_peer_id, EntityAuthorityInfo }` migrates monsters between player clients; world is host-owned | none |
| Wuthering Waves | client pushes `MovePackagePush`; server queues, no validation seen | client sends `DamageExecuteRequest` (which hit landed); **server computes damage** from attributes and pushes `AttributeChangedNotify` | host-based co-op, up to 3 | none |

The genre pattern: **client simulates, server owns durable state, validates
selectively, relays to peers.** None of the three runs a server-authoritative
simulation; kernel anti-cheat carries the integrity load.

Consequence, ruled: Aphelyon's single-player overworld and grid combat take the
HSR route. Combat/Account issue the battle setup plus a seed; the client
simulates; the server accepts a result it CAN replay because Arcane's fixed
step is byte-identical at any time scale (RunLoop). That is a services-protocol
feature (a Combat/Account follow-up, plan 3 in §11) and uses nothing in this
spec. The replication layer exists for the multiplayer sessions only.

### 1.3 The finding the sequencing doc asked for

`2026-09-16-direction-and-sequencing.md` says a finding that "the module cannot
serve every net mode" would outrank steps 2–4. **No such finding.** One game
module serves every net mode: `RoleMask` picks which systems exist in a
`Runtime` (Core-DLL §4), the per-entity `NetRole` (§2.4) picks the branch inside
a system, the presentation half is Client-masked and reads ghost state, and
nothing below forces a client-only or server-only build. The server compile-out
stays the later follow-on the Core-DLL spec §10 names.

### 1.4 Where it lives

Everything in this spec is **ArcaneCore** (the directional rule: generic
capability in the engine; the game contributes vocabulary). New directories:
`ArcaneCore/src/Arcane/Net/Bits/` (wire primitives), `ArcaneCore/src/Arcane/Net/Drivers/`
(Loopback, Tcp, Simulation; Gns in plan 2), `ArcaneCore/src/Arcane/Replication/`
(identity, snapshot, endpoint, interest, handshake, messages). The existing
`Arcane/Net/{TcpSocket,Protocol,RateLimiter}.hpp` (the services' text protocol,
header-only and unexported today) is untouched; the Tcp driver reuses its
socket helpers and gets its own binary framing. The services (Auth, Account,
Combat) stay scene-agnostic and link nothing new; Combat's broker work is
Aphelyon-side (plan 3).

## 2. Vocabulary and the authority model

### 2.1 `PeerId`

`std::uint16_t`, session-local. **Peer 0 is the session's server Runtime.**
Clients are 1 and up, assigned by Combat in the ticket (§3.2). A second server
partition in the next game is a peer too — that is what makes the mesh a policy
rather than a rewrite. `kInvalidPeer = 0xFFFF`.

### 2.2 `NetId`

`std::uint32_t`, session-local, minted by **peer 0** when a replicated entity
first appears (a client never mints, even under owner authority — the server
spawns, then grants, §2.6; a server partition mints with its own prefix later);
`0` is invalid. **Astra `Entity` handles never
cross the wire** (24-bit index + 8-bit version, process-local, recycled) and
`Identity::id` GUIDs are authored-only and 128-bit + string, so neither is the
wire identity. Each Runtime keeps a `NetId → Entity` table on its endpoint. The
top 8 bits are reserved as a partition prefix and are always `0` in v1; the
allocator is `partition << 24 | counter` so the mesh's allocator is the same
code with a nonzero prefix.

### 2.3 `NetIdentity` — the replicated-entity component

```cpp
struct NetIdentity            // Core, in the EngineComponentRoster (append-only)
{
    NetId  id        = 0;     // minted by the authority; never authored
    PeerId authority = 0;     // who simulates it (peer 0 = the server)
    PeerId owner     = kInvalidPeer; // which player it belongs to; may differ
    NetFlags flags   = {};    // OwnerAuthoritative, OwnerOnly (interest), ...
    std::uint64_t spawnKind = 0; // game-defined hash; decorates the ghost client-side
};
```

Its **presence** means "replicated". `authority` and `owner` are distinct on
purpose: a server-simulated unit owned by a player (the lane mode) has
`authority = 0, owner = 3`; a housing avatar under owner authority has
`authority = owner = 3`. `Serializable(false)` for the scene JSON — it is never
authored into an `.arcscene`; the game module adds it at spawn.

### 2.4 `NetRole` — derived, never sent

Per Runtime, per entity, computed from `NetIdentity` and the endpoint's local
`PeerId`:

| Role | When | Systems do |
|---|---|---|
| `Authority` | `authority == localPeer` | simulate; the endpoint snapshots it |
| `Autonomous` | `owner == localPeer && authority != localPeer` | **reserved for prediction** (§7); in v1 identical to `Proxy` |
| `Proxy` | otherwise | apply incoming state; the presentation interpolates |

`Runtime::HasAuthority()` stays the world-level predicate ("am I some kind of
server"); `NetRole` is the entity-level one every surveyed engine adds beside it
(UE `ENetRole`, O3DE `NetEntityRole`, DOTS `GhostOwner`). Both are queried live
per call site (`Runtime::RoleOf(Entity)`), never cached across a tick.

### 2.5 Authority policy — a session-config value

```cpp
enum class AuthorityPolicy : std::uint8_t { ServerOnly, OwnerAvatars };
```

- **`ServerOnly`** (default, every game mode): every entity's `authority` is
  peer 0; a spawn that asks otherwise is refused with a diagnostic.
- **`OwnerAvatars`** (housing, co-op): an entity spawned with
  `NetFlags::OwnerAuthoritative` gets `authority = owner`. The server **relays**
  that owner's deltas to the other peers and never writes that entity's
  replicated fields itself. This is Genshin's `authority_peer_id` generalized
  and it is why housing needs no client prediction.

A **server-side validation hook** for owner-authority state (between apply and
re-snapshot: speed and teleport bounds, à la Rust's anti-hack convars) is a
reserved seam (§7), not built.

### 2.6 `AuthorityChange` — the one handoff message

`AuthorityChange { NetId, PeerId newAuthority, full replicated state }`,
reliable. Used for: granting owner authority at spawn under `OwnerAvatars`;
revoking it when the owner disconnects (config: despawn, or hand to peer 0);
and, later, moving an entity between server partitions. One mechanism, three
policies. Genshin's `EntityAuthorityChangeNotify` carries exactly this shape
(entity, new peer, the state the new authority needs to continue).

## 3. Sessions, tickets, and Combat's broker role

### 3.1 A session is one process (user ruling 2026-09-17)

One `ArcaneServer.exe`: one `Runtime` in `NetMode::DedicatedServer`, one
replication endpoint, one listening driver. **Process lifetime is session
lifetime.** Chosen over "one ArcaneServer hosts N sessions" because: lifecycle
is spawn/run/exit with no session table and no half-dead states; a crash, hang
or runaway module bug kills one house or match (the hang watchdog and the
diagnostics capture are per-process today); `ServerApp` ticks exactly one
Runtime and multi-Runtime scheduling under load is unbuilt; the N-Runtimes
primitive was built for PIE's embedded server. Costs: memory per process (DLL +
content loaded N times) and a few hundred ms spawn latency hidden behind
matchmaking. N-per-process stays a **deployment policy** with a memory-bound
trigger (§7): `ServerApp` learns a session table; nothing above the seam
changes.

### 3.2 Tickets — verified offline, never a call to Auth

Combat spawns the process with `--session <id>` and a per-session secret in the
environment variable `ARCANE_SESSION_SECRET` (never argv: process lists are
readable). To admit a player Combat mints

```
Ticket { sessionId, playerId, peerId, kind ∈ {Player, Control}, expiresAt }
mac    = HMAC-SHA256(secret, canonical bytes of the ticket)
```

using Core's existing `Arcane::Crypto` (HMAC + constant-time compare, already
what the services' internal RPC uses), and hands the client the address plus
the ticket. `ArcaneServer` verifies tickets against its secret and the expiry;
it never calls Auth or Account, so the sim server keeps the Core-DLL §6
boundary (no services vocabulary). Expiry default 60 s; a ticket admits one
connection.

### 3.3 The Control peer

A ticket of kind `Control` admits Combat itself as a privileged peer over the
same driver and endpoint. Control receives session **events** (`PeerJoined`,
`PeerLeft`, `SessionResult { payload }`) and may send **admin commands**
(`EndSession`, `KickPeer`). The game module declares the result through the
Runtime (`Runtime::EndSession(payload)`); the endpoint forwards it to Control and
the process exits after a drain interval. This is the channel a mesh coordinator
uses later and it works across boxes where a report file would not. The
`--report` census (Core-DLL §6) stays for the witness (§8) and gains the same
fields.

### 3.4 Lifecycle

```
Combat: spawn ArcaneServer --listen --session S  (secret in env)
ArcaneServer: boot project, load module, boot the session's scene, listen
Combat: connect as Control
players: connect with Player tickets → handshake (§4.7) → ghosts stream in
play; late join and leave are ordinary events
module: EndSession(result) → Control gets SessionResult → drain → exit 0
```

A leaving owner's owner-authority entities are despawned or handed to peer 0
per session config (§2.6). An idle session (no Player peers for `--idle-timeout`)
exits with a distinct code so Combat can reap it. Combat keeps matchmaking,
invites, housing ownership and the PvP queues; it links nothing new.

### 3.5 Connection ↔ player binding

The endpoint keeps, per connection: `PeerId`, `playerId`, ticket kind, state
(`Handshaking, Live, Draining`), the last snapshot tick acked, the interest set,
the byte budget and its stats view. The game module reads the peer list from
the Runtime (`Runtime::Peers()`) and spawns avatars with `owner` set; it never
touches connections or the driver. `EngineContext` grows `localPeer` (an ABI
bump, cheap).

## 4. The snapshot pipeline

### 4.1 Tick contract

`RunLoop` gains `SimTick` (`std::uint32_t`, incremented once per fixed step,
`Rebind` preserves it). It is NOT Astra's `Registry::CurrentTick()` (change-
detection time, explicitly never serialized). Every snapshot, command and event
carries a `SimTick`.

| Value | Default | Where |
|---|---|---|
| sim rate | 60 Hz (`RunLoop::Config::fixedHz`, `--fixed-dt`) | runtime value, never a constant (Core-DLL §6) |
| send rate | 20 Hz (every 3rd sim tick) | session config |
| snapshot ring | 32 snapshots (1.6 s at 20 Hz) | session config |
| client interpolation delay | 2 send intervals (100 ms) | client config |

Clients estimate the server clock as `lastSnapshotTick + RTT/2` (RTT from driver
stats) and stamp commands with the server tick they target. In v1 the stamp
orders commands; it is the prediction seam (§7).

### 4.2 One world snapshot per send tick — the seam

At each send tick the endpoint walks every entity with `NetIdentity` whose
`authority` is the local peer, runs the quantized field visitor (§4.6) over its
replicated components, and stores the result in the ring keyed by `SimTick`:

```
WorldSnapshot { SimTick tick; FlatMap<NetId, EntityRecord> entries; }
EntityRecord  { NetIdentity header; per-component quantized field blocks; }
```

Astra change tracking skips entities untouched since the previous snapshot
(exact for `Transform`, which opts into `AstraChangeTracked`; per-chunk coarse
for everything else — a documented false positive, never a false negative; a
plan rebuild resets `lastRun` and forces one full re-walk, bursty but safe).
Skipped entities carry forward the previous record by reference.

**Per-connection encoders read snapshots, never the registry.** That is the
refinement that keeps the next game's replication-layer proxy (Star Citizen's
shape, shape doc §3) a deployment change: the ring can be shipped to another
process and the encoders run there. v1 runs both in one process.

### 4.3 Per-connection delta against an acked baseline

For each connection the encoder diffs the current snapshot against the snapshot
the connection last acked (Source 2's per-client `m_pLastSnapshot`; DOTS keeps
up to three baselines — one is enough for v1). **Quantize first, compare
quantized values**, so the delta is exact and an unchanged field costs one bit.
A baseline older than the ring → full snapshot (baseline tick 0). Snapshots go
`Unreliable` on drivers that have it and `Reliable` otherwise (§5.1); the
client's uplink packet carries `SnapshotAck { tick }`.

Owner-authority entities flow the same way **upward**: the owner runs the same
encoder against the server's acked baseline for the entities it owns; the
server applies, re-snapshots, and relays to the other peers on the next send
tick. One encoder, one decoder, both directions.

### 4.4 Interest, priority, budget

```cpp
struct IInterestPolicy { virtual void Evaluate(const Connection&, const WorldSnapshot&, InterestSet& out) = 0; };
struct IPriorityPolicy { virtual float Score(const Connection&, const EntityRecord&, SimTick lastSentTick) = 0; };
```

- **Interest** says which `NetId`s a connection MAY see. v1 ships `SessionWide`
  (every replicated entity — a house, a duel), the per-entity `NetFlags::OwnerOnly`
  (only its owner sees it: private state), and `Spatial` (radius around the
  peer's avatar; proven in ReferenceProject rather than by Aphelyon, because
  N2 says interest-based from day one and the solar system is a spatial set).
- **Priority** scores the interested entities per connection (distance, time
  since last sent, owner first) and the encoder fills a per-connection **byte
  budget** per send tick in score order; what does not fit waits, its score
  rising. Aphelyon's budget never fills; the tests force it to (§8). The budget
  reads the driver's stats (pending bytes, loss) so a congested link degrades to
  fewer entities per tick, not to a growing queue.

Both policies are session-config choices, installed by the host from the
session's config; the game module may register its own (`Runtime::SetInterestPolicy`),
in the engine's vocabulary.

### 4.5 Spawn, despawn, commands, events — the reliable channel

| Message | Direction | Payload |
|---|---|---|
| `Spawn` | server → peers | `NetIdentity`, `spawnKind`, the full replicated state of every replicated component (type hash + fields); only peer 0 spawns in v1 (§2.2) |
| `Despawn` | server → peers | `NetId` |
| `AuthorityChange` | server → peers | §2.6 |
| `Command` | client → server | `SimTick target`, game-defined type hash, fields |
| `Event` | server → client(s) | `SimTick`, game-defined type hash, fields |
| `SnapshotAck` | client → server | `SimTick` |

There is no prefab system in the engine (the closest is the editor clipboard's
`Edit::InstantiateSubtrees`, in ArcaneClient.dll and unreachable from a server),
so a `Spawn` carries its full component set. The **`spawnKind`** hash is the
game's vocabulary: the module's Client-masked system observes `Added<NetIdentity>`
and decorates the ghost with presentation components (sprite, animation) by
kind. That is DOTS' ghost-prefab split in spirit without a prefab asset. On the
receiving side the endpoint instantiates components exactly the way the scene
loader does (`AddComponentByTypeName`'s sequence: hash → descriptor →
default-construct → visitor → `AddComponentByID`), through the negotiated table
(§4.7), never `Registry::Load` (whole-world replace, and explicitly not a
security boundary).

Commands and events are reliable ordered and serialized by the same visitor
over game-defined reflected structs; the engine owns the bytes, the game owns
the types.

### 4.6 Serialization — reflection-driven, engine-owned, quantized

New Core primitives, `ArcaneCore/src/Arcane/Net/Bits/`: `BitWriter`/`BitReader`
with a **defined little-endian byte order** (Astra's binary archive refuses on
endianness mismatch and has no byte-swap; the wire cannot), varint + zigzag,
quantizers (`float → [min,max] in N bits`, smallest-three quaternion, half
floats), and a `Checksum::Portable` trailer where a message is worth it.

Wire policy is declared on the existing `ASTRA_REFLECT` rows through attributes,
beside `Range`/`Tooltip`/`Serializable`:

```cpp
ASTRA_REFLECT_FIELD(position, Net::Replicated{}, Net::Quantize{-4096.f, 4096.f, 0.01f});
ASTRA_REFLECT_FIELD(rotation, Net::Replicated{});        // glm::quat → smallest-three, 9 bits/component default
ASTRA_REFLECT_FIELD(hp,       Net::Replicated{});        // integers → varint
ASTRA_REFLECT_FIELD(name,     /* no Net:: attribute */); // never on the wire
```

A quantized `IFieldVisitor` backend (`ReflectionNet.hpp`) sits beside the JSON
one (`ReflectionJson.hpp`); Astra's `FieldVisitor.hpp` says a format backend
"lives entirely in the consumer", and this is one. A component needing a
hand-packed layout may implement `NetSerialize(BitWriter&)` / `NetDeserialize(BitReader&)`
instead; the descriptor prefers the explicit pair when present (the same
precedence rule Astra's binary archive uses for `Serialize`, and the same IM-8
trap: a trivially-copyable type with a hook must route through the hook).
`WorldTransform` and `PhysicsInterpBuffer` are never replicated: derived and
presentation-local respectively; the client rebuilds interpolation state from
received `Transform`s.

### 4.7 Handshake — refuses, never aliases

```
Hello   { protocolVersion, pluginABI, projectGuid, contentStamp, ticket }
Welcome { peerId, sessionId, simHz, sendHz, bootSceneGuid,
          [ { typeHash, wireIndex, schemaHash } … ] }   // every replicated component type
Refuse  { reason enum, detail string }
```

`ComponentID` is a **first-touch ordinal** (`EngineRoster.hpp`: "the ORDER of
the list IS the engine's id numbering") and differs between processes that
load modules in different orders; only `TypeID<T>::Hash()` (XXHash64 of the
type name) is cross-process stable, and `TypeIdentity::rttiName` is explicitly
not a wire discriminator. So the wire carries `wireIndex` (dense, per session)
and the `Welcome` table maps it to a type hash; the client resolves each hash
through `ComponentRegistry::GetComponentIDFromHash`. `schemaHash` covers the
type's replicated field names, types and attributes. A missing hash or a
schema mismatch **refuses the connection with a named diagnostic** — the
cross-process form of `VerifySharedTypeContext`, and the Camera-aliased-
Transform failure class (`ProjectHost.hpp`'s two war stories) is why it is a
refusal and never a best-effort mapping. `contentStamp` is guard G3's cooked
artifact format stamp; a mismatch refuses too, because the client boots the
`bootSceneGuid` from ITS content.

## 5. The driver seam

### 5.1 `INetDriver`, widened

Today: one method, `IsActive()`, one fake in ArcaneTests. It becomes a
message-oriented contract shaped on GameNetworkingSockets' semantics so no
driver's vocabulary leaks above it:

```cpp
struct INetDriver
{
    virtual ~INetDriver() = default;
    virtual bool IsActive() const noexcept = 0;                       // unchanged: the hot-reload gate
    virtual Result Listen(const Endpoint&) = 0;
    virtual Result Connect(const Endpoint&) = 0;
    virtual void   Close(ConnectionId, CloseReason) = 0;
    virtual void   Poll(INetEventSink&) = 0;   // Connected / Disconnected(reason) / Message(conn, bytes)
    virtual SendResult Send(ConnectionId, std::span<const std::byte>, SendFlags) = 0; // Reliable | Unreliable, NoDelay
    virtual ConnectionStats Stats(ConnectionId) const = 0;             // rttMs, lossPct, pendingBytes
    virtual Capabilities Caps() const noexcept = 0;                    // unreliable, encrypted, maxMessageBytes
};
```

- **Message-oriented**, not stream: fragmentation and reassembly are the
  driver's job; the endpoint sees whole messages ≤ `maxMessageBytes`.
- **`Poll` runs on the sim thread** at the top of each fixed step (Source runs
  networking in the main loop); no separate net thread in v1. GNS's internal
  threads deliver on `Poll`.
- A driver without `Caps().unreliable` sends `Unreliable` as reliable and counts
  it in stats; the endpoint is written once and never branches on driver type.
- `Runtime::SetNetDriver` keeps its non-owning contract; the host owns the
  driver and the hot-reload refusal (`PluginHost::RefuseReloadForActiveNetDriver`)
  inherits unchanged.

### 5.2 Drivers

| Driver | Plan | What |
|---|---|---|
| `Loopback` | 1 | in-process queues between two Runtimes on one `ProcessContext`; PIE's `EmbeddedServer` and every in-process test; `unreliable = true` (drops nothing) |
| `Tcp` | 1 | Core's existing socket helpers (`TcpSocket.hpp`: listen, connect-with-timeout, keepalive, send-all) plus a NEW length-prefixed **binary** framing (not the services' `LENGTH:` text framing); plaintext — the same posture as the services today; **stays as the server-to-server driver** in the mesh (EVE's choice) |
| `Simulation` | 1 | wraps any driver; injects loss, latency, jitter, reordering, duplication from a config; tests only |
| `Gns` | 2 | GameNetworkingSockets via the vcpkg port (protobuf + BCrypt on Windows), behind a premake option; brings the unreliable channel, encryption, MTU discovery, and later Steam Datagram Relay; lands inside this arc so the seam is proven against two real drivers before any real-time mode ships |
| WebTransport | reserved | the eventual WASM target (GNS has no browser path) |

The seam is modeled on GNS precisely so the TCP-first sequencing costs nothing:
`SendFlags`, the connection state machine, per-connection stats and the
message size cap are GNS's model, and TCP is the degenerate reliable-only case.

## 6. Hosts and the game module

### 6.1 `ArcaneServer`

New flags via `Arcane::Cli`: `--listen <addr:port>`, `--session <id>`,
`--max-peers N` (default 8), `--idle-timeout <s>`. `ARCANE_SESSION_SECRET` from
the environment; refused if `--listen` is given without it. The link-line gate
is unchanged (no `ArcaneClient.dll`). `ServerApp::Run` gains: create the driver,
`Listen`, attach the endpoint to the Runtime, `Poll` at the top of each fixed
step, send at each send tick, exit on `EndSession` / Control / idle. Exit codes:
0 result delivered, a distinct code for idle-reaped, the existing codes for
boot failures. The `--report` census gains `peersJoined, snapshotsSent,
bytesPerPeer, refusals, result`.

### 6.2 `ArcaneRuntime`

`--connect <addr:port> --ticket <token>` sets `NetMode::Client`, attaches a
`Tcp` driver, runs the handshake, boots the `Welcome`'s scene from local
content, and lets ghosts spawn into it. Without `--connect` nothing changes.
`--headless --connect` is the witness client (§8).

### 6.3 Editor PIE (`PlayTopology`)

| Topology | Today | With this arc |
|---|---|---|
| `Standalone` | one Runtime | unchanged; no driver |
| `EmbeddedServer` | second Runtime seeded from a snapshot, primary flipped to Client | the two Runtimes talk over **`Loopback`** — the real replication path in-process; the seed-from-snapshot step is replaced by the handshake + spawns |
| `ListenServer` | primary flipped to ListenServer | unchanged one-world dual role (Core-DLL §4), plus a listening `Tcp` driver for remote clients |
| `ClientOnly` | primary flipped to Client; editor spawns `ArcaneServer.exe` | the spawned server gets `--listen`, the primary `Connect`s with a ticket the editor mints (the editor holds the secret it passed) |

### 6.4 The game module

Registration is unchanged (`ARCANE_COMPONENT`, `RegisterSystem(mask, phase)`).
The module:

- adds `Net::` attributes to reflect rows it wants on the wire;
- spawns replicated entities by adding `NetIdentity { owner, flags, spawnKind }`
  — the engine mints `id` and applies the session's authority policy;
- branches on `Runtime::RoleOf(entity)` inside systems where server and client
  code differ per entity (and on `RoleMask` where whole systems differ);
- reads `Runtime::Peers()`, receives `Command`s through a Runtime-owned queue
  drained at the top of the fixed step, sends `Event`s through the Runtime;
- observes `Added<NetIdentity>` in a Client-masked system to decorate ghosts;
- calls `Runtime::EndSession(payload)` when the mode ends.

The engine owns every byte on the wire; the module never sees a driver, a
connection or a packet. `EngineContext` gains `localPeer`; ABI bumps.

## 7. Reserved seams — named, not built

| Seam | Relies on (built in v1) | Trigger |
|---|---|---|
| Client prediction + reconciliation | tick-stamped `Command`s, `NetRole::Autonomous`, the snapshot ring | the first real-time PvP mode that is not command-driven |
| Rollback / deterministic lockstep (the 2D fighter) | the same input stream; a **per-entity partial snapshot + restore** Astra lacks (whole-registry only today) — named here so prediction and rollback share it | the fighter is scheduled |
| Partition handoff (the mesh) | `AuthorityChange` between server peers, the `NetId` partition prefix | the next game |
| Replication-layer proxy process | encoders read the snapshot ring, never the registry | the next game's partition count |
| Owner-authority validation hook | a server pass between apply and re-snapshot | the first cheat report from housing |
| GNS + Steam Datagram Relay | plan 2; SDR when on Steam | plan 2 / Steam release |
| WebTransport driver | the seam | the WASM target |
| Server compile-out of Client-masked systems | Core-DLL §10 | the first shipping server build |
| N sessions per process | `ServerApp` session table | memory, not CPU, is the per-box limit |
| Lag compensation (rewind hit tests) | the snapshot ring | a hitscan mode |

## 8. Testing

- **Unit (ArcaneTests, rapidcheck properties where one exists):** `BitWriter`/`BitReader`
  round trips for every primitive; quantizers at range edges and NaN refusal;
  varint/zigzag; smallest-three quaternion error bound; delta encode/decode
  against a baseline is the identity; the schema hash is order-stable.
- **In-process, two Runtimes over `Loopback`** (the `EmbeddedServer` shape, one
  `ProcessContext`, shared `ComponentRegistry`): spawn on the server → ghost on
  the client within two send ticks with byte-equal quantized state;
  `OwnerAvatars`: the owner's delta reaches a third Runtime; despawn;
  `AuthorityChange` on owner disconnect (both configs); `Spatial` interest
  drops and re-adds an entity as the avatar moves; a budget of N bytes forces
  drops and priority recovers them; handshake refusal on a missing type hash,
  a schema mismatch, a content stamp mismatch, an expired ticket, a bad MAC.
- **Adversarial:** the same suite under `Simulation` at 10 % loss, 80 ms jitter,
  reordering; convergence within the ring depth; the full-snapshot fallback
  fires when the baseline ages out.
- **Two-process witness:** `ArcaneServer --listen --session --report` and
  `ArcaneRuntime --headless --connect --ticket --frames N --report` on
  ReferenceProject; the two reports are the assertions (peers joined = 1,
  snapshots sent > 0, refusals = 0, the client's ghost count = the server's
  replicated count). Jenkins runs it on `windows-1` with the Game stage; the
  automation verdict vocabulary (`docs/specs/2026-09-01-automation-verdict-vocabulary-design.md`)
  governs exit codes.
- **Hot reload:** `MultiRuntimeReloadTest`'s fake driver is replaced by a live
  `Loopback` and the refusal is re-proven.
- **Baselines:** the `~[gpu]` test-count baselines are re-booked at the plan's
  close, as every arc does.

## 9. Source 2 / Deadlock comparison (the standing rule)

**Matched:** server-authoritative simulation by default; one world snapshot per
tick, per-client delta against the client's acked snapshot; quantized fields;
a send rate below the sim rate (`cl_updaterate`); client interpolation delay
(`cl_interp`); a reliable side channel for events; GNS as the transport;
networking polled on the main loop.

**Diverged, with reasons:**

| Source 2 | Arcane | Why |
|---|---|---|
| hand-written SendTables / `DEFINE_SCHEMA` networked vars | reflection attributes on `ASTRA_REFLECT` rows | Astra already reflects every component; a second declaration would drift |
| the server is always the authority | per-entity `authority` peer id + a session policy | owner-authority sessions (housing, the genre norm) and partition handoff are policies, not forks |
| `CLIENT_DLL` / `GAME_DLL` compile-time split | runtime `RoleMask` + `NetRole` | Core-DLL §4/§10: the runtime half now, compile-out later |
| string tables for models/sounds | none | assets are GUIDs already |
| prediction + lag compensation shipped | reserved (§7) | nothing in the first consumer needs them; the seams are in |
| entity index is 11 bits (`MAX_EDICTS`) | 32-bit `NetId` with a partition prefix | the mesh |

No Source code is copied; the shape is (north-star rule).

## 10. Non-goals

| Out | Why / trigger |
|---|---|
| The single-player base game's combat and overworld | the HSR route, §1.2 — a services follow-up |
| Anything in §7 | reserved with triggers |
| Encryption on the `Tcp` driver | GNS brings it (plan 2); the services are plaintext today too and share the fix |
| A prefab/spawn-template asset | `Spawn` carries full state + `spawnKind`; a prefab arc is its own spec |
| Voice, text chat, presence, friends | services, not replication |
| Matchmaking, ELO, queues, housing ownership | Combat's domain (plan 3), unchanged shape |
| A separate net thread | `Poll` on the sim thread; revisit at the mesh |
| Linux | `ArcaneServer` is the first Linux target (Core-DLL §6); the lane is the Linux/CI milestone |

## 11. Plans (written when step 5 opens)

1. **Plan 1 — Core replication + Loopback + Tcp + hosts.** Wire primitives;
   `NetIdentity`, `NetRole`, `AuthorityPolicy`; `SimTick`; the snapshot ring and
   per-connection encoder/decoder; interest, priority, budget with the v1
   policies; `Spawn`/`Despawn`/`AuthorityChange`/`Command`/`Event`/`SnapshotAck`;
   the handshake and type-table negotiation; `INetDriver` widened; `Loopback`,
   `Tcp`, `Simulation`; `ArcaneServer --listen`, `ArcaneRuntime --connect`; the
   PIE mapping; the ArcaneTests suites and the two-process witness; ABI bump;
   `EngineRoster` append.
2. **Plan 2 — GNS.** The vcpkg port behind a premake option, the `Gns` driver,
   the same suites run against it, the Jenkins agent provisioning line.
3. **Plan 3 — Aphelyon (in the Aphelyon repo).** Combat's broker: spawn and
   lease `ArcaneServer`, mint tickets, the Control peer, housing invites and
   the duel-from-a-house flow; the housing session as the consumer proof; and
   the HSR-route battle setup + seed + replay validation on Combat/Account.

Order of the plans is fixed; each is one implementation session set with its
own review waves. The spec's rulings ledger (§12) is what a plan may not
re-open.

## 12. Rulings ledger (2026-09-17)

| # | Ruling | Rejected |
|---|---|---|
| R1 | Aphelyon's multiplayer = instanced sessions (housing, duels, queued modes); the base game is single-player | a shared overworld |
| R2 | The base game takes the HSR route (client simulates, server seeds + replay-validates) — OUT of this arc | every solo fight as a server session |
| R3 | Authority = a per-entity **peer id**; `ServerOnly` default, `OwnerAvatars` opt-in per session | server-only with prediction in v1; client-authoritative-with-relay everywhere |
| R4 | Rollback/lockstep: seam reserved (input stream, per-entity partial snapshot named), not built | designing both regimes now |
| R5 | One `ArcaneServer` process per session; Combat brokers | N sessions per process now; Combat hosting a Runtime |
| R6 | Approach A: one snapshot per send tick as the seam + per-connection acked-baseline deltas + interest/priority/budget | property replication with RPCs, reliable by default |
| R7 | `INetDriver` modeled on GNS; `Loopback` + `Tcp` in plan 1, `Gns` as plan 2 of THIS arc | GNS first; an in-house UDP driver |
| R8 | Reflection-attribute serialization with an explicit `NetSerialize` escape hatch | hand-written per-type send tables |
| R9 | The handshake refuses on any type/schema/content mismatch | best-effort mapping |
| R10 | Control peer over the session driver for Combat ↔ session events | a result file Combat polls |

## 13. Costs and risks, stated plainly

- **Astra has no per-entity partial snapshot.** Prediction and rollback both
  need one; v1 does not, and names it (§7) so the two later regimes share it.
- **GNS's vendor cost is real:** protobuf, a crypto backend, a CMake build
  under a premake tree, its Linux build later. Plan 2 pays it once; the seam is
  written so plan 1 never sees it.
- **`Tcp` is plaintext** and head-of-line blocks the snapshot stream. Acceptable
  for 2–8 peers in housing and command-driven modes; not for a real-time PvP
  mode, which is plan 2's trigger.
- **One process per session costs memory** (module + content per process).
  N-per-process is the trigger-guarded policy (§7).
- **Change tracking is coarse for everything but `Transform`;** a chunk with
  one changed entity re-walks its neighbours. Correct, not optimal; the exact
  per-entity opt-in exists if a hot component needs it.
- **The services' token posture is inherited:** tickets and the services'
  session tokens both cross plaintext TCP today. GNS fixes the sim side; the
  services' TLS is a separate item.
- **A stale line:** `2026-09-16-direction-and-sequencing.md` says
  `PreviousTransform` carries TAA's motion data; that component was deleted in
  Astra adoption Plan 2 (ABI 27). Unrelated to this arc; noted so F3/F5 do not
  build on it.

---

## Appendix A — existing code this spec builds on (survey 2026-09-17)

| Piece | Where | State |
|---|---|---|
| `NetMode`, `RoleMask`, `RolesOf`, `RoleMatches`, `SystemFactoryTable` | `ArcaneCore/src/Arcane/Plugin/SystemFactory.hpp` | done, tested (`RoleMaskTest`) |
| `Runtime::HasAuthority/SetNetMode/SetNetDriver/NetDriver`, secondary-world ctor sharing the `ComponentRegistry` | `ArcaneCore/src/Arcane/Base/Runtime.hpp` | done |
| `INetDriver { IsActive }` | `ArcaneCore/src/Arcane/Sim/NetDriver.hpp` | one method; widened by §5 |
| `PluginHost::RefuseReloadForActiveNetDriver` | `ArcaneCore/src/Arcane/Plugin/PluginHost.cpp` | done; dormant until a driver exists |
| `RunLoop` (fixed step, `SetFixedHz`, `Rebind`) | `ArcaneCore/src/Arcane/Sim/RunLoop.hpp` | gains `SimTick` |
| `ServerApp::Run` (one Runtime, headless tick, `--report`) | `ArcaneServer/src/ServerApp.cpp` | gains listen/endpoint |
| `PlayTopology { Standalone, ListenServer, EmbeddedServer, ClientOnly }` | `ArcaneEditor/src/App/PlayMode.hpp` | mapped in §6.3 |
| `EngineContext` (ABI 31) | `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp` | gains `localPeer` |
| `TypeID<T>::Hash()` (XXHash64), `GetComponentIDFromHash`, `VerifySharedTypeContext` | Astra `TypeID.hpp`, `ComponentRegistry.hpp`; `ProjectHost.hpp` | the handshake's model |
| `IFieldVisitor`, `ReflectionJson.hpp`, `AddComponentByTypeName` | Astra `Reflection/`; `ArcaneCore/src/Arcane/Serialization/` | the visitor backend's model; the remote-instantiate primitive |
| `AstraChangeTracked` (Transform), `Changed<T>`/`Added<T>` views | `Components.hpp`; Astra `View.hpp` | the snapshot walk's skip |
| `Arcane::Crypto` (HMAC-SHA256, constant-time compare) | `ArcaneCore/src/Arcane/Crypto/Crypto.hpp` | tickets |
| `TcpSocket.hpp` helpers | `ArcaneCore/src/Arcane/Net/` | the `Tcp` driver's sockets (framing is new) |
| `SessionCache`, `ServiceClient`, `MessageDispatcher` | Aphelyon `Source/Services/Common` | Combat's broker (plan 3); untouched by Core |

Absent today and built by plan 1: any UDP path, any bit-level writer/reader,
varint, quantization, byte-order conversion, a per-entity network id, a
monotonic sim tick, interest sets, connection↔player bindings, a spawn message.

## Appendix B — genre research sources (2026-09-17)

Public server reimplementations and protocol dumps; used only to establish
each game's client/server authority split, never for code.

- Honkai: Star Rail — LunarCore `Battle.java` (`SceneBattleInfo`: stage, waves,
  avatars, buffs, rounds limit, `logic_random_seed`), `HandlerPVEBattleResultCsReq.java`
  (client reports end status + statistics), `BattleService.finishBattle`
  (accepts reported HP), `HandlerSceneEntityMoveCsReq.java` (client position
  recorded as-is): https://github.com/Melledy/LunarCore
- Genshin Impact — Grasscutter `HandlerCombatInvocationsNotify.java` (client
  hit/move/animator events applied and forwarded; fall damage server-computed),
  `HandlerEntityAiSyncNotify.java` (client-reported alerts broadcast),
  `World.java` (host-owned worlds): https://github.com/Grasscutters/Grasscutter;
  protos `EntityAuthorityChangeNotify.proto`, `AuthorityChange.proto`
  (`entity_id`, `authority_peer_id`, `EntityAuthorityInfo`),
  `EntityAuthorityInfo.proto`: https://github.com/Admirepowered/genshin
- Wuthering Waves — wicked-waifus-rs `logic/handler/combat.rs`
  (`CombatSendPackRequest` → `DamageExecuteRequest`; damage computed
  server-side from attributes; `AttributeChangedNotify` pushed),
  `logic/handler/entity.rs` (`MovePackagePush` queued, unvalidated),
  `logic/handler/coop.rs` (lobby list only): https://github.com/FosterG4/wicked-waifus-rs;
  co-op = host-based, up to 3, no PvP: https://game8.co/games/Wuthering-Waves/archives/453695
- Shape references already on file: `docs/research/2026-09-16-multiplayer-shape-and-project-layout.md`
  (Source 2 / Destiny 2 / EVE / Star Citizen / SpatialOS), the Core-DLL engine
  surveys A and B (per-entity roles across nine engines).

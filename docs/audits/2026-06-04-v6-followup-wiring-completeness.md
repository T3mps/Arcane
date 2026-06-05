# v6 followup — Wiring + Completeness

**Date:** 2026-06-04
**Dimension:** wiring vs implementation, completeness against spec
**Working directory:** `D:\dev\starworks\Gacha`
**Scope:** `Server/Account`, `Server/Auth`, `Server/Combat`, `Server/Common`, cross-referenced against `docs/superpowers/specs/2026-06-01-account-db-migration-design.md` and `docs/superpowers/plans/2026-06-01-account-db-migration.md`.

The v5 pattern keeps surfacing: spec authors class → implementation lands → integration tests pass against direct invocation → class is never instantiated or invoked from a production code path. v6 finds **one new Critical** (the entire reducer pipeline is dead production code), confirms v5's deferrals are still open with explicit "DEFER" comments, and surfaces two new Highs in the same family.

---

## Status of v5 items in this dimension

- **C-V5-1 SnapshotWriter never instantiated:** STILL OPEN — explicitly deferred in `Server/Account/src/AccountServer.hpp:356-366` and `Server/Account/src/Db/SnapshotWriter.hpp:33-90` with a 4-piece scaffolding gap and triggers for revisit.
- **H-V5-2 OutboxRelay::Register never called:** STILL OPEN, but worse than v5 understood — see C-V6-1 below. v5 said "zero impact because no handler calls EmitToOutbox." That's wrong: `EffectDispatcher` calls `txn.EmitToOutbox(...)` for three destinations. v5 missed it because `EffectDispatcher` itself is not instantiated by any production code path either. The DEFER comment at `Server/Account/src/Db/OutboxRelay.hpp:57-86` is grounded on a false premise.
- **H-V5-1 AuthServer envelope-error classify:** CLOSED — `AuthServer.hpp:295-300` (HandleRegister) and `:359` (HandleLogin) both check `rpc.value.contains("error")` and log + bail before the result-fallback.
- **H-V5-3 cleanup-thread spawn-after-Stop SIGABRT:** CLOSED — `TcpServerBase.hpp:109` spawns the thread BEFORE `OnStarted()`, with a follow-on `m_running` check at `:119-123` so Start returns false when OnStarted requested early shutdown.
- **H-V5-6 Stop() LOCK2 deadlock surface:** CLOSED — `TcpServerBase.hpp:260-274` two-phase collect-then-join.
- **M-V5-4 ProtocolLoader::Id kInvalidMsgId silent fail:** PARTIAL — `IdOrThrow` exists at `Protocol.hpp:245-251` and is used by 40+ cached-ID sites in AuthServer, AccountHandlers, QuestHandlers, GachaHandlers, ProgressionHandlers, CombatServer. But `TcpServerBase::CreateErrorResponse` (`:348`) and `CreateSessionExpiredResponse` (`:354`) still use the non-throwing `.Id(...)` on the hot path. See L-V6-1.

---

## NEW findings

### [Critical-V6-1] Entire reducer pipeline (PullsReducer / QuestClaimsReducer / WalletReducer / EffectDispatcher) is dead production code
**Files:**
- `Server/Account/src/Reducers/PullsReducer.hpp` (class)
- `Server/Account/src/Reducers/QuestClaimsReducer.hpp` (class)
- `Server/Account/src/Reducers/WalletReducer.hpp` (class)
- `Server/Account/src/Reducers/ProgressionReducer.hpp` (class)
- `Server/Account/src/Effects/EffectDispatcher.hpp` (class)
- `Server/Account/src/Handlers/GachaHandlers.hpp:153-282` (HandlePull — hand-rolls events, bypasses reducer)
- `Server/Account/src/Handlers/QuestHandlers.hpp:542-706` (HandleClaimQuestReward — hand-rolls via BuildClaimEvents / ApplyClaimRewards, bypasses reducer + EffectDispatcher)
- `docs/superpowers/plans/2026-06-01-account-db-migration.md:3325-3343, 3365` (plan mandates the reducer pipeline)

**Status:** NEW

**Finding:** The migration spec and plan explicitly mandate the reducer-pipeline shape: handlers build the event payload, call `reducer.Apply(state, event)`, get back `{ next_state, effects }`, then `std::visit(EffectDispatcher{account, txn}, effect)` for each effect. The plan's Task 28 (`docs/superpowers/plans/2026-06-01-account-db-migration.md:3325-3330`) shows this verbatim for `HandlePull`; Task 29 step 2 (`:3365`) mandates the same for `HandleClaimQuestReward`.

Reality: handlers hand-roll events directly. `HandlePull` constructs `events::pulls::PullPerformed` payload inline, builds the `events::Event` envelope inline, appends via `txn.AppendEvent(...)`, and mutates `Account.GetCollection().Dispatch(...)` for grants. The four state reducers and the `EffectDispatcher` have **zero production call sites** — `grep` finds them only in `tests/ReducerTests/`, `tests/PropertyTests/`, and `tests/GoldenFile/SchemaMigrationTest.cpp`.

Greps confirming:
- `grep -rn "PullsReducer\|QuestClaimsReducer\|WalletReducer\|ProgressionReducer\b" Server/Account/src/` → only the class definitions themselves and one comment in RelationalFlush.
- `grep -rn "EffectDispatcher" Server/Account/src/` → only the class definition and one comment in RelationalFlush. No constructor calls anywhere.

**Impact:** This is the C-V4-1 / C-V5-1 family but bigger.

1. **Snapshot/replay design is unreachable.** The spec's whole premise — reducer-as-pure-function-of-(state, event) so cold-start replay reconstructs state from the event log — is unreachable today. Even if `SnapshotWriter` wires up tomorrow, the snapshots store a `ReducerState` struct that is not the projection format Account holds; and the replay code that would fold events through the reducer doesn't exist (no handler chain ever constructs one). The Critical here is bigger than C-V5-1: SnapshotWriter's deferral is grounded on "no traffic, replay is unreachable" — but the deeper reason replay is unreachable is that **the reducer pipeline itself is unwired**, not just the snapshot table.

2. **`EffectDispatcher::EmitToOutbox` is dead code** — and H-V5-2's "zero impact" claim is wrong only in concept (not in code paths today). `EffectDispatcher::operator()(ToastEffect)` emits to `notifications.toast`; `operator()(TelemetryEffect)` to `telemetry.event`; `operator()(GrantMaterialEffect)` to `inventory.grant_material`. **None of these destinations are reachable** because no production handler constructs an `EffectDispatcher`. The `Server/Account/src/Db/OutboxRelay.hpp:62-69` DEFER comment ("no production handler invokes `txn.EmitToOutbox(...)`") is **technically true but misleading**: the call sites exist; the calling-class is just unreachable from production.

3. **Two state-projection paths to keep in lockstep.** The reducer files (`PullsReducer.hpp:Apply`, etc.) encode "this is how an event mutates state on replay." The hand-rolled handler code (`GachaHandlers.hpp:178-194`, `QuestHandlers.hpp:618`) encodes "this is how an event mutates state on the live path." They are two implementations of the same business rule that **must not drift**, and there's no test asserting they agree. Today they're close enough that golden-file tests pass when the reducer is fed handler-shaped events — but any future business-rule change has to land in both places or replay silently disagrees with live.

4. **Property tests give false confidence.** `Server/Account/tests/PropertyTests/PullsPropertyTest.cpp` exercises `PullsReducer.Apply` over random event logs and asserts triple-replay equality + invariants. Green. But the reducer it's testing **is not in the live path** — a production-state corruption from a buggy hand-rolled handler would not be caught by reducer property tests. Same for `WalletPropertyTest.cpp`.

5. **Spec drift across docs.** The spec at `docs/superpowers/specs/2026-06-01-account-db-migration-design.md:677-694` ("Determinism enforcement") layers four mechanisms on top of pure reducers (signature, clang-tidy ban, property tests, golden-file replay). Three of the four (signature, ban, property tests) target a code path that doesn't run in production. The clang-tidy `aphelyon-reducer-purity` (`Tools/clang-tidy/README.md`) is source-only / not built; the regex stand-in `Tools/scripts/Check-ReducerPurity.ps1` scans files no live handler links against.

**Fix sketch:** Two paths, ADR-worthy:

(a) **Land the reducer pipeline in the four handlers** (the spec's stated direction). Migrate `HandlePull` to: build the event payload, call `m_pullsReducer.Apply(account.GetPullsState(), payload)`, set `account.SetPullsState(result.state)`, dispatch effects through `EffectDispatcher`. Same for `HandleClaimQuestReward`, the wallet path in `HandleAddCurrency`, and the progression path in `ClaimQuestReward`. Estimated ~200 LOC of mechanical edits per handler, modulo the spec's open question on how the reducer-state shape projects into Account's representation. Closes C-V6-1 and unblocks C-V5-1 (snapshots now serialize the same shape the reducer holds).

(b) **Delete the reducer pipeline as a documented YAGNI.** Per the spec's selective-ES tier, the four aggregates were chosen *because* their events are the durable record; the reducer-as-fold is what replay needs, but pre-launch with no players there's no replay use case. Remove the four reducer files + `EffectDispatcher` + the property/golden tests; demote the "fold-from-events" capability to a documented future workstream that lands alongside SnapshotWriter wiring. Closes the spec-drift surface explicitly; aligns docs with code.

Either path is fine. The current shape — code present, comprehensively tested, and unreachable from production — is the worst of both because future maintainers reading the property tests trust they protect live state.

**Verify with:** any production handler entry point should construct an `EffectDispatcher` and call `std::visit` on at least one reducer effect; today none do. After fix (a): `grep -rn "EffectDispatcher{" Server/Account/src/Handlers/` returns 4+ sites. After fix (b): the five class files + their tests are removed from the build.

**Cross-reference:** plan tasks 16-19, 28-29; spec §"Event Sourcing Patterns" subsections "RNG capture", "Determinism enforcement", "Side-effect channels".

---

### [High-V6-1] OutboxRelay::Register call sites enumerated by EffectDispatcher, but never reachable from production
**Files:**
- `Server/Account/src/Effects/EffectDispatcher.hpp:41` (emits to `"notifications.toast"`)
- `Server/Account/src/Effects/EffectDispatcher.hpp:49` (emits to `"telemetry.event"`)
- `Server/Account/src/Effects/EffectDispatcher.hpp:83` (emits to `"inventory.grant_material"`)
- `Server/Account/src/AccountServer.hpp:125` (RegisterHandlers — no `m_outboxRelay.Register(...)` for any of the three)
- `Server/Account/src/Db/OutboxRelay.hpp:57-86` (DEFER comment claims "zero callers of Register today" and "no handler emits to outbox")

**Status:** NEW — extends v5 H-V5-2 once C-V6-1 closes via path (a).

**Finding:** `EffectDispatcher` enumerates exactly three outbox destinations: `notifications.toast`, `telemetry.event`, `inventory.grant_material`. These are the production destinations that need handler registrations. The OutboxRelay.hpp DEFER comment says "Revisit when ANY handler emits its first outbox row" — but the *enumeration* is done already; the gap is purely (i) the EffectDispatcher being unreachable (C-V6-1) and (ii) no `m_outboxRelay.Register("notifications.toast", ...)` etc. in `AccountServer::RegisterHandlers`.

If C-V6-1 is fixed via path (a) (land the reducer pipeline), the *first commit* that wires `EffectDispatcher` into `HandlePull` writes rows to `outbox` with destinations `notifications.toast` etc. The relay's `PumpOnce` then silently skips them every 500ms forever — exactly the failure mode v5 H-V5-2 documented.

**Impact:** Bounded today (no production rows). Becomes Critical the day path (a) lands.

**Fix sketch:** In `AccountServer::RegisterHandlers` (or a sibling `RegisterOutboxDestinations` method), call `m_outboxRelay.Register("notifications.toast", [](const Json&){ /* stub: log and return true so the row drops */ return true; })` for each of the three. Stubs are fine pre-launch — the design intent is that consumers (notification service, telemetry service, inventory projection) live in separate processes; the relay's job is to dispatch via outbound RPC when they exist. A stub that logs + returns true unblocks the dispatch loop and surfaces the destination volume in logs so an operator sees them exercising. Or, more strictly, add a startup self-check that asserts `m_outboxRelay.HandlerCount() >= ExpectedDestinations(EffectDispatcher).size()` so a future EmitToOutbox addition without a matching Register fails fast (the forward-guard the OutboxRelay.hpp comment proposes "out of scope pre-launch").

---

### [High-V6-2] CombatServer is a stub with no message handlers — entire `Combat/src/` namespace is wiring-vs-implementation gap
**Files:**
- `Server/Combat/src/CombatServer.hpp` (entire file)
- `docs/superpowers/specs/2026-06-01-account-db-migration-design.md:32` ("Combat persistence (still a stub; spec it when the service grows beyond stub)")

**Status:** NEW (carry-forward from v3-v5 L-V4-7, promoted)

**Finding:** Combat's `OnProcessMessage` returns "not yet implemented." The service binds 7772/TCP + 7778/UDP and runs `ProbeStartupPeers` against Auth (the probe wire is real — Combat probes Auth at startup). But every cross-cutting invariant the audit cadence has codified for Account (idempotency keys, stripe locks, advisory locks, OutboxRelay registration, EventStore patterns) has zero coverage on the Combat side because there's no handler to apply them to. The moment Combat handlers materialize, the v1-v5 finding catalog has to be re-audited against them.

**Impact:** Not a today-bug. But the `client_handlers_total` metric (combat-stub line) ships in production as "service is up, replies to ping, says no" — which an ops dashboard may class as healthy without realizing zero business logic runs. The spec calls Combat persistence a separate future spec.

**Fix sketch:** Either (a) Document Combat as a deliberate placeholder in `CombatServer.hpp:OnProcessMessage` with a TODO referencing the future spec, OR (b) refuse to start Combat unless a `--enable-stub-combat` flag is passed so the build doesn't silently ship a no-op service. Pure governance choice.

---

### [Medium-V6-1] `Account.hpp` snapshot X-macro silently misses fields added without ceremony
**File:** `Server/Account/src/State/Account.hpp:117-172` (X-macro Snapshot)
**Status:** CARRY-FORWARD (v3-M-V3-2, v4-M-V4-1, v5-M-V5-1 cross-cutting; promoted here because it's a wiring gap rather than a code-quality gap)

**Finding:** `Account::CaptureSnapshot` and `Account::RestoreFrom` are driven by an X-macro that enumerates ~20 direct fields plus 2 indirect (rngState, collectionState). The convention "every mutable Account field is in the X-macro or in the 2-entry indirect block" is enforced by reviewer attention only. Adding a new mutable field without an X-macro entry silently disables Memento-style rollback for that field on `AccountTransaction::Rollback` — speculative mutations leak into the cached Account after a failed commit. `MarkStaleForReload` rescues across the cache boundary (the next `GetLockedAccount` rehydrates from DB), but mid-handler reads of post-Rollback Account would observe the leaked value.

**Impact:** Open trap for the next engineer who adds e.g. `std::vector<UnlockedTitle> m_titles` + a `SetTitle()` setter. Today's mutable field set is stable and audited; the failure mode is purely defensive.

**Fix sketch:** v3 / v4 / v5 all proposed a `[c7-fields]` Catch2 test (~80 LOC) that walks the X-macro and asserts round-trip equality for every direct field. Cheaper alternative: `static_assert(sizeof(Snapshot) == kExpectedSnapshotSize)` tripwire — a value drift fires at compile time. Either ends the carry-forward.

---

### [Medium-V6-2] No production wire for `isReplay` parameter the spec mandates
**Files:**
- `docs/superpowers/specs/2026-06-01-account-db-migration-design.md:699` ("A `bool isReplay` parameter is threaded through the dispatch")
- All reducers + `EffectDispatcher` (no `isReplay` parameter anywhere)

**Status:** NEW (spec compliance gap)

**Finding:** The spec mandates a `bool isReplay` parameter threaded through effect dispatch so live-mode dispatches side effects (toast/telemetry/outbox) and replay-mode skips them. No code carries the parameter. EffectDispatcher always dispatches; if replay ever becomes reachable (per C-V6-1's reducer-pipeline path), every replayed pull re-emits a toast and re-writes to outbox — duplicate notifications + duplicate analytics.

**Impact:** Blocks replay use cases. Today inert because replay is unreachable (C-V6-1 + C-V5-1).

**Fix sketch:** Bundled with C-V6-1 path (a). When wiring `EffectDispatcher` into handlers, the constructor takes the `bool isReplay` flag (default false). On the live path the handler constructs `EffectDispatcher{account, txn, /*isReplay=*/false}`; on a future replay code path `/*isReplay=*/true` short-circuits the three `txn_.EmitToOutbox(...)` calls. ~10 LOC. No-op until replay lands, but pins the contract.

---

### [Medium-V6-3] `events.schema_version` is dead-on-write and dead-on-read (M-V5-2 cross-cutting carry-forward)
**Files:**
- `Server/Account/schema.sql:300` (`schema_version INT NOT NULL DEFAULT 1`)
- `Server/Account/src/Events/Event.hpp:32` (`int schema_version = 1` — never overridden by any emit site)
- `Server/Account/src/Db/EventStore.hpp:170-189` (load reads the column)
- All four reducer files (none dispatch on schema_version)

**Status:** CARRY-FORWARD from v5 M-V5-2 cross-cutting

**Finding:** The column is written with the struct default, read into the struct, never consulted. The infrastructure to use it (upcaster registry, per-event-type version map) doesn't exist. First migration to v2 has nothing to extend.

**Impact:** Load-bearing-with-zero-scaffold the moment any event payload shape changes. Pre-launch flexibility means this is cheap to address now and expensive once a fixture set assumes v1 forever.

**Fix sketch:** Either (a) drop the column from schema + struct (it's actively misleading), or (b) add a no-op `ReducerCommon::ApplyMigrations(event)` shim called before every reducer dispatch. Bundle with C-V6-1 path (a) cleanup or path (b) cleanup. ADR-worthy.

---

### [Low-V6-1] `TcpServerBase::CreateErrorResponse` / `CreateSessionExpiredResponse` still use non-throwing `Id()` (M-V5-4 cross-cutting carry-forward)
**Files:**
- `Server/Common/src/Net/TcpServerBase.hpp:348` (`ProtocolLoader::Instance().Id("ErrorResponse")`)
- `Server/Common/src/Net/TcpServerBase.hpp:354` (`ProtocolLoader::Instance().Id("SessionExpired")`)

**Status:** PARTIAL — the IdOrThrow rollout covered 40+ handler-side cached IDs but missed two hot-path call sites in the shared base.

**Finding:** If `"ErrorResponse"` or `"SessionExpired"` is ever renamed in `protocol.json`, these silently send id 0. Every error reply from every service silently breaks. Both are also called per-message rather than cached.

**Fix sketch:** Cache the two IDs as static members in `TcpServerBase` (lazy init in first-call helpers) and resolve via `IdOrThrow` at first use. Or static-cache in a one-shot `InitializeProtocolIds()` called from `Start()`.

---

## Observations / Lows

- **C-V5-1 SnapshotWriter and H-V5-2 OutboxRelay::Register deferrals are both documented in-tree** with detailed DEFER comment blocks listing the scaffolding needed and the revisit triggers. This is good engineering hygiene — the audit's job is to verify the DEFER reasoning still holds, not re-flag every documented deferral. v6 only escalates these where new context (e.g., the unreachable EffectDispatcher producing imaginary outbox rows) invalidates the deferral's "current impact: zero" premise. Today both deferrals are still defensible AS WRITTEN; what changes is that C-V6-1 reveals a third deferral hiding in the same family (the reducer pipeline itself).
- **Clang-tidy `aphelyon-reducer-purity` check is "source-only, not currently built into a plugin"** (`Tools/clang-tidy/README.md:12`). The regex stand-in at `Tools/scripts/Check-ReducerPurity.ps1` is documented as the active CI check, scanning files under `Server/Account/src/reducers/`. Per C-V6-1, the files being scanned aren't live production code today, so the check is purity-protecting unreachable code. Not a finding on its own — the check is cheap and protects forward-portable code — but worth noting as part of the spec-drift catalog.
- **xoshiro256++ wiring is correct.** `GachaRNG.hpp:12` documents the choice; `Server/Account/src/State/GachaRNG.hpp` uses it (verified via grep). No `mt19937` in `Server/Account/src/`. The PRNG decision from the spec is implemented.
- **AccountServer member-init order is safe.** Reverse-destruction sequence: handlers → dispatcher → ctx → sessionCache/authClient → internalRpcHandlers/cache → questLoader/banners → repository → outboxRelay → pool. OutboxRelay's worker is joined (via ~OutboxRelay) before m_pool tears down. No UAF surface on shutdown.
- **OutboxRelay starts its worker thread in the member-init list** (`m_outboxRelay(m_pool)`), so the worker is running before `AccountServer::AccountServer` body executes. V5 M-V5-1 concurrency flagged this as a race surface for future `Register` calls in the ctor body. Today zero callers register, so the race window is theoretical; the audit comment at `AccountServer.hpp:76-80` documents the intent. Not promoted.
- **EventStore::Append (standalone) vs AppendInTx production use.** Audited per L-V3-5 (`EventStore.hpp:107-114`). `AppendInTx` is the live path via `AccountTransaction::Commit`. The standalone `Append` is alive for tests + the in-class `AppendIdempotent`. No new finding.
- **RelationalFlush::Flush** is wired through `AccountTransaction::Commit` (line 241). Not dead code.
- **Combat handler stub** intentionally returns "not yet implemented" — explicit non-finding pending its own spec.

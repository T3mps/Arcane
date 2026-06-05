# Aphelyon Server + Persistence Audit — v6 (Follow-up to v5)
**Date:** 2026-06-04 (successor to v5; after the v5 medium-tail closeout and Scope 4 per-account locks refactor)
**Scope:** Same as v5 — Server/Account, Server/Auth, Server/Combat, Server/Common, schema.sql, Client/src/network_tcp.lua + Client/src/services, Server/Account/tests.
**Method:** 9 parallel general-purpose audit agents (per-dimension), each given full v5 synthesis + v5 per-dimension followup + brief on Scope 1-3-4 closures + verbatim anti-nitpick directive from user. Each agent verified v5 closures against current code state and surfaced new findings. Convergent flagging used as confidence signal.

**Per-dimension findings:**
1. Wiring + completeness → `2026-06-04-v6-followup-wiring-completeness.md` (NEW dimension this sweep)
2. Persistence + schema + DB state → `2026-06-04-v6-followup-persistence.md`
3. Concurrency + lifecycle → `2026-06-04-v6-followup-concurrency.md`
4. Event sourcing + replay → `2026-06-04-v6-followup-event-sourcing.md`
5. Idempotency + outbox → `2026-06-04-v6-followup-idempotency.md`
6. Error handling + propagation → `2026-06-04-v6-followup-error-handling.md`
7. Security + auth + secrets → `2026-06-04-v6-followup-security.md`
8. Network + RPC + envelope → `2026-06-04-v6-followup-networking.md`
9. Cross-cutting + test coverage → `2026-06-04-v6-followup-cross-cutting-tests.md`

---

## Executive Summary

**v5 closures landed at unprecedented rate.** Of the v5 Critical + 8 High items I had listed as open at sweep start, only the two structural ones (C-V5-1, H-V5-2) remain. The other six Highs (H-V5-1, H-V5-3, H-V5-4, H-V5-5, H-V5-6, plus 4-of-5 H-V5-8 sub-items) all landed in code between the v5 audit (`2026-06-03`) and this sweep, via commits the orchestrator's initial brief missed: `88a9346` (Auth envelope-error), `058a1d1` (cleanup-thread spawn order), `90f2a88` (Stop two-phase), and the partman premake re-bootstrap. Plus the v5 medium-tail closeout (Scopes 1-3-4, commits `c36947e..47d16f0`) closed every M-V5-* item this sweep tested for. **Independent triple-confirmation by networking + security + error-handling agents pinned H-V5-1 closed in `88a9346`** — strong consensus on the state.

**Net result: 8 of 9 dimensions verdicted clean or A−**. The single concerning dimension is **wiring + completeness**, which surfaces the v6 headline:

**C-V6-1 — The entire reducer pipeline is dead production code.** `PullsReducer`, `QuestClaimsReducer`, `WalletReducer`, `ProgressionReducer`, and `EffectDispatcher` are all fully implemented with property tests, but **no handler invokes them**. Handlers hand-roll events directly through `AccountTransaction::AppendEvent` and apply state mutations imperatively. The spec's mandated `reducer.Apply(account, event) → std::visit(EffectDispatcher)` shape exists only in tests. Operational consequence: **live state and replay state are two independent implementations of the same business rules with no agreement test**. Reducer property tests give false confidence — they exercise classes that never run in production. This is C-V5-1 (SnapshotWriter unwired) one level deeper: the snapshot/replay machinery would be hollow even if SnapshotWriter were wired, because the reducer it would replay through is itself unwired. Same defect family as C-V4-1 (OutboxRelay never instantiated), now at the projection layer.

**H-V6-1 cluster — EffectDispatcher's three outbox destinations have no Register handlers.** When the reducer pipeline is eventually wired, `notifications.toast`, `telemetry.event`, and `inventory.grant_material` emissions land in the outbox with no dispatcher to drain them. The v5 H-V5-2 ("OutboxRelay::Register never called") is now refined: the comment in `OutboxRelay.hpp` claiming "no handler emits to outbox" is technically false today (EffectDispatcher emits to three destinations) and operationally false the moment the C-V6-1 cluster lands. Two agents (wiring + cross-cutting) independently caught this.

**Three other substantive new findings** beyond the C/H cluster:
- **H-V6-2** — `CombatServer::OnProcessMessage` returns "not yet implemented" for every message. Combat is a documented stub. Cross-service contracts (Auth → Combat session forwarding, Account → Combat match-result recording) have zero combat-side coverage. Either a stub-mode runtime gate (`--enable-stub-combat`) or a deferred Combat spec is needed before launch planning.
- **H-V6-3** — `Server/data/protocol.json` and `Client/data/protocol.json` have drifted: `CompleteQuest` / `CompleteQuestResponse` exist server-side only; `Client/src/network_tcp.lua:1850` calls `conn:send(MessageTypes.CompleteQuest, ...)` with a nil id. The 42 shared messages match correctly — this is missing-message drift, not collision drift. The Editor (`DataStore.hpp`) writes only the server file.
- **Bundle finding** — across event-sourcing, idempotency, persistence, and wiring agents, **5 separate Mediums** all silently activate the moment C-V6-1 is closed: `*FromStr` silent vocabulary coercion to Credits/Credits; `*FromJson` zero-fill for missing required fields; `isReplay` parameter unwired; `WorldFlagStore` mutations never set dirty bits; response-payload cache write-order convention. Independent agents recommend bundling all five with the C-V6-1 wire-up as one "make replay correct end-to-end" landing.

**Tally (raw, before de-dup):** 1 Critical, 4 High, ~12 Medium, ~22 Low.

**De-duplicated unique findings:** 1 Critical, 3 High, ~10 Medium, ~18 Low. The convergent flagging on the EffectDispatcher / OutboxRelay::Register cluster collapses to a single arc.

**Verdicts by dimension:**

| Dimension | Verdict | C/H/M/L | Notes |
|---|---|---|---|
| Wiring + completeness | concerning | 1/2/3/1 | C-V6-1 the headline; H-V6-2 Combat stub |
| Persistence + schema | clean | 0/0/2/3 | WorldFlagStore wiring gap; dead lastLogin field |
| Concurrency + lifecycle | clean | 0/0/1/4 | Scope 4 verified internally consistent |
| Event sourcing + replay | clean live; concerning on replay | 0/0/2/3 | All defects bite only when replay reachable |
| Idempotency + outbox | clean | 0/1/1/1 | H-V5-2 refined as H6-1 (startup self-check) |
| Error handling | clean | 0/0/0/1 | One BannerConfig silent-swallow; 4 observations |
| Security + auth | clean | 0/0/1/7 | RequiresAuthentication asymmetric polarity |
| Network + RPC | A− clean | 0/0/0/4 | Triple-verified v5 H closures |
| Cross-cutting + tests | clean | 0/1/1/1 | Protocol.json drift; raw Id() hot-path remnants |

**Test count growth:** v5 baseline 117 cases / 779 assertions → v6 HEAD **159 cases / 1131 assertions** (+42 / +352). Fix-vs-test ratio post-v5 includes deliberate test backfill for H-V5-8 sub-items (OutboxRelaySweepTest, ProbeClassificationTest, OutboxAccountIdTest) and Scope 4's 4-case concurrency suite.

**Systemic risk surfacing this pass:**

- **Wiring-vs-implementation is now a recurring class.** v4 caught C-V4-1 (OutboxRelay), v5 caught C-V5-1 (SnapshotWriter), v6 catches C-V6-1 (Reducer pipeline). The pattern: spec authors a component, integration tests cover it, but production startup never instantiates it. Each layer of the migration design (event log → snapshots → reducers → effect dispatch) has now been independently flagged as unwired. **Recommendation:** before v7, run a one-shot wiring audit script that diffs the spec's component inventory against `AccountServer.hpp`'s member-init list and `grep production-callsites`. The class is now characterized enough to be detected mechanically.

- **Replay-path defect cluster is invisible until it's load-bearing.** Reducer FromStr/FromJson coercion, missing isReplay threading, response-payload write-order, world_flags dirty-bit wiring — none bite today (live path bypasses them all). All five activate simultaneously when C-V6-1 closes. This creates a risk that the C-V6-1 wire-up commit looks small (a few constructor lines) but unleashes 5 silent corruption surfaces. Mitigation: bundle the wire-up with the 5 sub-fixes; spec the bundle as one atomic deliverable.

- **Carry-forward dominance is finally dropping.** v6 surfaces ~18 unique Lows vs v5's ~70 / v4's ~70 / v3's ~30. Two structural reasons: explicit DEFER blocks landed inline in source (v5 era) so they no longer re-surface in audit cycles; and the Scopes 1-3-4 work mechanically eliminated 6+ M-V5 carry-forwards. The audit noise floor is structurally falling for the first time in the v2→v6 arc.

- **AccountServer.hpp audit-tag density dropped 7.0% → 5.5%** (batch-k canonicalization collapsed duplicates). The v5 observation about extracting `AccountServer_DESIGN.md` is no longer triggering.

---

## CRITICAL (fix before next major feature)

### C-V6-1. The entire reducer pipeline is dead production code

**Files:** `Server/Account/src/Reducers/PullsReducer.hpp`, `QuestClaimsReducer.hpp`, `WalletReducer.hpp`, `ProgressionReducer.hpp`, `Server/Account/src/Effects/EffectDispatcher.hpp` (all fully implemented), `Server/Account/src/Handlers/` (none invoke the reducers)
**Source:** wiring C-V6-1
**Status:** NEW

The reducers exist. The `EffectDispatcher` exists. Property tests cover the four reducers with rigor (`PullsReducerTest`, `QuestClaimsReducerTest`, `WalletReducerTest`, `ProgressionReducerTest`). The spec at `docs/superpowers/specs/2026-06-01-account-db-migration-design.md` mandates the canonical shape:

```cpp
// SPEC-CANONICAL:
auto event = HandlerEmitEvent(...);
account.GetWallet().SetState(WalletReducer::Apply(account.GetWallet().GetState(), event));  // projection
std::visit(EffectDispatcher{txn, account}, event);                                          // side-effects
txn.AppendEvent(event);                                                                     // event log
```

What production handlers actually do:

```cpp
// HANDLER-ACTUAL (e.g., GachaHandlers::HandlePull):
account.GetWallet().Debit(cost);              // direct in-place mutation
account.GetPity(slot).Reset();                // direct in-place mutation
events::Event ev{...};                        // hand-rolled event
txn.AppendEvent(std::move(ev));               // event log
```

**Two independent implementations of every business rule.** The reducer-driven path applies `WalletDebitedEvent` and projects a new state. The handler-driven path mutates the live `Wallet` in place. They are not tested for agreement. A reducer fix would not propagate to live behavior; a handler fix would not propagate to replay behavior. Property-test confidence on the reducers is misleading — those classes never execute in any production path.

**Operational consequences:**
- Snapshot/replay cannot reproduce the live state on cold-start replay, because the live state is computed by a different code path. C-V5-1's SnapshotWriter wire-up would not actually unlock the replay value the migration spec promised — the replay would diverge.
- Any future "rebuild player state from event log" tool (debug, GDPR export, replay-for-test) gets a different answer than the live process.
- 5 separate Mediums in this audit (M-V6-event-sourcing-1, M-V6-event-sourcing-2, M-V6-wiring-3, M-V6-persistence-1, M-V6-idempotency-1) silently activate the moment C-V6-1 closes.

**Fix sketch:** This is structurally invasive. The migration spec authored the canonical shape; handlers regressed. Restoring it requires either:
- (a) Refactor each handler to `Reducer::Apply` + `std::visit(EffectDispatcher)` + `AppendEvent`. 4 aggregates × ~6 handlers = ~24 handler sites. Per-handler change is mechanical but each requires careful audit of side-effects (rate limits, idempotency, response shape).
- (b) Accept the imperative path as canon, delete the reducer classes + EffectDispatcher, update the spec, and document that "replay" rebuilds via re-running handler logic against the event log — which requires handlers to be re-entrant against event streams (they're not today).

Option (a) is the spec-aligned path; option (b) is the "make the code reflect reality" path. The choice gates every downstream replay/snapshot/debug-tool plan.

**Cross-reference:** Same defect family as C-V4-1 (OutboxRelay never instantiated) and C-V5-1 (SnapshotWriter never instantiated). Each layer of the migration design — event log → snapshots → reducers → effects — has now been independently flagged as unwired in some form. Before the v7 audit, run the wiring-vs-instantiation script noted in the systemic-risk section.

---

## HIGH

### H-V6-1. EffectDispatcher emits to 3 outbox destinations with no Register handlers

**Files:** `Server/Account/src/Effects/EffectDispatcher.hpp:41,49,83` (emits to `notifications.toast`, `telemetry.event`, `inventory.grant_material`), `Server/Account/src/AccountServer.hpp` (no `m_outboxRelay.Register(...)` calls)
**Source:** wiring H-V6-1, cross-cutting H-V6-1 (**two independent flags**), refinement of v5 H-V5-2
**Status:** NEW (refines v5 H-V5-2 from "comment-only" to "comment is technically false today")

The v5 H-V5-2 finding was: "OutboxRelay::Register never called; dispatch path is dead-code-by-omission." The annotation in `OutboxRelay.hpp` deferred this on the premise that "no handler emits to outbox." That premise is **false today** because `EffectDispatcher` (fully implemented, with property tests) emits to three named destinations. It is **operationally** true today only because C-V6-1 means EffectDispatcher is itself unreachable. The moment C-V6-1 closes, three outbox flows immediately start accumulating undispatched rows.

**Bundled with C-V6-1:** if option (a) of C-V6-1's fix is chosen, the three Register sites must land in the same commit. If option (b), delete the EffectDispatcher destinations and re-spec.

**Standalone fix sketch (regardless of C-V6-1 path):** add a startup self-check to `OutboxRelay::Start`:
```cpp
if (m_handlers.empty() && CountUndispatchedOutbox() > 0) {
    LOG_DB_WARN("OutboxRelay: {} undispatched rows with zero registered handlers — dispatch path is dead",
                CountUndispatchedOutbox());
}
```
~10 LOC. Closes the silent-accumulation surface even if the wire-up is deferred. Two agents (wiring + idempotency) recommend this independently.

### H-V6-2. CombatServer is a stub; cross-service contracts have zero combat-side coverage

**Files:** `Server/Combat/src/CombatServer.hpp` (`OnProcessMessage` returns "not yet implemented" for every message)
**Source:** wiring H-V6-2
**Status:** NEW

Auth → Combat session validation, Account → Combat match-result recording, the UDP gameplay channel — all wire up at the network layer but the Combat process rejects every message. This is documented in CLAUDE.md and the spec but creates real ops + integration risk:
- The Combat process binds TCP 7772 + UDP 7778 and starts cleanly, masking the stub state from health-check perspectives.
- A future change to the protocol vocabulary (any rename of a Combat message) is undetectable on the server side.
- The client-side code in `Client/src/services/combat_*.lua` issues real-shaped messages that hit a stub responder.

**Fix sketch (one of):**
- Add a runtime gate: `combat --enable-stub-mode` required to start; default refuses with `CombatServer is not implemented`.
- Author a Combat protocol/handler spec analogous to `2026-06-01-account-db-migration-design.md` so future audits have a non-NULL baseline.
- Document the stub-state explicitly in CLAUDE.md's Combat section (currently the combat memory describes only the client-side combat UI).

The CombatServer ctor-arg-order bug fixed during Scope 1 (silent `udpPort=7778` passed as `authInternalPort`) is a sign of how easy stub-side drift is to ship.

### H-V6-3. Server/Client protocol.json drift — CompleteQuest exists server-side only

**Files:** `Server/data/protocol.json` (defines `CompleteQuest` + `CompleteQuestResponse`), `Client/data/protocol.json` (missing these entries), `Client/src/network_tcp.lua:1850` (calls `conn:send(MessageTypes.CompleteQuest, ...)` with a nil id)
**Source:** cross-cutting H-V6-2
**Status:** NEW

The 42 other shared messages have matching ids between server and client — this is pure missing-message drift, not collision drift. The Editor (`Tools/Editor/src/DataStore.hpp:37`) writes only the server file; client copies are hand-maintained.

**Operational consequence:** any client call to `MessageTypes.CompleteQuest` sends id=nil → server rejects → quest-complete is broken for that quest type. Today the impact is bounded to whichever quest type the client side tries to drive via CompleteQuest (likely the F2 quest UI's complete button).

**Fix sketch:** copy the two entries from `Server/data/protocol.json` to `Client/data/protocol.json`. Longer-term: extend the Editor's `DataStore::Strings()` pattern to also synchronize client/server protocol.json on flush; or write a CI gate that diffs the two files and fails the build on drift.

---

## MEDIUM (selected; full lists in per-dimension reports)

### Wiring + completeness
- **M-V6-3.** The `isReplay` parameter mandated by the spec is not threaded through the dispatch path. If C-V6-1 closes and reducer-driven replay starts running, replay would re-fire outbox writes + toasts. Bundle with C-V6-1.

### Persistence
- **M-V6-1.** `WorldFlagStore` mutations never set dirty bits. `Set`/`Clear` mutate memory but `world_flag_adds`/`world_flag_removes` is never populated by production code. `Save`'s brute-force dirty-mark loop also omits `world_flags`. Today zero impact (no handler writes flags); first handler that does will silently lose data on commit + idle-evict + reload. Same wiring-vs-implementation defect class as C-V6-1.
- **M-V6-2.** `AccountData.lastLogin` is a dead field — loaded by Repository, dropped by Hydrator (Account has no `m_lastLogin` setter), never read. A future "last session time" feature would get a misleading value. Recommend deletion.

### Event sourcing
- **M-V6-1 ES.** `CurrencyFromStr` / `RewardKindFromStr` silently coerce unknown vocabulary to `Credits` / `Credits` (`WalletEvents.hpp:42-49`, `QuestClaimEvents.hpp:71-81`). Replay-corruption surface once C-V6-1 closes.
- **M-V6-2 ES.** `*FromJson` parsers zero-fill missing required fields via `j.value(k, 0)`. `StoryLevelAdvancedFromJson` (`ProgressionEvents.hpp:27-37`) missing `to_level` silently clobbers projection to 0; compounds with M-V5-2 (Progression baseline missing).

### Idempotency
- **M-V6-1 idem.** Handler response-payload cache write order is convention-only. A future refactor that hoists `BuildStatePayload` above the mutation silently caches T0 state for T1 events. Fix via documented STRUCT-VIOLATION example or a `StoreIdempotencyDeferred` lambda API.

### Security
- **M-V6-1 sec.** `RequiresAuthentication` defaults to FALSE on missing `requires_auth` (`Protocol.hpp:190` loader-side default), while the lookup-side fallback at `:393` defaults to TRUE. Asymmetric polarity: a future hand-edit of `protocol.json` that omits `requires_auth` silently opens an unauthenticated handler. No live exploit today (all 24 client-to-server messages set it explicitly), but it's the wrong default for a security boundary read from a hand-edited JSON file. 1-LOC fix.

### Concurrency
- **M-V6-1 conc.** `SaveAllAndClear` at `AccountCache.hpp:384` uses unbounded `lock_guard` on per-Account mutex. If a detached internal-RPC handler survives drain, `Stop()` wedges. Carry-forward interaction surface from H-V5-4 + Scope 4 q. Fix: `try_lock_for` + watchdog.

### Cross-cutting + tests
- **M-V6-1 ct.** `TcpServerBase::CreateErrorResponse` / `CreateSessionExpiredResponse` still use raw `Id()` on the hot path (M-V5-4 partial closure; `IdOrThrow` adopted at 45 sites but 2 remain).

---

## LOW / OBSERVATION (selected)

- **L-V6-1 (cross-cutting):** Total-cap (`MAX_CONNECTIONS_TOTAL`) refusal path remains untested even though per-IP cap is now covered. Add ~30 LOC to `TcpServerCapsTest`.
- **L-V6-1 (concurrency):** `m_stale` relaxed memory-ordering claim from Scope 4 batch o-followup overstates happens-before. Writer holds `m_handlerMutex`, reader holds `m_mapMutex` — no formal synchronizes-with edge through either mutex. Practical x86 runtime is correct, but the comment conflates wall-clock ordering with C++ memory model happens-before. Fix: either tighten to release/acquire OR reword the comment to acknowledge x86 reliance + self-healing via stale-flag.
- **L-V6-1 (error handling):** `BannerConfig.hpp:79` has empty `catch (...) {}` on canonical `_meta.json` parse. Worse than the L-V5-4 TemplateDatabase sibling because it's the single source-of-truth for banner slots. A typo zeros the whole gacha-slot table silently; only signal is "Loaded 0 slots" in startup log.
- **L-V6 (event sourcing):** 3 carry-forward items — central event-type registry, PullsReducer rng_state_after capture, schema_version upgrade path scaffold.
- **L-V6 (persistence):** `outbox_account_idx` now non-partial (correct per M-V5-1 closure); no outbox→accounts CASCADE path test (defer until GDPR delete spec lands); `seed.sql` comments-only (documented design).
- **L-V6 (networking):** Combat UDP port advertised but no socket exists (companion to H-V6-2); `Message::ParseBody` partial `std::stoi` (same defect class as closed M-V4-5 stoull, inner not outer); `RpcParser` 65536 maxBodySize vs MessageParser configurable cap; ServiceEndpoint Stop() overrun detaches threads.

Full enumeration in per-dimension reports.

---

## Verified Closed (from v5, by independent multi-agent consensus)

| Item | Source | Status | Confirming agents |
|---|---|---|---|
| **C-V5-1 SnapshotWriter unwired** | v5 Critical | **STILL OPEN** | wiring, event-sourcing |
| **H-V5-1 AuthServer envelope-error miss** | v5 High | **CLOSED** (`88a9346`) | networking, security, error-handling, cross-cutting (**4-way confirm**) |
| **H-V5-2 OutboxRelay::Register never called** | v5 High | **STILL OPEN; refined as H-V6-1** | wiring, idempotency, event-sourcing |
| **H-V5-3 m_cleanupThread spawn-after-Stop SIGABRT** | v5 High | **CLOSED** (`058a1d1`) | networking, concurrency |
| **H-V5-4 ServiceEndpoint 10s drain UAF window** | v5 High | **CLOSED** (Debug 30s / Release 10s) | networking, concurrency |
| **H-V5-5 partman premake schema-vs-live drift** | v5 High | **CLOSED in live DB** (premake=12, 17 partitions through 2027-06) | persistence (live psql verification) |
| **H-V5-6 TcpServerBase Stop() LOCK2 deadlock** | v5 High | **CLOSED** (`90f2a88` two-phase collect-then-join) | concurrency, networking |
| **H-V5-7 accounts.public_uid UNIQUE dead surface** | v5 High | **STILL OPEN with documented DEFER** tied to social-graph spec | persistence |
| **H-V5-8 5 v4-introduced untested code paths** | v5 High | **4 of 5 CLOSED** (OutboxRelaySweepTest, ProbeClassificationTest, InvokeForTest `APHELYON_TEST_BUILD` gate, OutboxAccountIdTest); per-IP cap covered, total cap still PARTIAL | cross-cutting |
| **M-V5-1 event-sourcing AppendIdempotent recheck lock** | v5 Medium | **CLOSED** (`a9bb510` + `1644d92`) | event-sourcing, idempotency |
| **M-V5-4 concurrency stripe false-contention** | v5 Medium | **CLOSED** (Scope 4 batches m–r) | concurrency |
| **M-V5-6 networking NAT cap configurability + log throttle** | v5 Medium | **CLOSED** (Scope 1 batch l, `27d7917`) | networking |
| **M-V5-7 networking ServiceEndpoint loopback caps** | v5 Medium | **CLOSED** (Scope 2 batch j, `c36947e`) | networking |
| **M-V5-1 persistence outbox NULL-debris + nullable** | v5 Medium | **CLOSED** (NOT NULL + live row delete, `635560f`) | persistence |
| **M-V5-3 persistence idempotency `created_at = now()` refresh** | v5 Medium | **CLOSED** | persistence |
| **M-V5-3 concurrency per-IP counter lag** | v5 Medium | **CLOSED** (`3c59b2d`) | concurrency |
| **M-V5-1 idempotency client `or uuid4()` fallback** | v5 Medium | **CLOSED** (`2d79bf5`) | idempotency |
| **M-V5-1 error-handling SessionCache 4-string deployment-skew split** | v5 Medium | **CLOSED** | error-handling, networking |
| **H-V4-5 InvokeForTest gate** | (carryforward) | **CLOSED via `APHELYON_TEST_BUILD`** | networking, security |

Items remaining open with explicit DEFER annotations: M-V5-2 persistence (epoch::bigint truncation, second-granularity OK pre-launch), M-V5-3 persistence (Save brute-force, 3-cycle carry-forward), M-V5-1 networking (server-side hardcoded 5000ms, annotated `bc05aee`), M-V5-3 networking (Probe 2-of-4 classification, ProbeClassificationTest pins current behavior), M-V5-1 idempotency (other idempotency Lows), various pre-launch Lows.

---

## Test Coverage Status

- v5 baseline: 117 cases / 779 assertions
- **v6 HEAD: 159 cases / 1131 assertions** across 4 binaries (Account 108/807, Common 38/289, Auth 12/34, Combat 1/1)
- **Net new since v5: +42 cases / +352 assertions** — deliberate H-V5-8 backfill (OutboxRelaySweepTest, ProbeClassificationTest, OutboxAccountIdTest, AccountCacheTest's 4-case Scope 4 concurrency suite) plus Scope-1/2/3 test additions (ServiceEndpointCapsTest, TcpServerCapsTest extension)

v5 prioritized top-5 status:
1. **Wire SnapshotWriter into AccountServer** — STILL OPEN; spec'd defer block lands at `AccountServer.hpp:356-366`. v6 finds the deeper C-V6-1 supersedes this.
2. **Apply C-V4-2 envelope-error classify to HandleLogin + HandleRegister** — **CLOSED** (`88a9346`).
3. **Fix m_cleanupThread spawn-after-Stop SIGABRT** — **CLOSED** (`058a1d1`).
4. **Restructure TcpServerBase::Stop() to drop m_clientsMutex before thread.join()** — **CLOSED** (`90f2a88`).
5. **Test backfill for 5 v4-introduced untested code paths** — **4 of 5 CLOSED**; total-cap still PARTIAL.

v6 recommended top-5:
1. **Resolve C-V6-1 reducer pipeline disposition.** Decide option (a) wire `reducer.Apply → EffectDispatcher` into handlers, or option (b) accept the imperative path as canon and delete the reducer classes. Either way, this is the blocking decision before any future replay/snapshot/debug-tool work.
2. **If option (a) above: bundle the C-V6-1 wire-up with the 5 silent-corruption Mediums** (event-sourcing FromStr/FromJson coercion, isReplay threading, world_flags dirty bits, response-payload write order) as one atomic "make replay correct end-to-end" landing.
3. **Add H-V6-1 startup self-check** to `OutboxRelay::Start` regardless of C-V6-1 path: WARN if registered-handler count is zero and undispatched-row count is non-zero. ~10 LOC.
4. **Fix H-V6-3 protocol.json drift.** Copy `CompleteQuest` entries to client; longer-term, add a CI gate or extend the Editor to write both files.
5. **Author a Combat stub-disposition** (H-V6-2): runtime gate, or Combat spec, or explicit CLAUDE.md documentation. Choose now to avoid the silent-stub trap.

Plus 1-LOC security fix (M-V6-1 sec: `Protocol.hpp:190` default flip), and the concurrency carry-forward considerations (`m_stale` comment tightening, SaveAllAndClear `try_lock_for`).

---

## Suggested triage order

**Decision-gated:**
1. **C-V6-1 disposition.** This blocks everything downstream. Ship option (a) bundle or option (b) cleanup-with-spec-update.

**This week (regardless of C-V6-1 path):**
2. **H-V6-1 startup self-check** in `OutboxRelay::Start` (~10 LOC, defense-in-depth).
3. **H-V6-3 protocol.json drift** — copy entries + CI gate or Editor extension.
4. **M-V6-1 security** — flip `RequiresAuthentication` default (1 LOC).
5. **M-V6-1 persistence WorldFlagStore dirty bits** — close before any handler tries to write a flag.

**Architectural decision needed:**
6. **H-V6-2 Combat disposition** — stub gate, or spec, or document. Pick one.
7. **H-V5-7 public_uid UNIQUE** — same decision class (drop the constraint or add the read path).

**Test backlog:**
8. Total-cap refusal test for `TcpServerBase` (the last H-V5-8 sub-item).
9. World-flag handler integration test once dirty bits land.
10. Combat-side handler tests once H-V6-2 disposition lands.

**Documentation:**
11. Tighten the `m_stale` Scope-4-batch-o-followup comment (L-V6-1 concurrency).
12. Update `OutboxRelay.hpp` DEFER block — the "no handler emits" premise is technically false.

**Audit infrastructure:**
13. Build a wiring-vs-instantiation script that diffs the migration spec's component inventory against `AccountServer.hpp`'s init list. Prevent C-V7-1 from being the 4th instance of the same defect class.

---

## Agent reports

Per-dimension findings (raw):
1. `2026-06-04-v6-followup-wiring-completeness.md`
2. `2026-06-04-v6-followup-persistence.md`
3. `2026-06-04-v6-followup-concurrency.md`
4. `2026-06-04-v6-followup-event-sourcing.md`
5. `2026-06-04-v6-followup-idempotency.md`
6. `2026-06-04-v6-followup-error-handling.md`
7. `2026-06-04-v6-followup-security.md`
8. `2026-06-04-v6-followup-networking.md`
9. `2026-06-04-v6-followup-cross-cutting-tests.md`

Synthesis written by the orchestrator session that ran the fleet (9 parallel agents).

---

## Methodology Note

This sweep took **9 parallel general-purpose agents** in a single fan-out, all 9 spawned in sequence at the start of the sweep. Each agent:
1. Read the v5 synthesis + v5 per-dimension followup + adjacent followups.
2. Received the orchestrator's brief on what Scopes 1-3-4 closed since v5, plus the user's verbatim anti-nitpick directive.
3. Audited current code state for both **v5-closure verification** AND **new findings**.
4. Wrote findings to disk (`docs/superpowers/audits/2026-06-04-v6-followup-<dim>.md`).
5. Returned a ≤300-word compact summary to the orchestrator (full reports stayed on disk).

The orchestrator (this session) maintained ~3KB of agent-result context (9 × ~300-word summaries) plus this synthesis, instead of inlining 9 full reports (~70KB combined). Total wall-clock for the 9-agent fan-out: ~19 minutes from spawn to last completion.

**Convergent flagging was strong** this sweep. The H-V6-1 EffectDispatcher cluster was independently surfaced by wiring + cross-cutting agents. The H-V5-1 closure was triple-confirmed by networking + security + error-handling. The C-V6-1 reducer-pipeline-dead headline was surfaced solely by the new wiring + completeness dimension — which is the strongest argument yet for keeping it as a permanent audit dimension.

**The orchestrator's initial brief was materially wrong about v5 status.** I had listed 6 v5 Highs as open (H-V5-1, H-V5-3, H-V5-4, H-V5-5, H-V5-6, H-V5-7); independent agent verification found all but H-V5-7 closed in commits between the v5 audit and this sweep. This is a reminder that "still open" assumptions decay quickly in an active codebase — the audit-of-record matters, and re-verification is the right default.

# v5 Follow-up Audit: Idempotency + Retry
**Date:** 2026-06-03 (same-day successor to v4, after the 20-commit v4 remediation arc)
**Scope:** End-to-end idempotency posture across `Server/Common/src/Idempotency/IdempotencyKey.hpp` (newly grown `ExtractClientKey`), `Server/Account/src/Cache/AccountRepository.hpp::FindIdempotency`, `Server/Account/src/Cache/AccountTransaction.hpp::StoreIdempotency`, `Server/Account/src/Db/OutboxRelay.hpp::SweepExpiredIdempotency`, schema.sql idempotency_cache, all 8 handler call sites (`GachaHandlers.hpp` ×2, `AccountHandlers.hpp`, `QuestHandlers.hpp`, `ProgressionHandlers.hpp` ×4), and `Client/src/network_tcp.lua`.
**Method:** Single-agent read-only sweep. Read v4 synthesis + v4 per-dimension followup + v3 per-dimension followup. Traced every `IdempotencyKey::Scoped` / `IdempotencyKey::ExtractClientKey` / `StoreIdempotency` / `FindIdempotency` / `SweepExpiredIdempotency` site, verified each v4 finding's status against current HEAD (`d65dbdf`), focused on the 7 carry-forward areas + the new `ExtractClientKey` adoption.

---

## Verdict

**Clean. Zero Critical, zero High, four Medium, six Low.** Every v4 idempotency finding has either landed or moved to a more-correct posture. The headline v4 Critical (C-V4-1, OutboxRelay never instantiated) is closed with the relay correctly placed between `m_pool` and `m_repository` in `AccountServer.hpp` — destruction order joins the worker thread before the pool tears down. ExtractClientKey adoption is **complete across all 8 production call sites** with the correct prefix per handler, no missed sites, no wrong-prefix adoptions. Five new direct tests exercise the cache layer's contract (round-trip, ON CONFLICT refresh, sibling-key disjointness, key-length boundary, cross-account isolation).

Three v4 Mediums explicitly survived the remediation: **M-V4-7 (cache-hit audit-log forensic blindness — Open)**, **M-V4-3 (admin_grant prefix mislabel — Deferred, requires coordinated rotation)**, and **M-V4-4 (sibling-key direction-blindness test — Open)**. The L-V4-7 log-key truncation, L-V4-5 doc-comment hoisting, and L-V4-8 schema doc cadence touch-up are also still open — none load-bearing.

One small new defect surfaced: the `idempotency_cache` table comment in `schema.sql:435` still says "runs every minute" despite the `b2f6049` doc-comment correction that resolved M-V4-1 inside the relay header. The schema comment is now the last place that contradicts reality.

---

## v4 → v5 status table

| v4 item | Status | Evidence |
|---|---|---|
| C-V4-1 OutboxRelay never instantiated | **Closed** | `AccountServer.hpp:79` `, m_outboxRelay(m_pool)` + member declaration at `:365`. Comments at `:71-78` + `:360-364` document the lifetime ordering. Commit `f24b3ca`. |
| H-V4-1 client `or uuid4()` fallback | **Partial** | Same shape as v4 — all 7 non-claim wrappers still mint at the inner-closure level (`network_tcp.lua:1217,1257,1350,1420,1428,1436,1444`). Claim is the canonical outer-mint pattern (`:1864`). No lint hook landed. Demoted to M-V5-1 (still pre-launch concern, but the F3/Inventory rework hasn't started so the regression window isn't open yet). |
| H-V4-2 AddCurrency cache scope (cached BuildStatePayload) | **Partial — documented** | Cached payload is still the full `BuildStatePayload` (`AccountHandlers.hpp:431,437`); ~24-field point-in-time surface unchanged. H-V4-9 (`3752b6a`) enumerated the surface in the `StoreIdempotency` doc-comment (`AccountTransaction.hpp:114-163`) so future field additions surface in the audit. Behavioral change (cache-minimal-ack-only, force GetState refresh) deferred. Demoted to M-V5-2. |
| H-V4-3 no idempotency machinery tests | **Closed** | `IdempotencyMachineryTest.cpp` (5 cases, +45 assertions): round-trip, ON CONFLICT refresh, sibling-key disjointness, length boundary, cross-account isolation. Commit `8ff1491`. |
| H-V4-9 M-V2-7 doc-comment wallet-only | **Closed** | `AccountTransaction.hpp:114-163` now enumerates per-handler scopes: pull/multi_pull (~16 fields), admin_grant (~24 fields), claim (~11 fields), progression handlers (per-handler). Forward-correctness rule added. Commit `3752b6a`. |
| M-V4-1 sweeper cadence doc | **Closed in header, Open in schema** | `OutboxRelay.hpp:63-71` updated to accurate "every 120/130/7200 pumps" arithmetic. `schema.sql:434-436` still says "runs every minute" — see L-V5-1. |
| M-V4-2 ON CONFLICT DO UPDATE racy on horizontal scale | **Deferred** | Single-instance contract unchanged. `AccountTransaction.hpp:282-307` policy preserved. Documented at v3-M3 / v2 followup. Carry-forward M-V5-3. |
| M-V4-3 admin_grant prefix mislabel | **Deferred** | Prefix still used at `AccountHandlers.hpp:354,423`. Rename requires coordinated client+server rollout. Documented carry-forward. |
| M-V4-4 sibling-key direction-blindness — no test | **Open** | Doc-comment at `QuestHandlers.hpp:478-492` is the entire defense. No collision test. Carry-forward L-V5-2. |
| M-V4-5 `request.value` throws on non-string | **Closed** | `IdempotencyKey::ExtractClientKey` (`IdempotencyKey.hpp:89-101`) type-guards; logs WARN on non-string; returns empty (request executes uncached). **All 8 handler sites adopted** — see ExtractClientKey audit below. Commit `b2f6049`. |
| M-V4-6 client max-byte assert | **Open** | `MAX_CLIENT_KEY_BYTES = 128` undocumented in `network_tcp.lua` wrappers; no Lua-side assert. Defense at server side is still correct (oversized → uncached execution, not 500). |
| M-V4-7 cache-hit audit log forensic blindness | **Open** | All 8 cache-hit short-circuits at `LOG_DATA_DEBUG` level only (`GachaHandlers.hpp:86,333`, `AccountHandlers.hpp:360`, `QuestHandlers.hpp:573`, `ProgressionHandlers.hpp:149,244,336,426`). No `audit_log` insertion on retry. Ops investigating "did the player retry this?" still see one row in audit_log — the original. **Carry-forward M-V5-4.** |
| L-V4-1 fixture vs idempotency_cache CASCADE | Confirmation, not bug | Unchanged. |
| L-V4-2 24h TTL vs mobile suspend | Open | Unchanged. |
| L-V4-3 deterministic keys reset cycle | Open | Unchanged. |
| L-V4-4 FindIdempotency no side-effect delete | Open | Unchanged. Now actually swept by C-V4-1's wired relay. |
| L-V4-5 sibling-key paragraph hoisting | Open | `QuestHandlers.hpp:478-491` doc-comment still inside the lambda body. |
| L-V4-6 claim-path outer-mint pattern documented | Confirmation | `network_tcp.lua:1860-1864` is the canonical example. |
| L-V4-7 log full-client-key leak | Open | All 8 sites still log full `clientKey`. |
| L-V4-8 schema.sql "every minute" doc | **Open** | Verbatim — `schema.sql:434-436` still claims "runs every minute"; actual cadence is 130 pumps × 500ms = 65s per the corrected `OutboxRelay.hpp:66` comment. Promoted from L-V4-8 → L-V5-1. |

---

## `ExtractClientKey` adoption audit

**Verdict: 8 / 8 sites adopted with the correct scope prefix; no missed sites; no wrong adoptions.**

Direct trace from `grep -n "ExtractClientKey\|IdempotencyKey::Scoped"` across `Server/Account/src/Handlers/`:

| Site | File:line | Scope prefix | Verified correct |
|---|---|---|---|
| HandlePull | `GachaHandlers.hpp:65,80` | `"pull"` | Yes |
| HandleMultiPull | `GachaHandlers.hpp:316,327` | `"multi_pull"` | Yes |
| HandleAddCurrency | `AccountHandlers.hpp:331,354` | `"admin_grant"` | Yes |
| HandleClaimQuestReward | `QuestHandlers.hpp:555,567` | `"claim"` | Yes |
| HandleLevelCharacter | `ProgressionHandlers.hpp:130,143` | `"level_char"` | Yes |
| HandleAscendCharacter | `ProgressionHandlers.hpp:225,238` | `"ascend_char"` | Yes |
| HandleLevelWeapon | `ProgressionHandlers.hpp:314,330` | `"level_wpn"` | Yes |
| HandleAscendWeapon | `ProgressionHandlers.hpp:404,420` | `"ascend_wpn"` | Yes |

The 8 prefixes are pinned by the `IdempotencyMachineryTest.cpp:136-139` sibling-key test, so any handler that adds a new prefix without extending that list will trip the test. Strong forward guard.

The helper itself (`IdempotencyKey.hpp:89-101`) is implemented correctly:
- field absent → empty (uncached execution)
- field present but JSON null → empty (uncached execution)
- field present but non-string → empty + LOG_DATA_WARN with observed JSON type (uncached execution)
- field present and string → returned

No `Scoped()` overload that takes `const Json&` was added — the design is `ExtractClientKey(json) → string`, then `Scoped(prefix, key)`. The v3-M4 fix suggestion ("centralize in `Scoped(prefix, request)`") was implemented in two steps instead of one; the call shape at every site is now:

```cpp
const std::string clientKey = IdempotencyKey::ExtractClientKey(request);  // type-safe
...
const std::string scopedKey = IdempotencyKey::Scoped("pull", clientKey);   // length+prefix
```

Both helpers return empty on rejection, so the existing `if (!scopedKey.empty()) { ... }` guard remains the single check. Clean.

---

## MEDIUM

### M-V5-1. Client `or uuid4()` fallback persists in all 7 non-claim wrappers
**Files:** `Client/src/network_tcp.lua:1217` (pull), `:1257` (multiPull), `:1350` (addCurrency), `:1420` (levelCharacter), `:1428` (ascendCharacter), `:1436` (levelWeapon), `:1444` (ascendWeapon)
**Status:** Carry-forward H-V4-1 → M-V5-1 (no inventory rework landed in v4 arc; regression window not yet open)

Verbatim from v4-H-V4-1: every wrapper defaults to `idempotencyKey or uuid4()`. The `or uuid4()` fallback is benign today because (a) no UI screen currently passes a key, and (b) claim's outer-scope mint pattern (`:1864`) is the canonical correct shape and the file comment at `:55-64` correctly explains the contract.

When the F3/Inventory rework lands (per `project_inventory_party_rework`), the UI MUST mint at `mousepressed`. Until then this is a documentation-and-future-lint concern, not an active defect — demoting from v4 High to v5 Medium reflects the actual exposure window.

**Fix (carry-forward):** Either drop the `or uuid4()` in the seven non-claim wrappers, OR add a Selene/lint hook flagging callers without the 4th arg, OR add to the inventory-rework spec a hard rule that the UI mints keys at gesture time. Pick one before the inventory work starts.

### M-V5-2. AddCurrency caches the full BuildStatePayload — 24-field point-in-time replay
**Files:** `Server/Account/src/Handlers/AccountHandlers.hpp:431,437` (cached `responseBody = BuildStatePayload(account)`)
**Status:** Carry-forward H-V4-2 → M-V5-2 (doc enumerated in H-V4-9, behavioral change deferred)

The H-V4-9 doc enumeration (`AccountTransaction.hpp:114-163`) now correctly names every cached field per handler — admin_grant caches the full ~24-field BuildStatePayload (identity + wallet × 5 + slot_pity + 4 story/streak/difficulty + 7 lifetime stats + party + 2 equipment maps). Future audit re-passes won't re-discover the surface; the surface is documented.

The remaining concern is behavioral: a 23h-old cached AddCurrency response replayed via cache-hit overwrites the client's freshest state. Today the only AddCurrency caller is the debug hotkey, so this is dormant. Future IAP / quest payouts will expose. The current sweeper (now actually running per C-V4-1's closure) bounds practical staleness at the 24h TTL.

**Fix options:** (a) cache a minimal `{success:true,grant_id:...}` ack and force the client to follow up with GetState (matches the pull-then-GetState pattern), (b) embed a `_cached_at` epoch into the cached payload so the client can detect retroactivity and refresh, (c) accept the current contract and document loudly at every AddCurrency UI integration site. Defer to the IAP integration spec.

### M-V5-3. ON CONFLICT DO UPDATE single-instance contract — undocumented in code
**Files:** `Server/Account/src/Cache/AccountTransaction.hpp:282-307` (the ON CONFLICT clause + audit-H2 comment)
**Status:** Carry-forward M-V4-2

The H2 doc-comment at `:282-291` correctly explains the refresh semantic but doesn't name the single-instance assumption. Pre-launch is single-instance; future horizontal scale would need either (a) a startup `pg_advisory_lock` asserting single-instance, OR (b) policy change to `DO UPDATE ... WHERE expires_at < now()` (only refresh expired rows; preserve fresh rows from racers).

**Fix:** at minimum extend the existing comment to "single-instance contract: two concurrent commits with the same scoped_key cannot occur because the per-player stripe lock serializes them; if horizontal scale is ever planned, switch the policy to `WHERE idempotency_cache.expires_at < now()` BEFORE deploy."

### M-V5-4. Cache-hit short-circuit emits no audit_log row — ops forensically blind to retries
**Files:** all 8 cache-hit sites (`GachaHandlers.hpp:86,333`, `AccountHandlers.hpp:360`, `QuestHandlers.hpp:573`, `ProgressionHandlers.hpp:149,244,336,426`)
**Status:** Carry-forward M-V4-7 (open)

Verbatim from v4-M-V4-7. The cache-hit path correctly emits no events and no audit row by design (the original commit IS the audit). But ops investigating "did the player retry this purchase?" only see one row in `audit_log` (the original commit) — the dedup is correct, the dedup is invisible. The log line `LOG_DATA_DEBUG("Idempotent X retry: player={} key={}", ...)` exists at every site but only surfaces at DATA_DEBUG verbosity — most ops setups won't run that loud in production.

A player ticket "this charge happened twice — refund me" investigator has no signal that a second call was deduped vs no second call happening. Forensically blind.

**Fix:** on the cache-hit path, write a lightweight `audit_log` row directly (NOT through `AccountTransaction.RecordAudit` — the txn isn't open and shouldn't be). One-shot INSERT outside any transaction; swallow errors. Schema:
```
action="idempotent_retry", actor=playerId, target={"scoped_key": scopedKey, "rpc": "<handler>"},
before=null, after=null
```
Cheap, ops-visible, no semantic concerns. Same scope as `RecordAudit` already supports (`audit_log` table at `schema.sql:401-416`), just bypasses the transaction wrapper since we're not bundling.

---

## LOW / OBSERVATION

### L-V5-1. schema.sql:434-436 still claims "runs every minute" for SweepExpiredIdempotency
**Files:** `Server/Account/schema.sql:434-436`

The b2f6049 commit corrected the OutboxRelay header comment to "every 120/130/7200 pumps" but missed the schema-side documentation. After C-V4-1's closure the sweeper actually runs now, so the schema comment's promise is real — but the cadence number is wrong (130 pumps × 500ms = 65s, not 60s).

**Fix:** one-line edit to `schema.sql:434-436`: "...in-process sweeper (`OutboxRelay::SweepExpiredIdempotency`) that runs every ~65 seconds to delete expired rows." Or just say "runs periodically" if the exact number is going to keep drifting.

### L-V5-2. M-V3-4 sibling-key direction invariant has no regression test
**Files:** `Server/Account/src/Handlers/QuestHandlers.hpp:478-491` (the invariant doc-comment); no test asserts the UNIQUE violation surfaces

Carry-forward M-V4-4. A future quest claim that both spends and grants the same currency would silently collide on the `:wallet:<currency>` sibling key. The current invariant is documented; an explicit test that constructs the colliding event bundle and asserts the UNIQUE violation would lock down the regression.

**Fix:** ~50-line integration test in `tests/Integration/` that builds two `wallet` events with the same `idempotency_key + ":wallet:credits"` suffix, attempts `txn.Commit()`, and asserts the unique-violation surfaces as `ConcurrencyConflict`.

### L-V5-3. No test exercises `SweepExpiredIdempotency` despite C-V4-1 closure
**Files:** `Server/Account/tests/Integration/OutboxRelayTest.cpp` (3 test cases, none covers Sweep)

The C-V4-1 fix wired the relay so SweepExpiredIdempotency now runs. There's no regression test that proves it ran — write a row with TTL=1s, advance time, kick the relay (or call `SweepExpiredIdempotency` directly via a friend / extracted helper), assert the row is gone. Would have caught C-V4-1 had it existed.

**Fix:** new test case in `OutboxRelayTest.cpp` (or a new `IdempotencySweepTest.cpp`). The relay's `SweepExpiredIdempotency` is private; either make it `protected` for friend access from tests, or expose a `KickSweepForTest()` debug method.

### L-V5-4. No test exercises a handler-level cache-hit short-circuit
**Files:** `Server/Account/tests/Integration/` (no test invokes `HandleX` and asserts the cache-hit branch returns without mutating state)

H-V4-3 closure (IdempotencyMachineryTest) covers the cache machinery (Store / Find / Scoped) directly but doesn't exercise the per-handler cache-hit short-circuit. The AddCurrencyEndToEndTest's "idempotent retry returns cached payload" (lines 126-176) asserts the cache row was written + only one event lands — it doesn't invoke `HandleAddCurrency` a second time and assert the cached branch fires.

**Fix:** add a test that drives `HandleAddCurrency` (or any of the 8 handlers) twice with the same `idempotency_key`, asserts:
- the second call returns the same payload byte-for-byte
- the events table has exactly one row for the action
- the wallet column reflects only one grant
- (post-M-V5-4) the audit_log has a row of type `idempotent_retry`

### L-V5-5. Log line at each cache-hit leaks full client key
**Files:** all 8 cache-hit `LOG_DATA_DEBUG` sites

Carry-forward L-V4-7. Benign when keys are UUIDs (the in-tree default); becomes a leak surface if a future client embeds structured info. Prefix-truncate to first 8 chars (same shape as v2-M-V2-14 token-prefix).

### L-V5-6. Sibling-key invariant paragraph still inside `emitDelta` lambda body
**Files:** `Server/Account/src/Handlers/QuestHandlers.hpp:478-491`

Carry-forward L-V4-5. 13 lines of doc-comment in the middle of a 78-line lambda body makes the function appear longer than it is. Hoist above the lambda or to class-level doc.

---

## Focus-question summary table

| # | Focus | Verdict |
|---|---|---|
| 1 | Client idempotencyKey wiring | Same shape as v4. claim is canonical (`:1864` outer-mint + executeWithRetry); 7 others mint via `or uuid4()` fallback in the inner closure. No regression yet because no UI caller passes a key. Demoted to M-V5-1. |
| 2 | Sibling-key direction-blindness (M-V3-4) | Doc-only at `QuestHandlers.hpp:478-491`; no test backstop. L-V5-2. |
| 3 | Cache-hit forensic blindness (M-V4-7) | Open — no `audit_log` row written on cache-hit. M-V5-4. |
| 4 | TTL choices | Effective TTL is now actually 24h (was unbounded under C-V4-1; relay now sweeps). L-V4-2 mobile-suspend concern carries forward. |
| 5 | Scope prefix collisions | 8 distinct prefixes pinned by `IdempotencyMachineryTest.cpp:136-139`. Test fails if a future handler reuses a prefix. Strong forward guard. |
| 6 | Partition-scoped UNIQUE vs cross-partition dedup (H-V3-12 carry-forward) | Documented at `QuestHandlers.hpp:320-333`. No new defects. Production path's actual cross-time dedup is `idempotency_cache.PK(account_id, scoped_key)` — un-partitioned, correct. |
| 7 | Client-side stable-key passthrough | Threaded all the way down at v3-H-V2-6 level; correctly preserved through `Network.X` → `IOClient.X` → `NetworkTCP.X`. Only the fallback shape is suspect. |
| 8 | ExtractClientKey adoption | **All 8 sites adopted with correct scope prefix**; no missed sites; no wrong-prefix adoptions. Type-guard works for absent/null/non-string. Test-pinned via `IdempotencyMachineryTest.cpp`. |

---

## Verified Closed (from v4, by direct trace)

| v4 Item | Status | Evidence |
|---|---|---|
| C-V4-1 OutboxRelay wired | Closed | `AccountServer.hpp:79,365`; commit `f24b3ca` |
| H-V4-3 idempotency machinery tests | Closed | `IdempotencyMachineryTest.cpp`; commit `8ff1491` |
| H-V4-7 populated round-trip + AddCurrency e2e | Closed | `PopulatedRoundTripTest.cpp` + `AddCurrencyEndToEndTest.cpp`; commit `9769d37` |
| H-V4-9 point-in-time doc enumeration | Closed | `AccountTransaction.hpp:114-163`; commit `3752b6a` |
| M-V4-1 sweeper cadence (header) | Closed in `OutboxRelay.hpp:63-71` | Schema doc still wrong — L-V5-1 |
| M-V4-5 type-safe key extractor | Closed | `IdempotencyKey.hpp:89-101` + 8 handler sites; commit `b2f6049` |

---

## Suggested triage order

**This week:**
1. **L-V5-1** — one-line schema.sql comment update to match the relay's actual cadence. ~1 LoC.
2. **M-V5-4** — write the `idempotent_retry` audit row on cache-hit short-circuit. ~40 LoC × 8 sites or a helper + 8 one-line calls. Single biggest forensic improvement.
3. **L-V5-3** — `SweepExpiredIdempotency` regression test. ~50 LoC. Pins C-V4-1 closure.
4. **L-V5-4** — handler-level cache-hit short-circuit test. ~80 LoC. Doubles as the M-V5-4 audit-row assertion.

**Before launch:**
5. **M-V5-1** — client `or uuid4()` fallback decision (drop / lint / spec-rule). Decide before F3/Inventory rework starts.
6. **M-V5-2** — AddCurrency cache scope decision (minimal-ack vs cached-state vs `_cached_at` marker). Decide as part of IAP integration spec.
7. **M-V5-3** — extend ON CONFLICT comment to name the single-instance contract. ~5 LoC.

**Pre-launch quality bar (cosmetic):**
8. **L-V5-2** — M-V3-4 sibling-key direction test.
9. **L-V5-5** — log-line key prefix truncation.
10. **L-V5-6** — hoist sibling-key invariant doc-comment.

---

## Sweep methodology notes

- Read v4 synthesis + v4 per-dimension idempotency followup + v3 per-dimension idempotency followup.
- Traced every `IdempotencyKey::Scoped` + `IdempotencyKey::ExtractClientKey` site (8 each, exactly the 8 production handlers).
- Verified `OutboxRelay` instantiation: `AccountServer.hpp:79` + `:365` (member declaration between `m_pool` and `m_repository`).
- Verified ExtractClientKey's body (`IdempotencyKey.hpp:89-101`): absent/null/non-string all return empty; non-string also logs WARN.
- Cross-checked the 8 prefixes against `IdempotencyMachineryTest.cpp:136-139` — every production prefix is in the test fixture; the test enforces distinctness.
- Searched for any `audit_log` insertion in cache-hit branches: zero matches. M-V4-7 unactioned.
- Verified `schema.sql:434-436` still claims "runs every minute" (drifted from `OutboxRelay.hpp:66`'s corrected 65s comment).
- Verified `network_tcp.lua` wrapper shape: 7 wrappers with `or uuid4()` inner-closure fallback, claim is outer-scope mint.

Read-only audit. Zero source modifications.

# Aphelyon Server — v3 Follow-up: Cross-Cutting / Integration / End-to-End

**Date:** 2026-06-03
**Auditor:** Cross-cutting / integration / E2E lens (v3 follow-up to the v2 per-dimension fleet)
**Method:** Walked the two highest-volume RPC flows end-to-end (claim_quest_reward, pull), audited the three-service RPC seam (Auth ↔ Account ↔ Combat) after C7-C's wire-shape change, cross-checked event payload struct↔reducer↔handler↔golden-file shapes for the four aggregates, traced the version-cursor lifecycle through Begin/Commit/Reload, and exercised the M-V2-2 X-macro Snapshot list against actual handler mutations.

**Required reading verified:** v1 audit, v2 synthesis, all 8 v2 per-dimension followups, full `git log 8e19666..HEAD` commit set (17 commits).

---

## Executive Summary

The v2 follow-up remediation landed cleanly. Every Critical and High in v2 closed in code, and the three step-wise refactors (AccountHydrator → AccountCache → InternalRpcHandlers; HandleClaimQuestReward helper extraction; Snapshot X-macro) sharpen the system at exactly the seams the v1/v2 audits flagged. The end-to-end claim and pull flows now correctly capture the Memento snapshot BEFORE handler mutations (C7-A), idempotency caches inside the same `pqxx::work` as events, and the RPC envelope MAC covers the on-wire bytes verbatim (C7-C). **However, the v3 surface still has 1 High, 5 Mediums, and 6 Lows that sit between the v2 dimension boundaries — issues that ONLY surface when looking at the system end-to-end rather than at any single lens.**

The headline finding is **H-V3-1**: The C7-C wire-format change (`params_json` opaque string) is a coordinated **breaking change** with no version negotiation, no startup canary, and no test coverage. Auth↔Account↔Combat must redeploy together; any version skew breaks every internal RPC silently with "Authentication failed". The commit message acknowledges this and waves "pre-launch, solo-dev" as the mitigation, but the same risk re-fires on every future envelope change. Plus three integration-shaped Mediums: (1) the schema's `events.event_type` column has no CHECK constraint, so the handler emits `"story_xp_gained"` for a `StoryLevelAdvanced` payload but the reducer doesn't dispatch on `event_type` at all — silent drift surface; (2) Snapshot X-macro vs. AccountTransaction Memento timing — there is no static_assert binding "everything mutated by a handler" to "everything in the X-macro list", so a future field is silently lost on Rollback; (3) the idempotency cache writes the **pre-Commit response** including post-mutation balances, so a future-state-snapshot semantic is locked in by the C7-A reorder (handlers can no longer build a response that captures pre-mutation balances even if they wanted to).

System-level health: **B+**. The fixes are correct; the seams between them are mostly correct; the gaps left are bounded by single-instance stripe-locking + the dev-DB pre-launch posture. None of the v3 findings are launch-blocking on their own. The persistent test-coverage gap is the systemic risk — the test suite still cannot detect a regression of any v2 or v3 fix outside C7-A's narrow lifecycle test.

---

## CRITICAL

*(none — v3 surface)*

---

## HIGH

### H-V3-1. Internal-RPC wire format change (C7-C) is breaking and has no canary
**Files:**
- `Server/Common/src/ServiceClient.hpp:131-136` (sender writes `params_json` as opaque string)
- `Server/Common/src/ServiceEndpoint.hpp:162-178` (receiver reads `params_json`, requires the field)
- C7-C commit (`0d014293`)

**Status:** NEW — load-bearing single-shot migration. **No version negotiation, no fallback path, no startup canary.**

Old wire: `{"method":..., "params":{...}, "ts":..., "mac":...}` (nested object)
New wire: `{"method":..., "params_json":"<string>", "ts":..., "mac":...}` (opaque string)

If an operator runs Auth (new build) + Account (old build) or vice versa, every internal RPC returns "Authentication failed" with no diagnostic. `request.value("params_json", std::string{})` defaults to empty when the field is missing → MAC computed over an empty string → mismatch. The MAC failure logs `LOG_NET_WARN("ServiceEndpoint: rejecting RPC '{}' — MAC verification failed", method)` which looks identical to a real secret-mismatch.

A startup canary RPC ("ping" or version exchange) would convert this from a silent runtime failure into a fail-fast at boot. None exists. The commit message says "Wire format change — Auth, Account, Combat must redeploy together. Pre-launch, solo-dev environment so coordinated restart is the norm." True for now; the precedent — wire-shape change as breaking sweep — re-fires every time the envelope changes.

**Adjacent issue (same family):** `ServiceClient::Call` returns empty `Json()` on every failure mode (connect, send, recv timeout, parse, MAC reject). SessionCache::Validate then routes ALL of these into the same "Auth-down" extension path. A subtle mismatch (MAC bypass attack, schema drift, garbled cert) is indistinguishable from "Auth process crashed" — no separate observability.

**Fix (preferred):**
1. Add a `version` field to the envelope; receiver returns "Unsupported protocol version" with a distinct error string the client can log specifically.
2. On startup, every service that owns a `ServiceClient` to another service runs a one-shot `Ping` RPC and refuses to start if it fails for any reason other than "service unreachable" (which falls back to retry loop). This converts wire-skew into a fail-fast startup error.
3. The `ServiceEndpoint::HandleConnection` catch-all at line 200-204 should distinguish "MAC failure" from "JSON parse failure" from "handler exception" in the response — currently they all flow through the same `"error": "..."` shape and the sender can't tell which.

---

## MEDIUM

### M-V3-1. Schema `event_type` is unconstrained TEXT; handler emits an event_type the reducer doesn't recognize
**Files:**
- `Server/Account/schema.sql:283` (`event_type TEXT NOT NULL` — no CHECK)
- `Server/Account/src/QuestHandlers.hpp:477` (emits `"story_xp_gained"` when level didn't change)
- `Server/Account/src/reducers/ProgressionReducer.hpp:14-32` (reducer dispatch ignores `event_type` — folds purely on payload)
- `Server/Account/tests/events/v1_story_level_advanced.json` (golden uses `"story_level_advanced"`)
- `Server/Account/tests/GoldenFile/SchemaMigrationTest.cpp:128-131` (only tests the `story_level_advanced` event_type)

**Status:** NEW — silent drift between handler and reducer event vocabulary.

`BuildClaimEvents` in QuestHandlers emits one of two event types based on `(postStoryLevel != pre.preStoryLevel)`:
- `"story_level_advanced"` when level moved
- `"story_xp_gained"` when only XP changed

Both carry the same `StoryLevelAdvanced` payload struct, with `from_level == to_level` in the xp-only case. `ProgressionReducer::Apply` dispatches purely on the payload — `event_type` is a parameter it ignores. So today this works correctly (the reducer folds both event_types identically). But:

1. There is no schema-level constraint that limits `event_type` to a known vocabulary.
2. The golden file test only covers `"story_level_advanced"`; the `"story_xp_gained"` event_type has zero golden coverage.
3. Anyone reading the schema or the events table for analytics will see TWO event types for what's structurally one event.
4. If a future reducer rewrite DOES start dispatching on `event_type`, it must explicitly add `"story_xp_gained"` to its switch or events get silently dropped during replay.

**Compounding:** events appended with `event_type="story_xp_gained"` would NOT match a hypothetical `WHERE event_type = 'story_level_advanced'` query that an analytics or sweeper job might use.

**Fix:** Either (a) emit only `"story_level_advanced"` for both cases (the payload disambiguates), (b) add `"story_xp_gained"` to the golden file fixture set, or (c) add a CHECK constraint enumerating all event_types — this would force a schema change every time a new event_type is added but locks the vocabulary down.

### M-V3-2. Snapshot X-macro coverage is enforced by convention, not by the type system
**Files:**
- `Server/Account/src/Account.hpp` (M-V2-2 X-macro: `APHELYON_ACCOUNT_SNAPSHOT_DIRECT_FIELDS` + 2 manual indirect entries for `rngState`, `collectionState`)
- `Server/Account/src/GachaHandlers.hpp:117-193` (pull handler mutates: wallet, RNG, pity, guarantee, stats, collection, dirty bits)
- `Server/Account/src/QuestHandlers.hpp:551-583` (claim mutates: quest_states, wallet, story_level, story_xp, login_streak, difficulty_tier)

**Status:** NEW (drift surface introduced by M-V2-2)

The M-V2-2 X-macro reduces Account::Snapshot drift from 3-place edits to 1-place edits, which is a real improvement. But the X-macro list (the SET of fields directly snapshotted) is not bound to the SET of fields a handler can mutate. The contract "every mutable Account field appears in either the X-macro list or the explicit indirect block in CaptureSnapshot/RestoreFrom" is enforced by:
- Reviewer attention
- A code comment

There is no static_assert, no template specialization, and no test that exhaustively iterates every Account setter and asserts "the snapshot restored after a mutation matches the snapshot taken before." The `[c7]` test from v1 covers 6 of ~24 fields (per the v2 test-coverage audit).

The concrete failure mode: a future engineer adds `std::vector<UnlockedTitle> m_titles` + `SetTitle()` setter, forgets the X-macro entry, and Rollback silently leaves the title set mutated. C1 stale-flag eviction still rescues on the next handler entry, but the in-handler "the snapshot worked" assurance is gone for that field.

**Fix:** Either (a) add a `[c7-coverage]` test that uses the X-macro to walk every field and round-trip it (the X-macro is the lever — expand it 4 ways: decl, capture, restore, **and** a test-walk), or (b) add a `static_assert(sizeof(Snapshot) == ...)` tripwire (cheap, signals when fields are added) — neither catches type-only drift but both raise the visibility floor.

### M-V3-3. Idempotency cache locks in post-mutation response semantics; client must refetch state on cache hit
**Files:**
- `Server/Account/src/GachaHandlers.hpp:270-291` (response body built AFTER mutations — pull case)
- `Server/Account/src/QuestHandlers.hpp:609-627` (response body built AFTER mutations — claim case)
- `Server/Account/src/AccountTransaction.hpp` StoreIdempotency call sites
- `Server/Account/src/db/RelationalFlush.hpp` (StoreIdempotency note about point-in-time semantics, per M-V2-7)

**Status:** NEW (interaction between C7-A reorder and M-V2-7 semantic doc)

C7-A moved `Begin()` to the TOP of each event-sourced handler so the Memento captures pre-mutation state. M-V2-7 documented that the idempotency cache `response_payload` is point-in-time. Both are correct individually. Combined: the cached response embeds wallet balances captured AFTER the mutation, by design — the client requesting a retry an hour later sees the wallet state from T1 even if the server-driven AddCurrency grant landed in between.

For pull/claim flows the client correctly issues a `GetState` refresh after the response, so UI stays current. For ProgressionHandlers (Level/Ascend × Char/Wpn), there's no follow-up `GetState`. A retry of a level-up that completed at T1 returns T1's scrap count — the player sees a "you have 500 scrap" UI even though a quest reward at T1.5 granted them another 200.

**Compounding:** the H-V2-6 fix wired the idempotencyKey param down through ProgressionService client wrappers, but no caller passes a key today (the audit noted this). So this latent bug is dormant until a caller starts passing keys — at which point the long-tail retry semantics suddenly become observable.

**Fix:** Either (a) document the contract in the client services that idempotent retry payloads MUST be supplemented by a state refresh, or (b) on cache hit, the server re-reads the live wallet balances and patches the cached payload's `wallet`/`scrap` fields. Option (b) trades the byte-exact retry guarantee for UI consistency; option (a) is the lower-effort choice.

### M-V3-4. `ServiceClient::Call` collapses all failure modes — operator can't tell MAC failure from network drop
**Files:**
- `Server/Common/src/ServiceClient.hpp:86-203` (single return-type: empty `Json()` on every failure)
- `Server/Common/src/SessionCache.hpp:117-126` (treats empty as "Auth-down" → enters extension path)

**Status:** NEW

`ServiceClient::Call` returns `Json()` on:
- Connect refused
- Send failed (broken pipe)
- Recv timeout
- Recv buffer overflow
- Parse error
- JSON exception during parse

All six paths look identical to the SessionCache. The SessionCache then treats this as "Auth unreachable" and routes through the H6 extension path. This is correct for connection-level failures but wrong for MAC failures or parse failures — those are integrity events that should fast-fail the session instead of extending it.

The C7-C wire-shape change (H-V3-1 above) makes this worse: a deploy skew or future envelope change manifests as "Auth-down with H6 extensions" rather than "RPC integrity failure" — the wrong observability shape for the actual problem.

**Fix:** Return a `RpcCallResult { enum { Ok, Unreachable, Integrity, Parse, Other }, Json payload }` instead of bare `Json`. SessionCache only enters the extension path on `Unreachable`. Integrity/Parse failures fail-closed.

### M-V3-5. `idempotency_cache` table has no sweeper; `expires_at` column is filter-only
**Files:**
- `Server/Account/schema.sql:365-388` (table + comment "a cleanup daemon will sweep the table later")
- `Server/Account/src/AccountTransaction.hpp` (IDEMPOTENCY_DEFAULT_TTL_SECONDS = 24h)

**Status:** VERIFIED-OPEN (v2 M1 idempotency followup flagged; no remediation)

The v2 idempotency followup estimated 50k rows/day → ~5 GB/year. Not immediate. But cross-cutting: this is the ONLY table the system writes durably for which no expiry sweep exists. `partman` handles `events` partition rotation; `outbox` has a manual purge; `audit_log` is intentionally append-only. `idempotency_cache` is the odd one out.

**Fix:** Single periodic `DELETE FROM idempotency_cache WHERE expires_at < now() - interval '1 hour'` on the OnCleanupTick thread (already runs CleanupIdleAccounts every 5 min). Trivially folded into the existing tick — 10 lines.

---

## LOW / OBSERVATION

### L-V3-1. `events.idempotency_key` UNIQUE includes `created_at` — cross-partition collisions silently allowed
**File:** `Server/Account/schema.sql:293`
The schema's `UNIQUE (account_id, idempotency_key, created_at)` doesn't actually enforce global dedup across partitions; same gotcha as the v1-C6 version-UNIQUE pre-fix. The deterministic claim key `claim:<questId>:<accountId>` will collide with itself only within a single partition. Daily quests claimed on day 1 and day 2 across a partition boundary slip through. The reducer's `claimed_quest_ids` set catches in-memory; the SQL invariant does not. Document and rely on the reducer.

### L-V3-2. `QuestRewardClaimed.claimed_at_streak_day` field name is misleading (M16 + L-Q5 carryover)
**Files:** `Server/Account/src/events/QuestClaimEvents.hpp:28`, golden file
Already noted in v2 code-quality L-Q5; flagged again here because cross-cutting analytics queries reading "claimed_at_streak_day = 7" will interpret as "calendar day 7 of streak" not "7-day consecutive streak count." Rename to `streak_count_at_claim`.

### L-V3-3. Golden file vocabulary gap: only 4 event types have v1 fixtures
**Files:** `Server/Account/tests/events/v1_*.json` (4 fixtures)
The system emits at least 5 event_types: `pull_performed`, `credits_spent` / `credits_added`, `quest_reward_claimed`, `story_level_advanced`, `story_xp_gained` (M-V3-1). The golden suite covers 4. Any future schema migration that touches `story_xp_gained`'s payload has zero gate.

### L-V3-4. Snapshot indirect-field block is comment-enforced
**File:** `Server/Account/src/Account.hpp` (CaptureSnapshot/RestoreFrom for rngState + collectionState)
M-V2-2 split fields into "direct" (X-macro) and "indirect" (manual). The manual block is comment-enforced: "Listed explicitly in CaptureSnapshot / RestoreFrom so any new indirect field joins them visibly rather than slipping past unnoticed." OK as a convention but undefended by tests. A future `WorldFlagStore` refactor that moves its mutation behind a getter/setter pair (and therefore becomes "indirect") needs both Snapshot sites updated.

### L-V3-5. APHELYON_QUEST_TOKEN_SECRET Release-fatal check lives in Account `main.cpp` only — correctly scoped, but worth confirming
**Files:** `Server/Account/src/main.cpp:114-127`, `Server/Auth/src/main.cpp`, `Server/Combat/src/main.cpp`
Verified: Auth and Combat do NOT need APHELYON_QUEST_TOKEN_SECRET — quest tokens are an Account-internal concept (minigame challenge tokens). The env-var enforcement matrix is:
- `APHELYON_INTERNAL_SECRET` → required Release-fatal in Auth + Account + Combat ✓
- `APHELYON_QUEST_TOKEN_SECRET` → required Release-fatal in Account only ✓
- `APHELYON_DB_CONNECTION` → optional override in Account only ✓
Correctly scoped; documenting here so future audits don't re-flag.

### L-V3-6. Aggregate-version cursor reload cycle has no exhaustive test
**Files:** `Server/Account/src/AccountRepository.hpp` LoadByAccountId / LoadEventVersions, `Server/Account/src/AccountTransaction.hpp` Commit cursor advance
The cycle is: ctor captures cursor → handler builds events with `cursor+1` → AppendInTx pre-check via `SELECT MAX` → COMMIT → Commit advances `cached_*_version` → next ctor reads the advanced cursor → ... → on reload, LoadEventVersions reads `MAX(version)` from DB and restores `cached_*_version`. The cycle is correct (verified by code-read), but no test exercises the full mutate→commit→reload→mutate path for all 4 aggregates. A regression that drops the cursor-advance in Commit (e.g., a future refactor that moves the loop) would not be caught by the existing suite. The v2 test-coverage audit flagged this as "AccountRepository populated round-trip" item #3.

---

## End-to-End Flow Walkthroughs

### Claim Quest Reward — end-to-end

1. **Client UI button** → `claimQuestReward(questId, callback)` in `Client/src/network_tcp.lua:1864` generates UUID **outside the closure** (correct H13 pattern).
2. **Auth validates session** via SessionCache → `AuthorizeToken` RPC over the C7-C wire shape.
3. **Account.HandleClaimQuestReward** (`QuestHandlers.hpp:486`):
   - ParseJsonStrict (M11/H-V2-7)
   - questId / clientKey extracted
   - QuestDefinition lookup
   - `getLockedAccount` → stripe lock acquired
   - IdempotencyKey::Scoped("claim", clientKey) → cache check via FindIdempotency (tri-state)
   - TickQuests::Apply
   - `q->IsClaimed()` / `IsCompleted()` checks
   - ClaimPreState snapshot
   - **`m_ctx.repository->Begin(account)` — Memento captured here** (C7-A correct)
   - `q->state = Claimed`
   - `ApplyClaimRewards` (mutates wallet + story state)
   - `AdvanceStreakIfNewDay` (login bridge)
   - `AdvanceDifficultyTier` (breakthrough bridge)
   - `PropagateClaim` + `UnlockEligibleQuests`
   - **BuildClaimEvents** — emits claim + 0–5 wallet deltas + optional progression (H-V2-9 deterministic sibling keys)
   - Build response body
   - `txn.AppendEvent` × N + `txn.StoreIdempotency` + `txn.Commit`
4. **Commit** runs `AppendInTx` per event (M-V2-5 advisory lock + SELECT MAX pre-check) → relational flush → outbox → audit → idempotency cache → `tx_->commit()` → cursor advance → ClearDirty.
5. **Response** serialized + returned. Retry with same scopedKey → cache hit → cached payload returned.

**Verdict:** End-to-end clean. C7-A timing is correct (Begin before mutation). H-V2-9 deterministic sibling keys are derived from `claim.idempotency_key + ":wallet:<currency>" / ":progression"`. The one cross-cutting observation is M-V3-3: the cached response embeds post-mutation wallet balances, locked-in by the C7-A reorder.

### Pull — end-to-end

1. **Client UI** → `Network.pull(slotId, handler)` — UUID generated PER NETWORK CALL (H13 anti-pattern). `GachaService:isLoading` debounce mitigates double-tap.
2. **Auth validate** → same path.
3. **Account.HandlePull** (`GachaHandlers.hpp:51`):
   - ParseJsonStrict
   - Banner / SlotDefinition / PityConfig lookup
   - `CanAffordPullByType` pre-check (so TrySpend below can't fail)
   - Capture RNG/pity/guarantee/wallet pre-state
   - **`m_ctx.repository->Begin(account)`** — C7-A correct
   - `TrySpendForPullByType`
   - `banner.Pull` (advances RNG + pity)
   - Collection mutation
   - Build pullEvent + walletEvent (H9 deterministic event keys from scopedKey)
   - Build response (includes post-mutation balances)
   - `txn.AppendEvent × 2 + StoreIdempotency + Commit`

**Verdict:** End-to-end clean. Same C7-A correctness as claim. One observation: the `pullEvent.idempotency_key` fallback when no scopedKey is provided is `"pull:<accountId>:<freshUuid>"` — by-design audit marker, not true dedup (acknowledged in v2).

---

## Verified Closed Cross-Cutting (v1+v2)

- **C7-A** — Begin() is at the top of all 5 event-sourced handlers (HandlePull, HandleMultiPull, HandleAddCurrency, HandleClaimQuestReward, all 4 progression). Verified by reading each.
- **C7-B** — `HandleResumeSession` routes through `ValidateSession` (later `GetValidSession` in H-V2-4/8 commit). IP + idle + expiry checks all run.
- **C7-C** — `params_json` opaque string carries the canonical bytes both directions; MAC verifies without re-serialization.
- **H-V2-2** — schema `DEFAULT 1` → `DEFAULT 0` for `difficulty_tier`; struct + reducer agree on 0 baseline.
- **H-V2-3** — `VerifyCredentials` re-reads under stripe lock after PBKDF2.
- **H-V2-4** — ResumeSession rate-limited via `m_loginLimiter` reuse.
- **H-V2-5** — Dummy hash precomputed at server start, run on `LoadByUsername` miss.
- **H-V2-6** — `idempotencyKey` parameter threaded through ProgressionService + addCurrency caller (still no UI caller, as v2 noted).
- **H-V2-7** — `HandleReportQuestProgress` + `HandleCompleteQuest` migrated to `ParseJsonStrict`.
- **H-V2-8** — `GetValidSession(token, clientIP, bumpIdle)` exists; three call sites migrated.
- **H-V2-9** — Quest claim sibling events derive idempotency_keys from `claim.idempotency_key + ":wallet:<currency>"` / `":progression"`.
- **M-V2-1** — `CleanupIdleAccounts` logs Save() failures.
- **M-V2-2** — Snapshot X-macro hoists type aliases + drives Capture/Restore from one list.
- **M-V2-3** — AccountServer.hpp split into AccountHydrator + AccountCache + InternalRpcHandlers; AccountServer.hpp now 345 lines (was 683).
- **M-V2-4** — HandleClaimQuestReward extracted into ApplyClaimRewards + BuildClaimEvents helpers.
- **M-V2-5** — `pg_advisory_xact_lock` at the top of `AppendInTx`.
- **M-V2-6** — Cursor advance now runs BEFORE ClearDirty in Commit.
- **M-V2-7** — Idempotency response payload documented as point-in-time.
- **M-V2-8** — `audit_log.target` null-Json normalized to "{}" with WARN.
- **M-V2-9** — `LogAuthStateChange` split into `NoteAuthLost` / `NoteAuthRecovered`.
- **M-V2-10** — RateLimiter switched to LRU eviction via reusable `LruCache<K,V>`.
- **M-V2-11** — `InternalRpcAuth::GetSharedSecret` caches via function-local static.
- **M-V2-12** — Quest token secret Release-fatal in `Account/main.cpp`.
- **M-V2-13** — `APHELYON_DB_CONNECTION` env-var override.

---

## Cross-Service Env-Var Matrix (verified)

| Variable | Auth | Account | Combat | Release-fatal? | Test-fixture default? |
|---|---|---|---|---|---|
| `APHELYON_INTERNAL_SECRET` | ✓ | ✓ | ✓ | yes (all 3) | dev fallback |
| `APHELYON_QUEST_TOKEN_SECRET` | — | ✓ | — | yes (Account) | per-process random |
| `APHELYON_DB_CONNECTION` | — | optional override | — | no | hardcoded dev DSN |

All three main.cpp files correctly enforce their required vars at startup before any pool/server/RPC init. No cross-service mismatches.

---

## Triage Suggestion

**Before next deploy:**
1. **H-V3-1** — Add envelope `version` field + startup `Ping` canary. Closes the wire-skew silent-failure class for all future envelope changes.
2. **M-V3-1** — Pick one event_type for the story-XP/level case (recommend `story_level_advanced` for both) OR add `story_xp_gained` golden fixture.
3. **M-V3-5** — Wire idempotency-cache sweeper into the existing OnCleanupTick.

**This week:**
4. **M-V3-2** — Add `[c7-coverage]` test that walks the X-macro list and round-trips every direct field.
5. **M-V3-3** — Document point-in-time idempotency semantic in `Client/src/services/ProgressionService.lua` or add server-side balance-patch on cache hit.
6. **M-V3-4** — Surface `RpcCallResult` enum from `ServiceClient::Call` so SessionCache can distinguish integrity failures from unreachability.

**Eventually:**
7. L-V3-1 through L-V3-6 — schema/comment polish, additional golden fixtures, test coverage expansion.

---

## System-Level Health Verdict

**B+.** The v1/v2 fixes are correct; the seams between them work; the v3 surface has no Critical findings. The deferred test-coverage backlog remains the single largest systemic risk — every fix shipped over the past 48 hours has weak or nonexistent regression coverage. The v2-to-v3 delta itself adds 17 commits across event-sourcing, security, error handling, idempotency, refactoring, and client plumbing; the test-suite delta over the same range adds ~20 assertions for LruCache + 2 v2 lifecycle tests. The ratio is the concern, not any one of the v3 findings.

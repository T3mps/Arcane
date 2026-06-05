# v3 Follow-up Audit: Idempotency + Retry
**Date:** 2026-06-03
**Scope:** Idempotency end-to-end after v1 (`2026-06-02-server-persistence-audit.md`) + v2 (`2026-06-02-server-persistence-audit-v2.md`) remediations across `Server/Account/src/{AccountTransaction,db/EventStore,GachaHandlers,AccountHandlers,QuestHandlers,ProgressionHandlers,AccountRepository,AccountCache}.hpp`, `Server/Common/src/IdempotencyKey.hpp`, `Server/Account/schema.sql`, `Client/src/network_tcp.lua`, `Client/src/services/{ProgressionService,gacha_actions}.lua`.
**Verdict:** **2 High, 4 Medium, 4 Low. No Criticals.** Every named v1/v2 fix is now correctly wired in code. The remaining gaps fall into three buckets: (a) durability of the deterministic-key idea on quest claim is still narrow, (b) the cache table still has no sweeper and the per-row response still carries point-in-time data without a client-side contract note, (c) the client-side `idempotencyKey` parameter is wired all the way down to `Network.*` but no UI caller mints one.

---

## Verdict — verified closed from v2

- **H-V2-9 (deterministic sibling keys).** Confirmed at `QuestHandlers.hpp:423,446,478`. `claimEvent.idempotency_key = "claim:" + questId + ":" + accountId`; siblings now derive as `bundle.claim.idempotency_key + ":wallet:" + cidStr` and `+ ":progression"`. The previous fresh-`claimEventId`-based UUIDs are gone. Cross-currency collisions impossible (cid suffix), claim↔progression collision impossible (suffix vocabulary disjoint from `"wallet:<cid>"`).
- **C7-C (wire format).** `ServiceClient::Call` (line 126-138) sends `params_json` as the exact byte string the sender MAC'd. `ServiceEndpoint::HandleConnection` (line 162-178) verifies against that same string with no re-serialization. Only one sender + one receiver in the whole tree (`grep params_json` confirms). No envelope-construction bypass.
- **M-V2-7 (point-in-time semantics).** Documented in `AccountTransaction.hpp:114-126` — the `StoreIdempotency` doc-comment explicitly states the cached `response_payload` is captured AT COMMIT TIME and an idempotent retry sees T1 wallet balances even if T2 has drifted. No re-fetch.
- **H-V2-6 (client-side threading).** `ProgressionService.{level,ascend}{Character,Weapon}` (lines 134-167) all forward `idempotencyKey` as 4th arg → `Network.X` → `NetworkTCP.X` (lines 1417-1444) → wire payload's `idempotency_key`. Vararg pass-through in `IOClient.lua:83-86` preserves the trailing nil → `or uuid4()` fallback at the wrapper. `gacha_actions.lua:162-167` mints two distinct keys per hotkey press (one per currency).
- **C7-A (snapshot timing).** All 5 event-sourced handlers (`HandlePull`, `HandleMultiPull`, `HandleAddCurrency`, `HandleClaimQuestReward`, the 4 progression handlers via `CommitProgressionScrapSpend`) confirmed to call `m_ctx.repository->Begin(account)` BEFORE any mutation. `AccountTransaction` ctor's mem-initializer list (line 49) captures `account.CaptureSnapshot()` as the Memento. Pre-mutation snapshot now load-bearing.

---

## High

### V3-H1. Quest-claim deterministic-key race window still under-defended
**Files:** `Server/Account/src/QuestHandlers.hpp:423,446,478,549`, `Server/Account/schema.sql:287,293`
**Focus:** questions 1+8

The deterministic event-level key (`"claim:<questId>:<accountId>"`) provides a fence against two concurrent claims of the same quest **only within a single Postgres partition**. The events table's UNIQUE constraint is `(account_id, idempotency_key, created_at)`; `created_at` defaults to `now()` and partitioning is monthly. Two claims that straddle a monthly partition boundary (one at 23:59:59.999 on the last day of month N, one at 00:00:00.001 on month N+1) hit DIFFERENT partitions, the UNIQUE constraint does not span partitions, and both INSERTs succeed at the schema level.

Defenses that actually fire today:

1. **`q->IsClaimed()` check at line 529** — pure in-memory. Second claim after first has committed sees `state == Claimed` and rejects with "Quest already claimed".
2. **Stripe lock + idempotency_cache PK** — `(account_id, scoped_key)` on `idempotency_cache` is NOT partition-keyed (no partitioning on that table), so the cache write provides true cross-time dedup for the scoped client key.
3. **`AppendInTx`'s `SELECT MAX(version) FOR UPDATE`-equivalent (via `pg_advisory_xact_lock` — M-V2-5)** — same-account commits serialize, so versions chain monotonically.

So the deterministic key does NOT itself provide the dedup guarantee its comment implies. It's a defense-in-depth marker: useful for forensic / audit queries (`SELECT ... WHERE idempotency_key LIKE 'claim:%'` finds every claim with metadata in the key), and a backstop if two parallel claims somehow both pass the `IsClaimed()` check on different node instances (which the project doesn't support today). But the audit comment at line 444 ("sibling derives from the claim's deterministic key for event-level dedup safety") oversells what the schema actually enforces.

**Concurrent-claim race (focus Q8) — does the second hit no cache?** Today: no. The stripe lock at `AccountCache::GetLockedAccount` serializes both claims. Second waits → reads cache via `FindIdempotency` → Hit on the scoped_key written by the first. **Unless** the second claim races with the first across the C1 `IsStale()` boundary (first Commit threw post-cache-write → MarkStaleForReload → second sees stale → erase → reload from DB). In that path the cache row from the first's COMMIT did land (idempotency_cache write is in the same pqxx::work as the commit; if commit succeeded the cache row landed). So the second's FindIdempotency still hits. Safe-by-construction today.

**Cross-partition vulnerability (focus Q5).** `AppendIdempotent`'s post-conflict probe at `EventStore.hpp:112-114` queries the partitioned `events` table by `(account_id, aggregate_kind, idempotency_key)`. Postgres routes the query across all partitions (no `created_at` predicate), so the probe DOES see a same-key row regardless of partition. The probe is correct. The risk is the OPPOSITE direction: a real duplicate INSERT in a different partition is NOT prevented by the UNIQUE constraint (which is partition-local), so `AppendIdempotent` only catches it if the INSERT happens to land in the same partition as the original. With monthly partitions and per-player stripe locks, the practical exposure is sub-millisecond windows at partition-rollover instants × concurrent inter-instance traffic — zero today (single-process) but the schema-level guarantee is weaker than the comment suggests.

**Note:** `AppendIdempotent` is only called from `EventStoreRoundTripTest.cpp:96`. Production `AccountTransaction::Commit` uses `AppendInTx`, which has NO idempotency_key dedup at all — it relies purely on the version pre-check. So the partition-spanning dedup discussion is moot for the production path; the only real-world dedup guarantee is `idempotency_cache.PRIMARY KEY (account_id, scoped_key)` (un-partitioned).

**Fix:**
1. Document at `QuestHandlers.hpp:423` and `EventStore.hpp:96` that `events.UNIQUE (account_id, idempotency_key, created_at)` is **partition-local**, NOT global, and `idempotency_cache.PRIMARY KEY (account_id, scoped_key)` is the actual cross-time dedup for client-driven retries.
2. Consider adding a global UNIQUE index on `events (account_id, idempotency_key)` via `CREATE UNIQUE INDEX CONCURRENTLY ON events_default ...` only if/when the project ships a from-genesis replay path; today the events.UNIQUE is decorative.
3. (Optional) Re-tag the column `events.idempotency_key → events.event_dedup_marker` to reflect that it's audit-shape not dedup-shape (continuation of v1-H9).

### V3-H2. Idempotency cache has no sweeper; growth is unbounded
**Files:** `Server/Account/schema.sql:370-378`, no sweeper in `Server/Account/src/`
**Focus:** question 9 (anything missed)

Verbatim from v2 followup-idempotency M1, still open. With M6's 24h TTL and per-call fresh UUID fallback (every committed mutation by every wrapper that doesn't take a UI-minted key writes a unique row that lives 24h then becomes invisible-but-not-deleted), the table grows monotonically until the documented "cleanup daemon" lands. Indexed `idempotency_cache_expires_idx` is sized for the future sweep but no daemon code exists.

Schema comment at line 376-378 explicitly says "a cleanup daemon will sweep the table later — until launch the entries are small enough to ignore." That's been the plan for two audit rounds without movement.

Estimate (verbatim from v2 followup): 50k rows/day at 1k DAU × 50 idempotent ops/player/day, ~300 bytes each → ~15 MB/day → ~5 GB/year. Plus disk-scan cost on every `FindIdempotency` since `expires_at > now()` filter scans the index leaves through expired rows.

**Fix:** Add a periodic `DELETE FROM idempotency_cache WHERE expires_at < now() - interval '1 hour'` on a 5-min tick, ideally hosted alongside `AccountCache::CleanupIdleAccounts` so it runs on the existing maintenance thread. Mark as **pre-launch blocker** — accumulation past 30 days starts to hurt FindIdempotency query latency on the index.

---

## Medium

### V3-M1. Client-side `idempotencyKey` is dead in tree; preparatory plumbing only
**Files:** `Client/src/services/ProgressionService.lua:134-167`, `Client/src/services/gacha_actions.lua:149-169`, `Client/src/network_tcp.lua:1205-1444`
**Focus:** question 4

Verified: the H-V2-6 fix added the param to `ProgressionService.*` and `gacha_actions.debug_add_tickets`, and the param flows correctly through `Network.X` → `IOClient.X` (varargs) → `NetworkTCP.X` → wire payload. No UI screen in the project currently passes a key. Comment at `ProgressionService.lua:122-126` is honest about this ("preparatory plumbing — Pre-fix, the param wasn't even exposed at this layer; current callers (none in-tree today) will pass nil").

`gacha_actions.debug_add_tickets` (line 149-169) mints **two distinct** keys per hotkey press: `ticketsKey` for the first AddCurrency, `limitedTicketsKey` for the second (called from inside the first's callback). This is **intentional and correct** — they're two distinct grant gestures (different currencies, different server-side scoped keys). The comment at line 145-148 acknowledges "held-hotkey re-fires still re-enter the handler and mint new keys (double-spend prevention there would need handler-scope debounce, out of scope)." A held key produces N×2 grants. That's debug-only flag behavior; flagged here only for completeness.

**Risk:** The same shape as v2-H-V2-6 — when the F3/Inventory rework lands (per memory `project_inventory_party_rework`) and UI starts to wire `ProgressionService.levelCharacter` to a button, double-tap protection relies entirely on the new screen's own debounce. The default `or uuid4()` fallback in `NetworkTCP` actively *enables* double-spend on user double-tap because every tap mints a fresh key.

**Fix (carry-forward from v2):** When the inventory/party rework starts wiring these calls, the UI MUST mint the idempotency key on `mousepressed` (not on `mousereleased`, not inside the click callback) and pass it through. Add a CI lint / pre-commit check that flags any new `Network.{pull,multiPull,addCurrency,level*,ascend*}` caller missing the 4th arg. Until then, document the contract in `ProgressionService.lua` doc-comments more loudly than the existing soft-spoken "preparatory plumbing" framing.

### V3-M2. M-V2-7 staleness applies cross-handler; the doc-comment only mentions same-handler retries
**Files:** `Server/Account/src/AccountTransaction.hpp:114-126`, all `StoreIdempotency` call sites
**Focus:** question 3

The M-V2-7 fix documented the point-in-time semantic at the `StoreIdempotency` doc-block. But the cached response_payload also affects **handlers that issue a follow-up read** based on the cached values. Specifically:

- `Client/src/services/GachaService.lua:338` follows every pull RPC with `Network.getState(...)` — so the client view recovers via the second roundtrip even on cache hit. **Safe.**
- `Client/src/services/gacha_actions.lua:155-160` (`refresh`) uses ONLY the AddCurrency response. No `getState` follow-up. Cache hit on AddCurrency → cached T1 wallet → `updatePlayerState(data)` writes stale wallet to the GachaService. **Marginal** — the only AddCurrency caller in-tree today is the debug hotkey, and the debug flow doesn't intersect real-money or quest-payout grants. But the entire point of AddCurrency per the project memory is "official canonical wallet-grant path used by purchases/quests/achievements", so any future IAP flow that hits this code path will see stale balances on retry.
- `QuestHandlers::HandleClaimQuestReward` builds its response from post-mutation account state (line 614-626) including `wallet`, `login_streak`, `story_level`, `story_xp`, `difficulty_tier`. A retry from a partition-rollover scenario (V3-H1) or a connection-drop-then-redial would surface T1 values to the UI. The client trusts `wallet` and friends. **Marginal** but visible — a player who claimed quest Q at T1, got `credits=500` in the response, hit a network glitch, and the auto-retry returns the cached T1 response while their actual balance is now `credits=400` after a subsequent pull, will see a momentary "credits jumped back to 500" UI flicker until the next state refresh.

No caller in the audited scope mutates state assuming the cached response reflects current. The cached pull → `Network.getState` chain is the documented pattern. But the doc-comment at `AccountTransaction.hpp:114-126` only mentions wallet balances and doesn't enumerate the other point-in-time fields (login_streak, story_level, story_xp, difficulty_tier, pity_counter, four_star_pity, guaranteed_next, in_soft_pity, consecutive_fifty_fifty_losses). All of them carry the same staleness.

**Fix:** Either (a) extend the doc-comment to enumerate every embedded snapshot field per handler so future maintainers know the full surface, or (b) on cache HIT path, the handler patches a `_cached_at: <ts>` field into the response so the client can detect staleness and refresh-out-of-band, or (c) extend the GachaService pattern of "always follow with getState" to gacha_actions.addCurrency and to ProgressionService (when callers land).

### V3-M3. `idempotency_cache` row replacement on conflict is racy across stripe-lock-bypass paths
**Files:** `Server/Account/src/AccountTransaction.hpp:247-261`
**Focus:** question 9

Verbatim from v2 followup-idempotency M2. The H2 fix uses `ON CONFLICT DO UPDATE SET response_payload = EXCLUDED.response_payload, expires_at = EXCLUDED.expires_at, created_at = now()`. Correct for the documented use case (refresh on long-tail retry-after-expiry).

Failure mode: any future code path that bypasses the per-player stripe lock (multi-instance horizontal scale, async-flush patterns, the `VerifyCredentials` read window closed by H-V2-3 but not currently asserted in code) could allow two commits with the same scoped_key to land. Pre-H2 `DO NOTHING` would have preserved the first commit's payload. Today's `DO UPDATE` destroys the first and writes the second.

For single-process the stripe lock makes this impossible. Mark to ensure horizontal scaling doesn't ship this regression.

**Fix:** Either (a) add a startup assertion that the service is single-instance via a pg_advisory_lock at process startup (releases on process exit), OR (b) change the conflict policy to `DO UPDATE ... WHERE idempotency_cache.expires_at < now()` (refresh only if the existing row is already expired, preserve fresh rows from racers).

### V3-M4. Non-string `idempotency_key` payload surfaces as "Internal server error"
**Files:** All 8 handler sites that do `request.value("idempotency_key", "")`
**Focus:** question 9

Verbatim from v2 followup-idempotency M4. Still open. `request.value("idempotency_key", "")` throws `nlohmann::json::type_error::302` if the field exists but isn't a string. Surfaces as the catch-all "Internal server error" to the client. No state corruption (handler hasn't mutated yet — the lookup runs before `Begin`), but the error string is opaque.

A misbehaving SDK that JSON-encodes a UUID as integer or as `{key:"...", v:1}` object gets an opaque 500 instead of a clear "expected string" hint. The error is debuggable from server logs (catch-all logs `e.what()`) but the client gets a generic message.

**Fix:** Either explicit type-check (`if (request.contains("idempotency_key") && !request["idempotency_key"].is_string()) return errorResponse("Invalid idempotency_key type");`) or centralize in `IdempotencyKey::Scoped` by changing its signature to `Scoped(prefix, request)` — pull the field with explicit type, return empty on type mismatch + WARN.

---

## Low / Observation

### V3-L1. `IDEMPOTENCY_DEFAULT_TTL_SECONDS = 86400` may still be short for mobile multi-day suspend
**Files:** `Server/Account/src/AccountTransaction.hpp:107`
**Focus:** question 6

M6 bumped TTL from 3600s (1h) to 86400s (24h). Mobile users who background the app for >24h (vacation, weekend, work commute over a holiday) will miss the cache and re-execute. For pulls/claims protected by other invariants (`q->IsClaimed()` for claims, wallet `CanAfford` precheck for pulls), this fails safe — but a retry-after-49h on AddCurrency for a one-time IAP grant would silently double-credit. The AddCurrency caller surface today is debug hotkey only; future IAP integration would expose the gap.

**Fix:** Either (a) accept the 24h cap and design IAP idempotency separately (server-side IAP receipts have their own dedup window), or (b) bump default to 7d (~604800s) and accept ~7× table growth pressure (mitigated by V3-H2's sweeper). 7d covers >99% of mobile-resume gaps; the long tail is bounded by IAP's own server-side receipt dedup.

### V3-L2. Per-RPC scope prefixes are disjoint; no collision possible (verified)
**Files:** all `IdempotencyKey::Scoped(...)` sites
**Focus:** question 7

Enumerated: `pull`, `multi_pull`, `admin_grant`, `claim`, `level_char`, `ascend_char`, `level_wpn`, `ascend_wpn`. All 8 are distinct strings — no two handlers produce the same scoped_key for unrelated retries even if a client reuses a UUID across actions. Cache PK `(account_id, scoped_key)` is the actual dedup boundary; the prefix discipline keeps each handler's namespace clean.

One mild observation: `admin_grant` is the prefix for the canonical wallet-grant message (AddCurrency). The "admin" framing is historical — per project memory `project_addcurrency_not_debug_only`, this is the official grant path for purchases / quests / achievements. Renaming to `wallet_grant` or `grant` would align with intent, but rotation requires a careful migration (old retries on the old prefix during deploy would miss the cache and re-execute). Defer until a coordinated client+server rollout.

### V3-L3. `H-V2-9` keys are deterministic per (questId, accountId) but daily quests reset
**Files:** `Server/Account/src/QuestHandlers.hpp:423`, `Server/Account/src/reducers/QuestClaimsReducer.hpp`
**Focus:** v1/v2 inheritance

Verbatim continuation of v2-followup-idempotency-L3. For repeatable daily quests, `claim:Q:accountId` is the same key across multiple days. Different `created_at` (partition + timestamp) means the events table writes one row per claim cycle (UNIQUE doesn't fire across partitions or even within a partition at different timestamps, since the UNIQUE includes `created_at`).

Reducer side: `QuestClaimsReducer::Apply` throws if `claimed_quest_ids.count(quest_id) > 0`. Replay from genesis on a player with day-1, day-2, day-3 claims of the same daily quest would fire on day-2 replay. Currently unwired (reducer only invoked per-event through `ReducerResult`); becomes a bug the moment a from-genesis snapshot-rebuild path lands.

**Fix:** Either bake the reset cycle into the key (`claim:Q:2026-06-03:accountId`), or have the reducer reset `claimed_quest_ids[Q]` on a `quest_reset_advanced` event (not currently modeled).

### V3-L4. `FindIdempotency` does not delete expired rows as a side effect
**Files:** `Server/Account/src/AccountRepository.hpp:289-320`
**Focus:** question 9

Confirmation, not bug. Filter `expires_at > now()` hides expired rows from lookup but doesn't delete them. The sweeper (V3-H2) needs to handle deletion separately. Worth a one-line comment at the SELECT noting that "expired rows accumulate; sweeper required".

---

## Focus-question summary table

| # | Focus | Verdict |
|---|---|---|
| 1 | H-V2-9 deterministic sibling keys | Closed in code; (a) no in-process race (stripe lock + IsClaimed check), (b) cache replay returns response without duplicate INSERT, (c) extraction via BuildClaimEvents preserves the chain (`bundle.claim.idempotency_key + ":wallet:" + cidStr`). Comment at line 444 overstates strength (V3-H1). |
| 2 | C7-C wire format | Closed; only one ServiceClient → ServiceEndpoint pair; no envelope bypass. |
| 3 | M-V2-7 point-in-time semantics | Closed in doc but the doc only mentions wallet (V3-M2). |
| 4 | H-V2-6 client threading | Closed at ProgressionService → Network layer; no UI callers yet (V3-M1). Two distinct keys per debug_add_tickets press is intentional. |
| 5 | AppendIdempotent partition | Production path uses `AppendInTx` not `AppendIdempotent` — moot. Test-only path is correct (cross-partition probe via no-created_at predicate). |
| 6 | Cache TTL vs mobile suspend | 24h covers 99% of mobile-resume gaps; long tail bounded by IAP's own dedup (V3-L1). |
| 7 | scopedKey disjointness | 8 distinct prefixes; no two handlers share a namespace (V3-L2). |
| 8 | Cache-write race window | Stripe lock serializes; cache write atomic with commit; second retry sees Hit even across the C1 stale-reload boundary because the cache row landed with the commit. |
| 9 | Anything missed | V3-H2 sweeper still missing; V3-M3 horizontal-scale `DO UPDATE` regression vector; V3-M4 non-string payload → opaque 500. |

---

## Suggested triage

**Pre-launch blockers:**
1. V3-H2 idempotency_cache sweeper (5-min tick, ~10 LoC).
2. V3-M4 type-check on `idempotency_key` payload (one helper, 8 mechanical replacements).

**Before next major refactor wave:**
3. V3-M1 lint rule for `Network.*` mutating calls missing the 4th arg (catches the inventory/party rework regression).
4. V3-M2 extend M-V2-7 doc-comment to enumerate every point-in-time field per handler.

**Defer:**
5. V3-H1 documentation pass (the actual fix would require a global UNIQUE index, which costs nothing today but isn't worth shipping until a from-genesis replay lands).
6. V3-M3 horizontal-scale guard (single-process today).
7. V3-L1/L2/L3/L4 — cosmetic / future-work.

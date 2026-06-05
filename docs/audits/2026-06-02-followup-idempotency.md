# Follow-up Audit: Idempotency + Retry

## Summary

The server-side idempotency machinery (scoped key cap, tri-state lookup, atomic cache-row write, before-rate-limit ordering, 24h TTL, refresh-on-conflict) is broadly correct and the previous-audit fixes hold under single-process scrutiny. Two real gaps remain: **(1)** the C7 Memento snapshot is captured AFTER handlers mutate the live `Account`, so Rollback restores POST-mutation state and the only thing actually rolling speculative state back is the C1 stale-flag reload; and **(2)** the H13 client wrappers expose an `idempotencyKey` parameter that no caller in the project passes — every UI invocation falls back to `uuid4()` per network call, so the contract is unused outside of `claimQuestReward`. The 24h cache-table has no sweeper. Several long-tail retry-after-expiry paths are correct-by-luck (UI-side `isLoading` guard, quest reset clearing `IsCompleted`) rather than correct-by-design.

## Critical

*(None — every double-execution risk found is gated by another correctness mechanism that's currently sound, but see C7-like fragility under H1.)*

## High

### H1. C7 snapshot captures POST-mutation state in every event-sourced handler
**Files:** `Server/Account/src/AccountTransaction.hpp:49`, `Server/Account/src/GachaHandlers.hpp:133-184,289`, `Server/Account/src/GachaHandlers.hpp:455-540,614`, `Server/Account/src/AccountHandlers.hpp:362,396`, `Server/Account/src/QuestHandlers.hpp:348-547,572`, `Server/Account/src/ProgressionHandlers.hpp:170-178,479`
**Status:** NEW

The C7 fix in `AccountTransaction` constructor (line 49) calls `account.CaptureSnapshot()` so Rollback can restore. But every handler mutates the live `Account` BEFORE constructing the transaction:

```cpp
// HandlePull (GachaHandlers.hpp:133-289)
account.GetWallet().TrySpendForPullByType(slot->ticketType);   // wallet mutated
PullResult result = banner.Pull(account.GetRNG(), pity, ...);   // RNG / pity / guarantee mutated
account.RecordPull(bType, result.item.rarity, ...);             // stats mutated
account.GetCollection().Dispatch(...);                          // collection mutated
// ... ~150 lines of mutation ...
auto txn = m_ctx.repository->Begin(account);                    // <-- snapshot captured here
```

`AccountTransactionTest.cpp:189-219` constructs the txn FIRST and mutates SECOND — which is the only ordering that makes RestoreFrom restore the pre-action state. Every shipping handler reverses this order. Result: on Commit failure, `RestoreFrom` restores the POST-mutation state (because that's what was snapshotted), and the only thing actually undoing the speculative mutations is the C1 `MarkStaleForReload` → `GetLockedAccount` cache-evict → DB reload on the next access.

This means the C1 stale-flag mechanism is doing the entire rollback job by itself; C7's snapshot adds nothing. If C1 ever regresses (the audit explicitly called out that C1's contract is "load-bearing"), C7 will silently fail to provide its documented guarantee. The test that "covers" C7 doesn't exercise the production path.

**Fix:** Either (a) move `Begin()` to the top of each handler before any mutation (loses M2's lazy-acquire benefit), (b) expose `CaptureSnapshot()` as a separate handler-boundary step distinct from `Begin()`, or (c) document explicitly that the C7 snapshot is residual safety and the actual rollback guarantee is provided by C1's stale-reload — and add a regression test that mutates BEFORE Begin then asserts Rollback + reload semantics.

### H2. H13 client idempotency contract has zero in-project callers
**Files:** `Client/src/services/GachaService.lua:360-362`, `Client/src/services/ProgressionService.lua:121,130,139,148`, `Client/src/services/gacha_actions.lua:150,152`, `Client/src/network_tcp.lua:1205,1245,1335,1417,1425,1433,1441`
**Status:** NEW

The H13 fix added an optional trailing `idempotencyKey` parameter to seven `NetworkTCP.*` wrappers (`pull`, `multiPull`, `addCurrency`, `levelCharacter`, `ascendCharacter`, `levelWeapon`, `ascendWeapon`). No caller in the project supplies it:

```lua
-- GachaService.lua:360
Network.pull(slotId, handlePullResponse)                    -- 2 args; idempotencyKey nil

-- ProgressionService.lua:121
Network.levelCharacter(characterId, newLevel, callback)      -- 3 args; idempotencyKey nil

-- gacha_actions.lua:150
Network.addCurrency("tickets", amount, function(ok, data) ... end)  -- 3 args; idempotencyKey nil
```

The wrappers fall back to `idempotencyKey or uuid4()` (network_tcp.lua:1217 etc.), so every call generates a fresh UUID per-network-call — exactly the H13 anti-pattern the previous audit called out. The only true H13-conformant caller is `claimQuestReward` (network_tcp.lua:1864), which generates the key once outside its executeWithRetry closure.

Why the current code isn't actively corrupting:
- `GachaService:doPull` (line 317) flips `self.isLoading = true` and `canPull` (line 300) refuses while `isLoading`. Double-tap is debounced at the UI layer.
- None of these RPCs are wrapped in `executeWithRetry`, so the network layer never auto-retries.

Why this is still HIGH:
- The protection is incidental (UI-state guard). Any refactor that drops `isLoading` (e.g., the planned inventory/party rework, or a debounced rapid-fire button) silently restores double-execute on user double-tap.
- Progression actions (`levelCharacter` etc.) have no `isLoading` guard at the wrapper or service layer. A double-tap on a level-up button submits two requests with different UUIDs → both succeed if scrap permits → double-spend.
- `addCurrency` chained at gacha_actions.lua:152 is intentional (two distinct grants), but a UI bug that fires the outer button twice would mint duplicate currency.

**Fix:** Plumb a `requestId` argument from the UI down to the wrapper for every button-triggered mutating call. Generate it on `mousepressed` / `keypressed`, capture in the closure, pass through any retry. Audit ALL `Network.*` mutating call sites and either document why "fresh UUID per call" is correct or pass the captured id.

### H3. Quest-claim wallet/progression events have non-deterministic idempotency_keys
**Files:** `Server/Account/src/QuestHandlers.hpp:487,509,542`
**Status:** NEW

The prior audit (H9) noted that `claimEvent.idempotency_key = "claim:" + questId + ":" + accountId` is deterministic. But the surrounding wallet + progression events in the same handler derive theirs from a freshly generated `claimEventId`:

```cpp
// QuestHandlers.hpp:487 — deterministic
claimEvent.idempotency_key = "claim:" + questId + ":" + std::to_string(account.GetAccountId());

// QuestHandlers.hpp:509 — fresh per-call
e.idempotency_key = "claim_wallet:" + aphelyon::UuidV7::ToString(claimEventId) + ":" + cidStr;

// QuestHandlers.hpp:542 — fresh per-call
pe.idempotency_key = "claim_progression:" + aphelyon::UuidV7::ToString(claimEventId);
```

Effect on retry-after-cache-expiry: the deterministic claim-event key DOES collide with itself across two executions in the same partition (UNIQUE `(account_id, idempotency_key, created_at)` only protects same-`now()` writes), so two re-claims still slip through if they land in different partitions. But the wallet/progression events get NEW keys each time, so they ALSO slip through. End result: the deterministic key contributes nothing the version monotonicity check doesn't already provide.

Also: comment at `QuestHandlers.hpp:487` says the claim is "already deterministic" — misleading because the wallet/progression siblings in the same commit aren't.

**Fix:** Either derive all three events' idempotency_keys from `scopedKey` (when supplied) the same way the pull/multi_pull/admin_grant paths do (H9), or document explicitly that quest claim relies on the `IsClaimed()` check at handler-line:331 for cross-execution dedup and the event-level keys are audit markers only.

## Medium

### M1. idempotency_cache has no sweeper; 24h TTL bounds visibility but not table size
**Files:** `Server/Account/schema.sql:358-381` (TODO comment at line 370), `Server/Account/src/AccountTransaction.hpp:107`
**Status:** VERIFIED-OPEN (M6 in prior audit bumped TTL; cleanup deferred)

Schema comment line 370: "a cleanup daemon will sweep the table later — until launch the entries are small enough to ignore." No daemon exists. With M6's 24h TTL × per-call fresh UUID (H2 above), every committed mutation writes a unique row that lives forever. Bound is monotonic growth ≈ (active players × mutating-RPC rate) × runtime — not × 24h, because old rows never delete. Indexed `idempotency_cache_expires_idx ON expires_at` is sized to support the future sweep.

Estimate for moderate load (1k DAU, 50 idempotent ops/player/day, 24h TTL): 50k rows/day, ~300 bytes each (scoped_key ~50 + lz4-compressed response_payload ~250) → ~15 MB/day → ~5 GB/year. Eventually a real problem; not immediate.

Compounding factor: H2 above means most rows are written but never queried for retry, so the table accumulates faster than the lookup-traffic suggests.

**Fix:** Ship the sweeper before launch. Single periodic `DELETE FROM idempotency_cache WHERE expires_at < now() - interval '1 hour'` on a 5-min tick is sufficient (the index handles it). Could live on the same `OnCleanupTick` thread as `CleanupIdleAccounts` (AccountServer.hpp:130). Document the rate so ops knows when to add it.

### M2. idempotency_cache `ON CONFLICT DO UPDATE` is wrong if horizontal scaling lands without stripe-lock awareness
**Files:** `Server/Account/src/AccountTransaction.hpp:215-230`
**Status:** NEW (potential regression vector)

H2's fix replaces `DO NOTHING` with `DO UPDATE SET response_payload = EXCLUDED.response_payload, expires_at = EXCLUDED.expires_at, created_at = now()`. Correct for the documented use case (long-tail retry-after-expiry refreshing the cache row).

Failure mode if stripe locks are ever bypassed (same C6 concern from prior audit applied to idempotency): two concurrent executions for the same scoped_key both Miss the cache, both execute, both commit. With `DO UPDATE`, the second's payload overwrites the first's. Pre-H2 `DO NOTHING` would have preserved the first. Both outcomes are wrong, but `DO UPDATE` actively destroys the canonical first-result that future retries should see.

This is fine today because (a) stripe lock serializes in-process, and (b) the project is single-process. Mark explicitly so a future horizontal-scaling pass doesn't ship a regression here.

**Fix:** Either add `WHERE idempotency_cache.expires_at < now()` to the `DO UPDATE` (refresh only stale rows, preserve fresh ones from horizontal racers), OR document that the stripe-lock is the SOLE guarantor of single-execution and any cross-node deployment must add distributed locking before idempotency_cache is trustworthy.

### M3. Oversized client-key log warning is unrate-limited
**Files:** `Server/Common/src/IdempotencyKey.hpp:60-62`
**Status:** NEW

H3's cap fix emits `LOG_DATA_WARN("Rejecting oversized idempotency_key ...")` per offending request. No rate limit on the warn log itself. A hostile client running through a session token (rate limiter doesn't gate on cap rejections — `IdempotencyKey::Scoped` runs before the rate-limit check) can spam oversize keys and inflate the log file. Log destinations vary (disk, ELK, etc.) — disk-bound logs are a DoS vector.

**Fix:** Per-account rate-limit the warn (e.g., `m_oversizedKeyLimiter.Allow(playerId, ...)`), OR downgrade to `LOG_DATA_DEBUG`. The action still executes uncached, so observability of "client misbehaving" is the only value of the warn; one log per 10s per player suffices.

### M4. Non-string `idempotency_key` payload throws and surfaces as "Internal server error"
**Files:** `Server/Account/src/GachaHandlers.hpp:65,307`, `Server/Account/src/AccountHandlers.hpp:310`, `Server/Account/src/QuestHandlers.hpp:298`, `Server/Account/src/ProgressionHandlers.hpp:130,214,298,383`, `Server/Common/src/TcpServerBase.hpp:487-496`
**Status:** NEW

`request.value("idempotency_key", "")` throws `nlohmann::json::type_error::302` if the JSON field exists but isn't a string (e.g., `"idempotency_key": 12345`). `ParseJsonStrict` parses successfully because the payload IS valid JSON. The throw propagates to `TcpServerBase::ProcessMessage`'s catch-all (line 487) and surfaces as "Internal server error" to the client. No state corruption (no mutations have happened), but the client sees an opaque 500-style response instead of the obvious "expected string" hint.

**Fix:** Validate the type with `if (request.contains("idempotency_key") && !request["idempotency_key"].is_string())` and reject with "Invalid idempotency_key type", OR use `request.value("idempotency_key", std::string{""})` with explicit type which throws the same error but at a known site you can catch locally.

### M5. Cached response payload preserves stale wallet balances; client UX assumes refresh
**Files:** `Server/Account/src/GachaHandlers.hpp:265-282,594-611`, `Server/Account/src/AccountHandlers.hpp:394`, `Server/Account/src/QuestHandlers.hpp:553-566`, `Client/src/services/GachaService.lua:338`, `Client/src/services/gacha_actions.lua:143-153`
**Status:** NEW

The cached pull response embeds wallet balances + pity counters as they were at original-execution time. A retry an hour later gets those stale values back. `GachaService:doPull` follows up with `Network.getState(...)` (line 338) which refreshes — so for pulls, the client recovers. But `gacha_actions.lua` AddCurrency just calls `refresh(ok, data)` (line 145) using the cached response as authoritative; no separate getState.

For now the AddCurrency UI is admin/debug-shaped, so the staleness isn't player-facing. For future real money / IAP flows passing through the same path, the cached response could show a player "you have 500 credits" after they've spent 200 — visible UX bug until next refresh.

**Fix:** Document the contract (cached response is authoritative for the retry, NOT for the current wallet — client must refetch state separately). Or: on cache hit, the handler could re-read the live wallet and patch the cached response with current balances before returning. Adds DB roundtrip on every hit, but preserves the byte-exact "action result" while keeping client state truthful.

## Low / Observation

### L1. Idempotency check skipped for empty keys — same behavior whether client omits or cap-rejects, but logs differ
**Files:** `Server/Common/src/IdempotencyKey.hpp:56-62`
**Observation:** When `rawClientKey.empty()`, return is silent. When `rawClientKey.size() > 128`, return is the same empty string BUT a warn fires. Two semantically identical outcomes with different observability. Intentional and correct — just note for future maintainers.

### L2. Pull/MultiPull event idempotency_key fallback strings collide cross-execution per design
**Files:** `Server/Account/src/GachaHandlers.hpp:228-231,562-565`, similar in AccountHandlers / Progression / Quest
**Observation:** When no `scopedKey`, the fallback `"pull:<accountId>:<freshUuid>"` is guaranteed unique per call. Comment correctly labels this an "audit marker rather than a true dedup key." This is fine — but the comment block at GachaHandlers.hpp:221-227 conflates "we want the UNIQUE constraint to guard" with "the UNIQUE constraint actually guards" when in fact the constraint `(account_id, idempotency_key, created_at)` is `created_at`-keyed and cross-partition retries slip through regardless. Same observation applies to events.UNIQUE comment at schema.sql:286 — describes intent better than effect.

### L3. `claimEvent.idempotency_key` semantic ambiguity for repeatable quests
**Files:** `Server/Account/src/QuestHandlers.hpp:487`, `Server/Account/src/reducers/QuestClaimsReducer.hpp:25-26`
**Observation:** Daily quest Q can be claimed once per day. Each claim writes an event with the same `idempotency_key="claim:Q:accountId"`. Different days → different `created_at` → events.UNIQUE doesn't fire. But `QuestClaimsReducer::Apply` throws if `claimed_quest_ids.count(quest_id)` (line 25-26). Reducer replay of the event log for an account that completed Q on day 1, day 2, day 3 would throw on day-2 replay. Not a current bug because reducer is only invoked through `ReducerResult` infrastructure that wraps individual events — but if the reducer is ever wired to a from-genesis replay path (e.g., for cold snapshot rebuild), it'll fire. Either the reducer should reset `claimed_quest_ids` on a `quest_reset_advanced` event (not modeled), or the events for repeatable quests need cycle-encoded ids (`"claim:Q:2026-06-02:accountId"`). Defer to whoever owns the snapshot/replay path.

### L4. `FindIdempotency` SELECT does not filter `expires_at <= now()` cleanup path
**Files:** `Server/Account/src/AccountRepository.hpp:289-320`
**Observation:** The query at line 297-303 filters `expires_at > now()` — correct for hiding expired rows from lookup. But it doesn't DELETE them as a side effect, so the eventual sweeper has to do it separately. Just a confirmation, not a bug.

### L5. `idempotency_cache.created_at` set to `now()` on DO UPDATE refresh
**Files:** `Server/Account/src/AccountTransaction.hpp:223`
**Observation:** `created_at = now()` in the DO UPDATE means the column tracks "last-refreshed" rather than "first-created" after an expiry-refresh cycle. Forensic queries asking "when did this idempotency token first appear" become wrong. Probably acceptable for a transient cache, but worth a comment so audit / abuse-investigation queries don't assume `created_at` is immutable.

## Verified Closed

1. **H2 (refresh on conflict)** — `ON CONFLICT DO UPDATE` at `AccountTransaction.hpp:220-223` correctly refreshes expires_at + response_payload for the long-tail retry-after-expiry path. Verified.
2. **H3 (length cap)** — `IdempotencyKey::Scoped` at `IdempotencyKey.hpp:54-69` rejects oversized keys by returning empty; handlers all guard with `if (!scopedKey.empty())`. Verified across all 8 sites (GachaHandlers pull + multi_pull, AccountHandlers admin_grant, QuestHandlers claim, ProgressionHandlers level_char/ascend_char/level_wpn/ascend_wpn).
3. **H7 (idempotency before rate-limit)** — Pull (`GachaHandlers.hpp:80-89` before `91-96`), MultiPull (`GachaHandlers.hpp:318-327` before `329-334`), AddCurrency (`AccountHandlers.hpp:333-342` before `344-349`) all check cache first. Verified.
4. **H8 (AppendIdempotent dedup includes aggregate_kind)** — `EventStore.hpp:98-99` query includes `aggregate_kind = $2`. Verified. (Note: this only affects the standalone `AppendIdempotent` path; `AccountTransaction::Commit` uses `AppendInTx` which doesn't have an idempotency-key dedup at all — relies on the per-handler scoped key derivation.)
5. **H9 (event idempotency_key derives from scopedKey)** — Pull (`GachaHandlers.hpp:228-231,256-258`), MultiPull (`562-565,588-590`), AddCurrency (`AccountHandlers.hpp:386-389`), Progression scrap-spend (`ProgressionHandlers.hpp:504-506`) all use `scopedKey` (or `scopedKey + ":wallet"`) when supplied. Verified. (See H3 above for the quest-claim sibling-event gap that survived H9.)
6. **H13 (client wrappers accept optional idempotencyKey)** — All 7 mutating wrappers at `network_tcp.lua:1205,1245,1335,1417,1425,1433,1441` accept the optional trailing param. Verified at the wrapper level. (But see H2 above — no caller passes it; the contract is effectively dead in-project.)
7. **M6 (TTL bumped to 24h)** — `IDEMPOTENCY_DEFAULT_TTL_SECONDS = 24 * 60 * 60` at `AccountTransaction.hpp:107`, used as the default in `StoreIdempotency`. Verified.
8. **M7 (tri-state lookup)** — `IdempotencyLookup` at `IdempotencyKey.hpp:23-29`, `FindIdempotency` returns `Hit`/`Miss`/`Error` at `AccountRepository.hpp:289-320`. Handlers reject on `Error` with "Cache unavailable, please retry" — verified at all 8 sites. No resource leaks on the Error path (the lookup completes before any pool-leased work begins; the Error catch is before any handler-side mutation).
9. **C7 (snapshot/restore on Rollback)** — Snapshot field exists, `RestoreFrom` is called in `Rollback`, integration test passes. **BUT** see H1 above — the test exercises a code path no handler actually uses; in production, the snapshot captures POST-mutation state because Begin is called after handler mutations.

## Assurance — new guarantees the recent fixes provide

- **Idempotent cache write is atomic with events:** Cache row is buffered into `AccountTransaction::Commit`'s pqxx::work along with events, outbox, audit, and relational flush. A failed commit rolls all of them back together. No window where a player got charged but cache says "duplicate next time" or vice versa.
- **Tri-state Error rejection is leak-free:** `FindIdempotency` runs BEFORE `GetLockedAccount` mutates anything; on Error the handler returns immediately. No pool lease held, no work object created, no snapshot captured. The lease/work acquisition lives inside `AccountTransaction::EnsureOpen()` (M2 lazy path), which the Error rejection path never reaches.
- **Cross-RPC scoping:** `IdempotencyKey::Scoped` prefixes by action name (`pull`, `multi_pull`, `claim`, `admin_grant`, `level_char`, `ascend_char`, `level_wpn`, `ascend_wpn`). All scoped keys are disjoint across RPCs even if the client reuses a UUID. Cache PK is `(account_id, scoped_key)` so no in-table collision possible.
- **Byte-exact response on cache hit:** `responseBody = response.dump()` computed before Commit, stored verbatim in cache, returned verbatim on hit. `nlohmann::json` default uses `std::map` (alphabetical key order) so dumps are deterministic between calls.
- **Concurrent retry serialization:** Stripe lock (`StripedMutex<64>`) at `GetLockedAccount` ensures two simultaneous retries for the same player serialize. First commits + writes cache row; second sees Hit on FindIdempotency. Single-process correctness is solid.
- **Length cap honored:** `IdempotencyKey::Scoped` returns empty for `> 128 bytes`, handlers skip lookup + store but execute the action. No DoS via mega-key bloat, no silent reject. Warn fires for observability (with caveat M3).
- **Pull/MultiPull rate-limit doesn't strangle legitimate retries:** H7 ordering means a retry of an already-committed pull short-circuits via cache hit before rate budget is consumed. Without this, a network-glitch retry mid-cooldown would have been rejected with "Too many pulls", the client (which doesn't pass the original key — see H2) would generate a fresh UUID, miss the cache, and double-charge.

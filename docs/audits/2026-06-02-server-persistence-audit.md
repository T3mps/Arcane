# Aphelyon Server + Persistence Audit
**Date:** 2026-06-02
**Scope:** Server/Account, Server/Auth, Server/Common, Server/Account/schema.sql, Client/src/network_tcp.lua, Server/Account/tests
**Method:** 8 parallel general-purpose audit agents covering: event sourcing, persistence/schema, concurrency, idempotency, error handling, security, code quality, test coverage. Findings cross-referenced and triaged below.

---

## Executive Summary

The system is **architecturally sound** — selective event sourcing, dirty-driven relational flush, per-player stripe locks, idempotency cache, and denormalized accounts row are well-designed and largely correct. But two systemic issues land most of the CRITICAL findings:

1. **The stale-flag rollback contract is unimplemented.** `AccountTransaction::Rollback` sets `m_stale=true`, but **no code path reads it**. The commit-failure recovery story (cache reload from DB → discard speculative mutations) doesn't actually work. Worse: `Save()` on idle-evict will persist the divergent in-memory state, converting a transient rollback into permanent corruption.

2. **Rate limiting is globally disabled at process start** (`RateLimiter.hpp:14`: `g_rateLimitingEnabled{false}`). Every documented limit is a no-op. Combined with `HandleAddCurrency` having no debug-only enforcement and PBKDF2 iterations set to 10k (vs 100k+ minimum), an authenticated player can mint currency in unlimited bursts and offline brute-force is much faster than designed.

Everything else — including the just-shipped idempotency, denorm-sync, version-cursor, and quest-unlock fixes — is correct in code but **almost entirely untested**. The 142-assertion suite covers reducers and a thin slice of persistence; it would not catch a regression of any fix shipped this week.

---

## CRITICAL (fix before any production deployment)

### C1. Stale flag is set but never checked
**Files:** `Server/Account/src/AccountServer.hpp:322-363`, `Server/Account/src/AccountTransaction.hpp:178-184`
**Found by:** Concurrency + Error Handling agents (independent)

`AccountTransaction::Rollback` calls `MarkStaleForReload()`. The contract documented at `AccountTransaction.hpp:180-183` ("The cache's GetLockedAccount checks IsStale() and re-hydrates from DB before handing out the next reference") is **false**. Production code never reads `m_stale` — the only references are in `tests/Integration/AccountTransactionTest.cpp:84,107`.

**Effect:** After Commit() throws, the in-memory account stays speculatively mutated (wallet decremented, pity bumped, cursors advanced) while DB rolled back. The next handler reads the corrupted state. Player can spend the same tickets twice; version cursors drift; events get colliding versions or wrong before/after pairs.

**Fix:** In `GetLockedAccount`, after locating `accountPtr`, check `IsStale()`; if true, erase from `m_accounts` (under map lock) and fall through to reload. `ClearStale()` after successful reload. Critical that this happens under the stripe lock.

### C2. `Save()` persists divergent state on stale account
**Files:** `Server/Account/src/AccountRepository.hpp:131-168`, `Server/Account/src/AccountServer.hpp:121-122, 311-313`
**Found by:** Error Handling agent

Compounds C1. When `CleanupIdleAccounts` or `OnStopped` flushes the cache, a stale account whose Commit had failed earlier is written back to DB. `Save()` marks every collection dirty and runs `RelationalFlush::Flush` — the rolled-back wallet balances get re-committed over the truth. **Transient rollback → permanent corruption.**

**Fix:** In `Save()`, early-return if `account.IsStale()` (log + skip). Apply to both `CleanupIdleAccounts` and `OnStopped` call sites.

### C3. Rate limiting globally disabled at process start
**File:** `Server/Common/src/RateLimiter.hpp:14`
**Found by:** Security agent

`std::atomic<bool> g_rateLimitingEnabled{false}` — every `Allow()` and `GetCooldownRemaining()` short-circuits. The "30/min" `AddCurrency` limit, the "120/60s" pull limit, login throttling — all no-ops in Release builds unless explicitly toggled.

**Fix:** Default to `true`. Gate disablement behind a Debug-only CLI flag. Assert on startup if Release + disabled.

### C4. `HandleAddCurrency` has no enforcement against client misuse
**Files:** `Server/data/protocol.json` (AddCurrency msg id 7), `Server/Account/src/AccountHandlers.hpp:278`
**Found by:** Security agent

`AddCurrency` has `requires_auth: true` but no `debug_only` gate, and the protocol parser doesn't enforce `debug_only` anyway. Combined with C3, an authenticated player can mint up to 1,000,000 per request, unlimited.

**Fix:** Add `debug_only` enforcement to the message dispatcher and gate `AddCurrency` behind it; require server-side admin role to call it in non-dev.

### C5. PBKDF2 iterations 10x too low
**File:** `Server/Common/src/Crypto.hpp:34`
**Found by:** Security agent

`DEFAULT_ITERATIONS = 10000`. Comment acknowledges 100k as production minimum. Every existing account hashed at 10k.

**Fix:** Raise to 200k. Rehash on next successful verify (lazy migration). Audit existing records.

### C6. Event-version pre-check missing in commit hot path
**Files:** `Server/Account/src/AccountTransaction.hpp:97-113`, `Server/Account/src/db/EventStore.hpp:17-58`
**Found by:** Event Sourcing + Persistence agents (independent)

`EventStore::Append` has a `SELECT MAX(version) FOR UPDATE` pre-check (the optimistic-concurrency guard). `AccountTransaction::Commit` bypasses `EventStore::Append` entirely with an inline INSERT. The UNIQUE constraint `(account_id, aggregate_kind, version, created_at)` includes `created_at`, so two concurrent commits crossing a partition boundary can both insert `version=N+1`.

Per-player stripe locks make this practically impossible *for the same player on the same node*, but the schema-level invariant the design relies on is not enforced. If stripe locking is ever bypassed (e.g., the `VerifyCredentials` issue in H1) or the service ever scales horizontally, version-collision bugs are silent.

**Fix:** Route `AccountTransaction::Commit` through `EventStore::Append` (refactor to accept an existing `pqxx::work&`), OR replicate the MAX-version pre-check inline. OR document explicitly that stripe locks are the sole guard and add startup checks.

### C7. Rollback does not undo speculative in-memory mutations
**File:** `Server/Account/src/AccountTransaction.hpp:176-184`
**Found by:** Event Sourcing + Error Handling agents

Even if C1 is fixed, the design itself has a wart: handlers mutate the live `Account` *before* `Commit()`. If Commit throws, Rollback marks stale but **the in-flight handler is still holding the now-stale `Account&`**, can still build a successful-looking response, and the in-memory state remains corrupted until reload.

Workaround C1 covers most cases (next handler gets a fresh load), but the current handler returning a "success" payload that mentions post-mutation balances after a failed commit is wrong.

**Fix:** In handlers, throw / return error from anywhere that depends on Account state after a Commit failure. Better: refactor handlers to buffer mutations in a transaction-local struct and apply to `Account` only after `Commit()` succeeds. (Larger refactor; defer.)

---

## HIGH (real bugs or close to it)

### H1. `VerifyCredentials` mutates account state without the stripe lock
**File:** `Server/Account/src/AccountServer.hpp:162-167`
Internal RPC `VerifyCredentials` runs on `ServiceEndpoint` connection threads. It takes only the map mutex; calls `LoadAccountFromData` which runs `TickQuests::Apply` (mutating dirty + quests). If a client handler is mid-RPC for the same player, both can briefly coexist; the stripe-less mutation can race the legitimate handler.
**Fix:** Take stripe lock first, then map lock — same protocol as `GetLockedAccount`.

### H2. Expired idempotency cache row blocks future caching
**Files:** `Server/Account/src/AccountTransaction.hpp:142-154`, `schema.sql:367`
PK is `(account_id, scoped_key)` with no `expires_at`. Cache lookup filters by `expires_at > now()` so an expired row → miss → handler re-executes → `INSERT ... ON CONFLICT DO NOTHING` silently swallows the new row. **Every retry of the same key after expiry re-executes side effects.** Long-tail retries = double/triple charging.
**Fix:** `ON CONFLICT DO UPDATE SET response_payload=EXCLUDED.response_payload, expires_at=EXCLUDED.expires_at, created_at=now()`.

### H3. No length cap on client idempotency key
**Files:** `Server/Account/src/AccountTransaction.hpp:82-87` + 8 handler sites
`scopedKey` is built by string-concat of unvalidated `idempotency_key`. Column is unbounded TEXT. Hostile client can submit multi-MB keys, bloating `idempotency_cache` until account-deletion CASCADE.
**Fix:** Reject `clientKey.size() > 128` in each handler (or centralize in a `ScopedKey(prefix, raw)` helper).

### H4. Last-login lies (bumped on every flush)
**File:** `Server/Account/src/db/RelationalFlush.hpp:54-57`
`FlushAccountsRow` writes `last_login = now()` whenever `dirty.accounts_row` is set. Idle-evict triggers Save which marks accounts_row dirty → `last_login` gets set to the eviction time, not the actual login.
**Fix:** Track `last_login` separately — set only on Auth `Login` and `Register` paths; have `FlushAccountsRow` either omit it or gate on a `dirty.last_login_changed` bit.

### H5. Connection pool exhaustion can deadlock the service
**File:** `Server/Account/src/db/ConnectionPool.hpp:35-41`
`acquire()` uses `cv_.wait` with no timeout. Handler already holds stripe lock during the wait → 16 stuck queries cascades into stripe-lock starvation across all players sharing those stripes. DoS surface (slow DB → service-wide hang).
**Fix:** `wait_for` with timeout (e.g. 5s); throw on exhaustion.

### H6. Session token has no IP binding + Auth-down extends sessions indefinitely
**Files:** `Server/Auth/src/AuthServer.hpp:153-154`, `Server/Common/src/SessionCache.hpp:55-76`
`bindToIP=false` means a stolen 64-char token works from any IP for 24h. Combined with SessionCache extending TTL whenever Auth is unreachable, a network partition keeps revoked tokens alive forever.
**Fix:** Enable IP binding for production; cap Auth-down grace extension to a bounded number of cycles; force-revalidate on Auth recovery.

### H7. Rate-limit check runs before idempotency cache hit
**Files:** `GachaHandlers.hpp:57-62, 274-279`, `AccountHandlers.hpp:280-285`
A legitimate retry of a committed action that arrives during cooldown gets "Too many pulls" instead of the cached response. Client (which generates fresh UUIDs per call) then retries with a NEW UUID → no dedup → double-charge.
**Fix:** Move idempotency lookup ahead of rate-limit check. Idempotent retries shouldn't consume rate budget.

### H8. `OutboxRelay` dedup query misses aggregate_kind dimension
**File:** `Server/Account/src/db/EventStore.hpp:62-74`
`AppendIdempotent` dedup query keys on `account_id + idempotency_key` only, not `aggregate_kind`. Current handlers prefix the event idempotency_key with the action name, so collision is unlikely, but the schema-level invariant isn't enforced.
**Fix:** Include `aggregate_kind` in the dedup lookup or assert handler-side that keys are prefixed.

### H9. Reducer event-id includes generated UUID, so events.idempotency_key is dead weight on pulls
**Files:** `Server/Account/src/GachaHandlers.hpp:209, 526-527`, `QuestHandlers.hpp:472`
`pullEvent.idempotency_key = "pull:<accountId>:<freshUuid>"` — never collides because `freshUuid` is fresh per call. The unique constraint on `events.idempotency_key` therefore protects nothing on the pull path. Client retry protection lives entirely in `idempotency_cache`.
**Fix:** Either use the client-supplied scopedKey as the event's idempotency_key, or rename the column to `event_dedup_key` to avoid confusion. Worth noting also that `QuestHandlers.hpp:472` correctly uses a deterministic key (`"claim:" + questId + ":" + accountId`) and provides true event-level idempotency.

### H10. `EffectDispatcher` has a latent version-collision bug
**File:** `Server/Account/src/EffectDispatcher.hpp:111`
`ev.version = account_.Dirty().cached_wallet_version + 1` is recomputed each `GrantCurrencyEffect`. Two effects in one pass → both write `version=cached+1` → collision. The dispatcher isn't currently invoked from the live path (handlers build events directly), so this is latent — but if it's ever wired in, the bug fires.
**Fix:** Maintain a local counter inside the dispatcher, OR delete the dispatcher entirely if it's vestigial.

### H11. `last_streak_day` epoch-day conversion is timezone-dependent
**File:** `Server/Account/src/AccountRepository.hpp:279`, `Server/Account/src/db/RelationalFlush.hpp:56-57`
Write uses `to_timestamp(...)::date` (session TZ); read uses `EXTRACT(EPOCH FROM date)/86400`. Different session TZs can off-by-one around midnight.
**Fix:** Force UTC explicitly or store as INTEGER day-of-epoch.

### H12. UUID generation comment is wrong; client RNG seeding fragile
**File:** `Client/src/network_tcp.lua:20-22`
Comment claims `main.lua` seeds math.random via `love.math`. It doesn't. LÖVE's boot.lua happens to seed math.random via `os.time()`, so it works — but the contract is fragile and the comment misleads.
**Fix:** Add `math.randomseed(os.time() * 1000 + love.timer.getTime() * 1e6)` explicitly OR switch to `love.math.random(0, 255)` which is documented seeded.

### H13. UUID generated per network call (not per logical user action)
**Files:** `Client/src/network_tcp.lua` (5 RPC wrappers)
Pull/MultiPull/AddCurrency/Level/Ascend wrappers all call `uuid4()` inside the function body — each `Network.pull()` invocation = fresh UUID. User-driven re-tap = new UUID = double execution. Only `claimQuestReward` generates outside the closure (correct for executeWithRetry).
**Fix:** Either document explicitly in comments, OR add UI-level debounce, OR move UUID generation up one level (per-service, captured by closure when the user clicks).

---

## MEDIUM

- **M1.** `OnStopped` saves under map mutex without per-player stripe lock — race with in-flight internal RPCs whose endpoint join times out (`AccountServer.hpp:117-126`).
- **M2.** Empty `AccountTransaction` for non-mutating handlers (GetQuestState, ReportQuestProgress, CompleteQuest, SetParty) still acquires a connection lease — perf wart under read load.
- **M3.** Internal RPC (Account↔Auth, port 7770/7773) has no shared-secret HMAC; loopback-only is the sole trust boundary. Any local process compromise = full impersonation.
- **M4.** `VerifyQuestToken` uses raw SHA-256(secret||...) instead of HMAC-SHA256, and compares hex strings with `!=` (not constant-time). `Crypto::HMAC_SHA256` already exists. (`QuestHandlers.hpp:803`)
- **M5.** `m_questTokenSecret` generated per-process, never rotated, never persisted. Memory-dump leak exposes the HMAC key for whole uptime.
- **M6.** TTL hardcoded to 3600s in `AccountTransaction::StoreIdempotency`. No config knob. Mobile retry-after-1.5h scenario = double-charge.
- **M7.** `FindIdempotency` swallows DB errors silently. For high-cost RPCs (pull, claim), failing fast with "retry shortly" would be safer than re-executing.
- **M8.** `RateLimiter` per-key map is unbounded — DoS via IP spray inflates map until 10-min lazy sweep.
- **M9.** `Login` heartbeat path bumps idle timer on every authenticated message — idle timeout never really fires while any client traffic flows.
- **M10.** `m_loginLimiter.Reset(clientIP)` on successful login lets a brute-forcer who guesses once continue from the same IP. (`AuthServer.hpp:262`)
- **M11.** `ParseJsonSafe` returns empty Json on parse failure, masking malformed payloads as defaults — `AddCurrency` with bad payload becomes `amount=0` → grant 1.
- **M12.** `std::stoi(slotStr)` on gear-slot map key has no range check; negative → uint8_t wrap to 255. (`AccountHandlers.hpp:252`)
- **M13.** `LoadEventVersions` switch silently drops unknown aggregates — adding a 5th aggregate without updating the switch will cause silent version collisions.
- **M14.** `loadouts.name` and `preset_id > 0` columns shipped in schema but never written; future-feature dead surface in DB.
- **M15.** `gear_substats` wipe-and-rewrite renumbers `slot_idx` based on in-memory vector order — fragile if order ever shifts.
- **M16.** `QuestRewardClaimed` event payload includes `login_streak_now` but no reducer projects it; would lose history if relational row is ever removed.
- **M17.** `quest_states_active_idx WHERE state IN (1, 2)` uses magic literals tied to QuestState enum — no CHECK or comment grounds them.
- **M18.** Three different `CurrencyFromStr` implementations (WalletEvents.hpp, EffectDispatcher.hpp, AccountHandlers.hpp inlined).
- **M19.** Post-commit code at `AccountTransaction.hpp:164-173` runs outside the DB tx. If `MutableDirty()` or the switch throws after `tx_->commit()` succeeds, account marked stale but DB is committed — recoverable via C1 fix but worth a try/catch.

---

## LOW / OBSERVATIONS

- **L1.** `AccountTransaction.hpp:178` — `tx_->abort()` swallows exceptions without logging.
- **L2.** `db-snapshot.bat` uses `--no-comments` which strips column comments — annotations on the live DB silently lost.
- **L3.** Dead code: `AccountServer::ConvertAccountToData` (36 lines), `AccountRepository::TimestampToTimeT` (stub), `AccountTransaction::store_` + `pool_` members unused after init.
- **L4.** `EffectDispatcher` (~160 lines) is fully built but not invoked from any handler. Vestigial scaffolding — confirm intent.
- **L5.** `Phase 7 TODO` comment in `Account.hpp:418-423` is stale; the invariant it warns about is now enforced.
- **L6.** Stale comment in `TickQuests.hpp:120` says `FlushQuests` is "currently stubbed in RelationalFlush" — it isn't anymore.
- **L7.** `audit_log.before/after` stores literal `'null'` string for null Json values, confusing `WHERE before IS NULL` queries.
- **L8.** `OnStopped` ignores `Save()` return value — flush failures silently lost.
- **L9.** Idempotency cache lookup before rate-limit check would be safer (see H7).
- **L10.** `AccountServer.hpp` mixes 5 responsibilities (~504 lines). Worth splitting `LoadAccountFromData` into `AccountHydrator` and `m_accounts`+eviction into `AccountCache`.
- **L11.** `QuestHandlers.hpp` is 827 lines, `HandleClaimQuestReward` body is ~250. Extract event-construction block.
- **L12.** Event types / aggregate names / reason codes are stringly-typed throughout. `inline constexpr` namespace would help.
- **L13.** `HandlerContext::repository` is documented "never null" but every handler dereferences without checking. Drop the pointer for a reference, or add a startup assert.

---

## Test coverage gaps (everything shipped this week is untested)

The 142-assertion suite is reducer-heavy + thin on integration. Recent fixes have no regression coverage:

- **Denorm-sync via AppendEvent (commit `516553a`):** `AccountTransactionTest` covers story_level but not wallet/stats. A test that mutates wallet, appends an event, commits, and asserts the accounts row reflects post-event values would gate the fix.
- **Version-cursor preservation in `DirtyState::Clear()` (commit `f208b70`):** Pure unit test — set cursor, set dirty bit, Clear, assert dirty cleared + cursor preserved. Not present.
- **`LoadEventVersions` hydration:** No test seeds events then verifies `cached_*_version` is restored.
- **Idempotency end-to-end (commit `7c25dbb`):** No test references `StoreIdempotency` or `FindIdempotency`. Cache hit returning cached payload byte-for-byte, expired entry triggering fresh execution, atomic rollback — all unverified.
- **TickQuests::Apply (commit `4fbaf95`):** Zero tests. Unlock-on-load, expire/recycle/auto-complete interaction, idempotence under repeated Apply — all unverified.
- **EffectDispatcher:** Zero tests AND has a latent bug (H10). Either test it or delete it.
- **Most `RelationalFlush::Flush*` methods:** Only story_level + party_slot are exercised. FlushMaterials, FlushQuests, FlushPity, FlushLoadouts, FlushOwnedWeapons, FlushOwnedGear, FlushAccountsRow wallet/stats — all untested.
- **`AccountRepository::LoadById` over a populated account:** Round-trip test covers only the 4 scalars; no test verifies the 9 helper Load* methods (characters, weapons, gear, party, loadouts, pity, world flags, quests, materials, event-versions).
- **Two-thread concurrency:** Stripe lock ordering, two-load-race for same player, eviction-during-handler — none tested.
- **`CleanupIdleAccounts` lifecycle:** No test exercises the evict-then-reload pattern.
- **HandleSetParty / HandleAddCurrency end-to-end:** Recently migrated; not gated.
- **QuestClaimsReducer / ProgressionReducer:** No rapidcheck property tests (Wallet + Pulls have them).
- **Cross-aggregate replay determinism:** No property test interleaves wallet+pulls+claims+progression events.
- **`WalletPropertyTest.cpp:122`:** Empty inputs return without `RC_DISCARD()` — noise that passes silently.
- **Integration test isolation:** Shared Postgres, no per-test teardown — events/snapshots/outbox/audit_log accumulate across runs.

---

## Assurance — what is provably sound

- **SQL parameterization:** Every `tx.exec` across the audited surface uses `pqxx::params{}`. Zero string concatenation into SQL. (Security agent verified across all 47 sites.)
- **FK CASCADE coverage:** Every account-scoped table has `ON DELETE CASCADE` on its `account_id` reference. Verified table-by-table.
- **AccountTransaction commit ordering:** events → flush → outbox → audit → idempotency → `tx_->commit()` → `tx_.reset()` → in-memory bookkeeping. Load-bearing but correct. The post-commit `tx_.reset()` ordering means a thrown bookkeeping step doesn't accidentally trigger Rollback on a committed work.
- **Idempotency atomicity (when cache row didn't pre-exist):** Cache INSERT is in the same `pqxx::work` as events. Successful retry returns cached payload byte-for-byte; cache lookup uses Postgres `now()` so no clock-skew dependency.
- **Per-player serialization:** Stripe locks (`StripedMutex<64>`) serialize all commits for the same account — wipe-and-rewrite patterns (`gear_substats`, `quest_objectives`) safe.
- **`DirtyState::Clear()`:** Explicitly preserves the four `cached_*_version` cursors. The fix is correct in code; just untested.
- **`accounts_row` always dirty on `AppendEvent`:** Denorm cannot drift behind the log on any successful event-sourced commit.
- **CSPRNG (Crypto.hpp):** BCrypt on Windows, `/dev/urandom` on Unix. `ConstantTimeCompare` is XOR-accumulate (correct).
- **Wire framing:** length-prefix and buffer caps prevent parser memory blow-up.
- **Player-id authorization:** Every handler reads `playerId` from the session-derived value, not the payload. No IDOR via payload spoofing in any audited handler.
- **OutboxRelay correctness:** `FOR UPDATE SKIP LOCKED` + per-row commit on success = safe at-least-once delivery for any future multi-instance setup.
- **SnapshotWriter destructor drain:** worker drains before exit (tested).
- **`AccountTransaction` lifecycle states (commit / rollback / destructor / double-commit):** Tested. Move/copy disabled.
- **Pure reducers (all 4):** No time/RNG/fs/env/static-mutable. Verified by direct read.
- **Reducer invariants:** WalletReducer asserts non-negative + `current+amount == balance_after`. PullsReducer asserts pity_5/4 pre-state alignment.
- **Pull event payload integrity:** RNG state, pity, guarantee, wallet balance all captured BEFORE mutation.

---

## Suggested triage order

**Today / this week:**
1. C1 + C2 — Wire `IsStale()` check into `GetLockedAccount`; gate `Save()` on stale-check. (One commit covers both.)
2. C3 — Default `g_rateLimitingEnabled = true`; debug-flag the disable.
3. C5 — Raise PBKDF2 iterations to 200k + lazy rehash on next verify.
4. H2 — Change idempotency `ON CONFLICT DO NOTHING` to `DO UPDATE`.
5. H3 — Length-cap client idempotency keys.

**Before launch:**
6. C4 + M3 — AddCurrency debug gate + internal-RPC HMAC.
7. C6 — Either route through `EventStore::Append` or replicate the version pre-check in `Commit`.
8. C7 + H1 — Refactor handler-state semantics so rolled-back commits don't leak corrupt state; stripe-lock `VerifyCredentials`.
9. H4–H6 — Last-login source-of-truth; pool exhaustion timeout; session IP binding.
10. The H/M test coverage gaps for fixes shipped this week.

**Eventually:**
- M-class items + code quality cleanup + dead code removal.
- L-class observations + comment maintenance.

---

## Agent reports (full text available via SendMessage to agentIds in this session's history)

1. Event sourcing + atomicity — agentId `af87940817a6df121`
2. Persistence + schema — agentId `aeebe665c93791c90`
3. Concurrency + lifecycle — agentId `a3227de5b0a1cc420`
4. Idempotency end-to-end — agentId `ad1962546fc594273`
5. Error handling + rollback — agentId `a919f872a87f8d379`
6. Security — agentId `a7b87043e94b7282f`
7. Code quality + dead code — agentId `aa8ceb69a27e36ae7`
8. Test coverage gaps — agentId `a2e0f3672587e6c6e`

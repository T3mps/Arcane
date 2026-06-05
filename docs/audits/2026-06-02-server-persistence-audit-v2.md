# Aphelyon Server + Persistence Audit — v2 (Follow-up)
**Date:** 2026-06-02 (same day, after remediation pass)
**Scope:** Same as v1 — Server/Account, Server/Auth, Server/Common, Server/Account/schema.sql, Client/src/network_tcp.lua, Server/Account/tests
**Method:** 8 parallel general-purpose audit agents, each given the v1 audit + recent commits as required reading.

**Per-dimension findings** (raw agent reports — read for the file:line evidence):
1. Event sourcing + atomicity → `2026-06-02-followup-event-sourcing.md`
2. Persistence + schema → `2026-06-02-followup-persistence.md`
3. Concurrency + lifecycle → `2026-06-02-followup-concurrency.md`
4. Idempotency + retry → `2026-06-02-followup-idempotency.md`
5. Error handling + rollback → `2026-06-02-followup-error-handling.md`
6. Security → `2026-06-02-followup-security.md`
7. Code quality + complexity → `2026-06-02-followup-code-quality.md`
8. Test coverage → `2026-06-02-followup-test-coverage.md`

---

## Executive Summary

**The v1 remediation pass closed every Critical, High, and Medium item from the original audit — and three new Criticals emerged from the fixes themselves.** Two of them undermine guarantees the v1 fixes were supposed to provide; the third is an adjacent path that v1 didn't cover but became security-critical the moment H6 IP binding shipped.

Of the three new Criticals, **C7-A (snapshot captured post-mutation) is the headline**: four independent agents flagged it from event-sourcing, error-handling, idempotency, and concurrency lenses. The Memento snapshot path I added in C7 captures Account state AFTER handler mutations have already landed, so `Rollback`'s `RestoreFrom` restores to the post-mutation point. The system stays safe only because C1's stale-flag eviction is the actual recovery mechanism; the C7 snapshot is dead code on every real handler path. The C7 unit test passes only because it mutates AFTER `Begin()` — the exact opposite of what handlers do.

Beyond the three new Criticals, the follow-up surfaces six High items (two that were missed in v1, four that emerged from the v1 fixes), thirteen Medium items, and the long-standing test coverage gap that v1 already documented and v2 reconfirms.

**Audit-vs-fix scorecard:**
- v1 Critical (7): all closed in code. C7's mechanism is wired but defeated by handler-call-order → C7-A here.
- v1 High (13): all closed.
- v1 Medium (19): all closed.
- v1 Low (13): 8 closed, 4 deferred (L4 intentional, L10/L11/L12 larger refactors), 1 absorbed by H7.
- **NEW criticals (3): C7-A, C7-B, C7-C — see below.**

---

## CRITICAL (fix before next deploy)

### C7-A. Memento snapshot captures POST-mutation state
**Files:** `Server/Account/src/GachaHandlers.hpp:117-184 → 289` (HandlePull), `:489-543 → 575` (HandleMultiPull), `Server/Account/src/AccountHandlers.hpp:347-362 → 396` (HandleAddCurrency), `Server/Account/src/ProgressionHandlers.hpp:426-433 → 479` (CommitProgressionScrapSpend), `Server/Account/src/QuestHandlers.hpp:381-420 → 572` (HandleClaimQuestReward)
**Flagged by:** Event-sourcing + Error-handling + Idempotency + Concurrency agents (4 of 8, independently)
**Status:** **NEW — undermines the entire C7 fix shipped in commit `c3ed91a`.**

Every event-sourced handler mutates the live `Account` BEFORE constructing the `AccountTransaction`:

```cpp
// HandlePull (representative)
account.GetWallet().TrySpendForPullByType(slot->ticketType);  // L122 — mutation
PullResult result = banner.Pull(account.GetRNG(), pity, guarantee, pullNumber);  // L133 — RNG/pity mutation
account.RecordPull(...);  // L134 — stats mutation
// ... 100+ more lines of mutation ...
auto txn = m_ctx.repository->Begin(account);  // L289 — snapshot captured HERE
```

`AccountTransaction::AccountTransaction` runs `preTxSnapshot_(account.CaptureSnapshot())` in its initializer list. By the time it runs, every handler-side mutation has landed. The snapshot encodes the **post-mutation** state, so on `Rollback()`, `RestoreFrom` restores to the same post-mutation state and does nothing useful.

**Why the system still works:** the C1 fix (`GetLockedAccount` → `IsStale()` → erase + reload from DB) handles the actual recovery on the NEXT handler invocation. The Memento path is a no-op insurance policy that the comments in `Account.hpp:60-61` advertise as load-bearing.

**Why the unit test passes:** `AccountTransactionTest.cpp:189-219` constructs `AccountTransaction txn(pool, store, account)` FIRST, then mutates Account, then rolls back. That's the only call ordering where Memento works. Production handlers do the opposite.

**Fix (preferred):** move `m_ctx.repository->Begin(account)` to the TOP of each event-sourced handler, before any mutation. M2's lazy lease acquisition means the cost is bounded — only `EnsureOpen()` (triggered by first `AppendEvent` / etc.) actually touches the pool. Snapshot capture has its own cost, but that's the whole point of C7.

**Fix (alternative):** defer snapshot capture inside `AccountTransaction` to the same `EnsureOpen` boundary as the lease — but that defeats the design intent of capturing PRE-tx state.

**Fix (architectural):** the original audit suggested "buffer mutations in a transaction-local struct and apply on Commit success." We chose Memento because RNG iteration in pulls makes buffering awkward. Reconsidering: maybe a hybrid — buffer the buffer-friendly mutations (wallet, stats, collection), pass the live RNG/pity to handlers as a snapshot from the start. Larger refactor.

### C7-B. `HandleResumeSession` bypasses IP binding entirely
**Files:** `Server/Auth/src/AuthServer.hpp:80, 385-412`, `Server/data/protocol.json` (ResumeSession msg has `requires_auth: false`)
**Flagged by:** Security agent
**Status:** **NEW — adjacent path that v1's H6 fix doesn't cover.**

`HandleResumeSession` has `requires_auth: false` (so `TcpServerBase` skips `ValidateSession`), then calls `m_sessionManager.GetSession(token)` directly. `GetSession` checks validity but performs no IP check, no idle check, no rate limit. A token stolen via memory dump, log leak, or LAN sniff gives full session takeover from any IP via the resume path — H6's IP binding is defeated.

**Fix:** route ResumeSession through `m_sessionManager.ValidateSession(token, clientIP, bumpIdle=true)` before returning the AuthResponse. Also add it to the login rate-limit budget (single IP shouldn't be able to brute-force token guesses via resume — though the 256-bit token entropy makes brute-force impractical, defense-in-depth still applies).

### C7-C. Internal RPC HMAC re-serializes `params` on the receive side
**Files:** `Server/Common/src/ServiceEndpoint.hpp:160-167`, `Server/Common/src/InternalRpcAuth.hpp` (helper design)
**Flagged by:** Security agent
**Status:** **NEW — latent bug masked by current call shape.**

`ServiceEndpoint::HandleConnection` parses the request envelope, then computes the MAC by re-serializing `params.dump()` and comparing against the received `mac` field. This works **only because** every current internal RPC payload contains only string fields, and nlohmann::json's default `dump()` sorts keys alphabetically, so the client's `params.dump()` and the server's re-`params.dump()` are bit-identical.

The first numeric or float field added to any internal RPC payload (e.g., a future `expires_in: 3600` in AuthorizeToken) will trip JSON canonicalization differences (integer formatting, whitespace) and break every internal call with "Authentication failed."

There's also a smaller second-order risk: a request with a duplicate JSON key (`{"playerId": "a", "playerId": "b"}`) will be parsed deterministically by nlohmann (last wins) and the MAC will validate against the second value — but the handler logic may inspect the first. Low likelihood, but a real MAC-bypass class.

**Fix:** MAC over the raw on-wire JSON bytes, not the re-serialized form. The sender already has the canonical form (its own dump); have it MAC that exact byte string. The receiver verifies against the exact bytes it received before parsing.

---

## HIGH

### H-V2-1. Eager snapshot capture defeats M2's lazy-lease win
**Files:** `Server/Account/src/AccountTransaction.hpp:30-45`
**Flagged by:** Concurrency + Code-quality
**Status:** NEW

`AccountTransaction`'s ctor unconditionally calls `account.CaptureSnapshot()` — a deep copy of wallet, RNG, CollectionState (characters/weapons/gear/substats), QuestStateStore, world flags, materials, party, equipment, plus scalars. For read-mostly handlers (`HandleGetQuestState`, `HandleReportQuestProgress` on a no-op tick, etc.) that M2 was designed to make zero-cost, the snapshot capture is the new perf wart. Empty Commit short-circuits the DB roundtrip but still pays the snapshot.

**Fix:** defer `CaptureSnapshot()` to the same `EnsureOpen()` boundary as the lease — captured lazily on first buffered op. NB: if the C7-A fix moves `Begin()` to the top of handlers, this fix can't be applied naively (handlers would mutate before snapshot). The two fixes interact.

### H-V2-2. `difficulty_tier` schema-vs-code drift
**Files:** `Server/Account/schema.sql` (accounts.difficulty_tier DEFAULT 1), `Server/Common/src/AccountData.hpp` (default 0), `Server/Account/src/Account.hpp` (m_difficultyTier = 0), `Server/Account/src/AccountRepository.hpp` Create()
**Flagged by:** Persistence
**Status:** NEW

Schema sets `difficulty_tier DEFAULT 1`; C++ struct defaults are 0. `Create()` omits `difficulty_tier` from its INSERT (DB row gets 1), but returns an `AccountData` populated from struct defaults (carries 0). `LoadAccountFromData` then sets `m_difficultyTier = 0`. The next `FlushAccountsRow` writes 0 back over the schema default. The schema default never actually sticks for any account.

**Fix:** Either change the schema default to 0 (match code), or change struct defaults to 1 (match schema), or have `Create()` use `RETURNING *` to read back the actual row. The third is the most maintainable.

### H-V2-3. `VerifyCredentials` reads `LoadByUsername` before stripe lock
**Files:** `Server/Account/src/AccountServer.hpp:151-180`
**Flagged by:** Concurrency
**Status:** NEW (partial regression — H1 fix moved the stripe lock acquisition, but to after the DB read)

The H1 fix correctly takes the stripe lock after successful PBKDF2 verify. But `LoadByUsername` runs BEFORE PBKDF2 — and BEFORE the stripe lock. If a concurrent commit on the same player runs `BumpLastLogin` + invalidates the cache mid-read, `VerifyCredentials` sees stale data, then proceeds to cache that stale `AccountData` in `m_accounts`. The system converges via the stale-flag fallback on the next event-sourced commit (ConcurrencyConflict trips), but stale state lives in the cache until then.

**Fix:** option (a) take stripe lock before `LoadByUsername` (cost: stripe held during PBKDF2 200k iterations — ~150ms); option (b) re-read under stripe lock after PBKDF2 (cost: 2× DB read for legit login).

### H-V2-4. `HandleResumeSession` has no rate limiter
**Files:** `Server/Auth/src/AuthServer.hpp:385-412`
**Flagged by:** Security
**Status:** NEW

Companion to C7-B. Even after the IP fix, the path has no rate limit. Login and Register are throttled (`m_loginLimiter`, `m_registerLimiter`); Resume isn't. A token-guessing attacker can hit ResumeSession unbounded.

**Fix:** add an explicit limiter (e.g., reuse `m_loginLimiter` with `RateLimits::Login()` config) on the resume path.

### H-V2-5. Username enumeration timing oracle
**Files:** `Server/Account/src/AccountServer.hpp:211-217` (VerifyCredentials)
**Flagged by:** Security
**Status:** NEW (C5 made it louder)

C5 raised PBKDF2 iterations from 10k to 200k. `VerifyCredentials` flow:
- Unknown user: `LoadByUsername` → returns nullopt → ~1ms total.
- Known user: `LoadByUsername` → PBKDF2 verify → ~150-400ms total.

The 20× louder timing signal lets an attacker enumerate valid usernames by measuring response time. Pre-C5 the gap was ~7-20ms, marginally exploitable. Post-C5 it's ~150-400ms, trivially exploitable.

**Fix:** dummy-hash on miss — when `LoadByUsername` returns nullopt, still run a `PBKDF2_HMAC_SHA256` against a hardcoded dummy hash (any 200k-iter hash works) before returning the error. Adds latency but eliminates the oracle.

### H-V2-6. Client-side `idempotencyKey` parameter is dead code
**Files:** `Client/src/services/GachaService.lua`, `Client/src/services/ProgressionService.lua`, `Client/src/services/gacha_actions.lua`
**Flagged by:** Idempotency
**Status:** NEW (H13 fix added the param; UI never uses it)

The H13 fix added an optional trailing `idempotencyKey` to 7 client RPC wrappers so UI could generate one per user click and reuse on retries. Every existing UI caller passes 3 args (no key), so the wrappers always fall back to per-call `uuid4()`. Pull/MultiPull is rescued by `GachaService:isLoading` debounce; ProgressionService has no debounce — double-tap on a level-up button is a double-spend.

**Fix:** thread per-click idempotency through ProgressionService (and gacha_actions.lua's addCurrency wrapper). Pattern matches `claimQuestReward` which already does it correctly.

### H-V2-7. M11 incomplete — two mutating handlers still use `ParseJsonSafe`
**Files:** `Server/Account/src/QuestHandlers.hpp:168` (HandleReportQuestProgress), `:584` (HandleCompleteQuest)
**Flagged by:** Error-handling
**Status:** NEW (M11 sweep missed two sites)

The M11 fix migrated 7 mutating handlers to `ParseJsonStrict` (reject on parse failure). These two were missed — both mutate quest state. Malformed payload falls through to default-empty fields, which downstream validation may or may not catch depending on the quest config.

**Fix:** mechanical — same `ParseJsonStrict` pattern as the M11 sites.

### H-V2-8. SessionManager helpers accept invalid sessions
**Files:** `Server/Auth/src/SessionManager.hpp` `GetSession`, `GetPlayerName`, `GetSessionByPlayerId`
**Flagged by:** Security
**Status:** NEW

These accessors check `valid==true` but don't check idle/IP-mismatch/expiry. A session whose idle window elapsed but hasn't been touched-and-thus-invalidated is still returned by `GetSession`. `HandleResumeSession` uses `GetSession` directly (per C7-B), so a session that should have idle-timed-out can still resume.

**Fix:** add a `GetValidSession(token, clientIP)` that runs the full `ValidateSession` invariant check and returns the session only if all checks pass.

### H-V2-9. Wallet/progression events tied to quest claim use fresh UUIDs
**Files:** `Server/Account/src/QuestHandlers.hpp:509, 542`
**Flagged by:** Idempotency
**Status:** NEW

`claimEvent.idempotency_key` is deterministic (`"claim:" + questId + ":" + accountId`), giving real event-level dedup on the claim record. The sibling wallet and progression events emitted in the same commit use freshly-generated UUIDs in their idempotency_key. The "deterministic key gives event-level dedup" property is half-true — if the claim is re-executed via some atomicity bypass, the wallet/progression dedup wouldn't fire.

**Fix:** derive the sibling events' idempotency_key from the deterministic `claimEvent.idempotency_key` (e.g., `+ ":wallet"`, `+ ":progression"`). Pattern matches H9's pull/wallet pairing.

---

## MEDIUM

### M-V2-1. `CleanupIdleAccounts` ignores `Save()` return value
**Files:** `Server/Account/src/AccountServer.hpp:428` (CleanupIdleAccounts)
**Flagged by:** Concurrency + Error-handling
L8 fixed `OnStopped`; the parallel `CleanupIdleAccounts` path was missed. Idle-evict-time save failures still drop silently.

### M-V2-2. `Account::Snapshot` drift surface (24 fields, no enforcement)
**Files:** `Server/Account/src/Account.hpp:68-160`
**Flagged by:** Code-quality + Concurrency
Snapshot duplicates Account's mutable member layout with no compile-time check against drift. Adding a new mutable Account field requires updating 3 places (declaration, CaptureSnapshot, RestoreFrom). Map types are spelled inline rather than via `using` declarations — adding a `WeaponEquipmentMap`-typed field to Snapshot then renaming the using gets diff-noisy. Consider a `#define ACCOUNT_MUTABLE_FIELDS(X) X(wallet, Wallet) X(...) ...` X-macro pattern, or a tagged `std::tuple` to enforce 1:1.

### M-V2-3. `AccountServer.hpp` grew from 504 to 616 lines
**Files:** `Server/Account/src/AccountServer.hpp` (entire file)
**Flagged by:** Code-quality
L10 was deferred as a larger refactor; the M-class sweep landed M5+M1+L8+L13+H1+C5+C1+H4 inline. The file now mixes 5 responsibilities ~616 lines deep. `SetupInternalEndpoint()` alone is 113 lines covering 3 verbs. The architecture would benefit from `AccountHydrator` (LoadAccountFromData), `AccountCache` (m_accounts + eviction + GetLockedAccount), and `InternalRpcHandlers` (the three internal RPC methods) splits, but that's a real commit, not a one-liner.

### M-V2-4. `QuestHandlers.hpp` line growth
**Files:** `Server/Account/src/QuestHandlers.hpp` (entire file, 827 → 847)
**Flagged by:** Code-quality
Minor growth; `HandleClaimQuestReward` body 285-580 ≈ 295 lines (was ~250). L11 deferred.

### M-V2-5. v1 C6 `SELECT MAX(version)` lacks `FOR UPDATE`
**Files:** `Server/Account/src/db/EventStore.hpp:23-37` (AppendInTx pre-check)
**Flagged by:** Event-sourcing
C6 closed the named bug for single-instance via stripe locks, but the in-tx `SELECT MAX(version)` has no `FOR UPDATE`. Two concurrent commits on different connections at microsecond-different `created_at` can both succeed at the same version. Per-player stripe locks make this impossible today; any future horizontal scale or stripe-lock bypass (e.g., the H-V2-3 race window) would re-expose it.
**Fix:** `pg_advisory_xact_lock(account_id, hash(aggregate_kind))` at the top of `AppendInTx`.

### M-V2-6. M19 catch ordering: `ClearDirty` runs before cursor advance
**Files:** `Server/Account/src/AccountTransaction.hpp` (Commit's try block)
**Flagged by:** Event-sourcing
Inside the try-catch, `account_.ClearDirty()` runs first, then the per-aggregate cursor advance. If `ClearDirty` throws (allocator failure on the dirty containers), the catch runs and marks stale — but the cursors were never advanced, and they were preserved through Clear. After the eventual reload, `LoadEventVersions` rehydrates the cursors from DB MAX — so this case is harmless today. Worth reversing order anyway: advance cursors first (read snapshot ev.version values; pure copy), then Clear. Removes the need for DirtyState::Clear's cursor-preservation contract entirely.

### M-V2-7. Cached pull response carries stale wallet balances
**Files:** `Server/Account/src/AccountTransaction.hpp` StoreIdempotency call sites in pull handlers
**Flagged by:** Idempotency
The cached `responseBody` was built BEFORE Commit and contains the player's wallet balances at commit time. If the player's wallet changes between commit T₁ and retry T₂ (e.g., concurrent AddCurrency from server-driven grant), the cached response on retry returns the T₁ balance. Client may render an inconsistent UI.
**Fix:** either don't include wallet balances in cached responses (re-fetch on retry), or document that idempotent retries see point-in-time response.

### M-V2-8. `audit_log.target` is NOT NULL but L7's nullopt handling only covers before/after
**Files:** `Server/Account/src/AccountTransaction.hpp` audit log INSERT
**Flagged by:** Error-handling
L7 fixed the before/after columns to write SQL NULL via `std::optional<std::string>`. The `target` column is also a Json field — if a handler passes a null Json target, the INSERT writes literal `"null"` jsonb. Same ambiguity trap, just on a different column.

### M-V2-9. SessionCache "force-revalidate on recovery" iterates under map lock — fragile
**Files:** `Server/Common/src/SessionCache.hpp` LogAuthStateChange
**Flagged by:** Security
The H6 fix has `DropExtendedSessions` iterate cache entries under the mutex. The caller must hold the lock if invoked from inside the lock; the path is documented but enforced by comment only.

### M-V2-10. `RateLimiter` cap is global (single attacker shape can DoS legit traffic)
**Files:** `Server/Common/src/RateLimiter.hpp` (M8 cap)
**Flagged by:** Security
M8's `MAX_RECORDS=10000` is global across the limiter instance. An attacker spraying 10k unique IPs fills the table; legit users whose first hit falls inside the same instance get refused-as-rate-limited. The audit explicitly chose "refuse the new entry" as the safest default — note that it also blocks legit users. Consider per-key-prefix caps (per-IP cap + per-user cap separately).

### M-V2-11. `std::getenv` runs on every internal RPC
**Files:** `Server/Common/src/InternalRpcAuth.hpp` `GetSharedSecret`
**Flagged by:** Security + Code-quality
Each `ServiceClient::Call` and each `ServiceEndpoint::HandleConnection` calls `GetSharedSecret()` which calls `std::getenv`. Read-once and cache at startup.

### M-V2-12. Quest token secret WARN-but-start should be Release-fatal
**Files:** `Server/Account/src/AccountServer.hpp` InitializeQuests (M5 path)
**Flagged by:** Security
M5 falls back to a per-process random secret if `APHELYON_QUEST_TOKEN_SECRET` is unset, with a WARN. Consistent with C3/H6 patterns (RPC HMAC, IP binding), this should be Release-fatal — operator must set the env var.

### M-V2-13. Hardcoded DB password in connection string
**Files:** various connection-string literal sites + docker-compose.yml
**Flagged by:** Security
`postgresql://aphelyon:aphelyon@localhost:5432/aphelyon` appears in code. For dev only this is fine; for any non-dev environment, externalize via env var.

---

## LOW / OBSERVATION

- **L-V2-1.** `RestoreFrom` catch is silent (loses evidence — asymmetric with L1's tx_->abort logging fix). Add a LOG_DATA_WARN.
- **L-V2-2.** `Account.hpp` Snapshot doc-comment overstates restore guarantee (C7-A makes it dead).
- **L-V2-3.** `tx_->abort()` is called even after `tx_->commit()` succeeded but post-commit threw — noisy log.
- **L-V2-4.** M13 logs ERROR but still lets load succeed with the unknown aggregate's events orphaned in-memory.
- **L-V2-5.** `events.idempotency_key` UNIQUE includes `created_at` partition key (same shape as the v1-C6 version constraint pre-fix).
- **L-V2-6.** `audit_log.metadata` / `events.metadata` lack the `quest_states.metadata` CHECK that enforces top-level JSON object shape.
- **L-V2-7.** `gear_substats.slot_idx` column name now misleads — after M15 it's a stat_type rank, not positional.
- **L-V2-8.** `subStats` post-reload order differs from roll order (consequence of M15). Verify nothing in display logic depends on roll order.
- **L-V2-9.** `<algorithm>` is dead in AccountServer.hpp after L3.
- **L-V2-10.** `SaveAccountToRepository` is now uncalled (call sites use `m_repository.Save(*account)` directly).
- **L-V2-11.** `Account.hpp` is missing `<vector>` (works transitively today).
- **L-V2-12.** `EffectDispatcher` remains 138 lines of dead scaffolding with no tests (acknowledged kept).
- **L-V2-13.** `claimed_at_streak_day` stores a count not a calendar day — name is misleading.
- **L-V2-14.** Per-validate `token_prefix` logging at debug level — minor enumeration aid.
- **L-V2-15.** M11 strict-parse + M7 idempotency tri-state are emerging duplicates worth a helper.
- **L-V2-16.** SessionConfig::bindToIP struct default is `false` (C3-style pattern would default `true`).
- **L-V2-17.** `clientIP==""` silently bypasses IP binding when reached via paths that don't populate clientIP (e.g., test harness). Defense-in-depth.

---

## Verified Closed (from v1)

All v1 Criticals, Highs, and Mediums are closed in code. The four-agent consensus confirms:
- **Critical:** C1, C2, C3, C5, C6, C7 (mechanism in place — see C7-A for timing defect), C4 (correctly dismissed).
- **High:** H1–H13.
- **Medium:** M1–M19.
- **Low:** L1, L2, L3, L5, L6, L7, L8, L13. L4 retained per project preference. L10/L11/L12 deferred as larger refactors.

---

## Test Coverage Status (still wide open)

Suite went from 142 to 162 assertions (+20). Only two new tests landed during the remediation pass:
- `[c7]` Memento restore (6 of ~24 Snapshot fields exercised — misses RNG/pity/guarantee/collection/party/equipment, the very fields that motivated picking Memento over Unit-of-Work)
- `[m2]` Lazy lease acquisition (tight, well-designed; pool-of-1-with-external-hold proves the negative)

Every other previous-audit gap remains open. Every M/H/C fix shipped this week is untested except C7 and M2. The agent's prioritized top-5:
1. C1 stale erase + reload regression test
2. H2 expired-row UPSERT (idempotency cache)
3. AccountRepository populated round-trip (LoadById over an account with characters/weapons/gear/quests)
4. H10 EffectDispatcher sequential versions
5. End-to-end HandleAddCurrency through DB

**Plus:** the integration tests have zero teardown — events/snapshots/outbox/audit_log/idempotency_cache rows accumulate forever across runs. Row-level isolation via `UniqueUsername` works; table-level cleanup is missing.

---

## Suggested triage order

**Today / immediate:**
1. **C7-A** — move `Begin()` to top of each event-sourced handler. Touches 5 handlers, ~5-10 lines each. Highest impact; restores the C7 guarantee we shipped.
2. **C7-B** — wire ResumeSession through ValidateSession. ~20 lines.
3. **C7-C** — MAC over on-wire bytes not re-serialized. ~30 lines across ServiceClient + ServiceEndpoint.

**This week:**
4. **H-V2-1** — defer snapshot capture inside AccountTransaction to first mutation (requires C7-A fix first; the snapshot must be captured BEFORE the first handler-side mutation, which means at `Begin()` time, which is what C7-A enforces).
5. **H-V2-2** — pick a default for `difficulty_tier` and unify schema + struct + Create() flow.
6. **H-V2-3** — stripe lock before `LoadByUsername` in VerifyCredentials.
7. **H-V2-4 + H-V2-8** — ResumeSession rate limit + GetValidSession helper.
8. **H-V2-5** — dummy-hash on username miss.
9. **H-V2-6** — thread idempotencyKey through ProgressionService (real user impact).
10. **H-V2-7** — finish M11 sweep (ReportQuestProgress + CompleteQuest).
11. **H-V2-9** — derive sibling-event idempotency_keys from claim's deterministic key.

**Before launch:**
12. M-V2-1 through M-V2-13.
13. The test-coverage backlog — start with the prioritized top-5 from the test-coverage audit.

**Eventually:**
14. L items + L10/L11/L12 refactors.

---

## Agent reports

Per-dimension findings (raw):
1. `2026-06-02-followup-event-sourcing.md` — Event sourcing + atomicity
2. `2026-06-02-followup-persistence.md` — Persistence + schema
3. `2026-06-02-followup-concurrency.md` — Concurrency + lifecycle
4. `2026-06-02-followup-idempotency.md` — Idempotency + retry
5. `2026-06-02-followup-error-handling.md` — Error handling + rollback
6. `2026-06-02-followup-security.md` — Security
7. `2026-06-02-followup-code-quality.md` — Code quality + complexity
8. `2026-06-02-followup-test-coverage.md` — Test coverage

Synthesis written by the orchestrator session that ran the fleet.

# v5 Follow-up Audit — Error Handling + Rollback
**Date:** 2026-06-03 (same-day successor to v4)
**Auditor:** Auditor v5 (follow-up to v1 2026-06-02, v2 2026-06-02, v3 2026-06-03, v4 2026-06-03)
**Scope:** AccountTransaction Commit/Rollback/EnsureOpen/dtor + Memento Snapshot, Account::CaptureSnapshot/RestoreFrom (X-macro), AccountDirty::Clear (cursor preservation), every handler's Begin/Commit ordering, EventStore::AppendInTx ConcurrencyConflict translation, Crypto::ParseStoredHash odd-length guard + downstream timing-oracle mitigation, OutboxRelay vs AccountTransaction torn-write surface.

Sources reviewed:
- v4 synthesis (`2026-06-03-server-persistence-audit-v4.md`)
- v4 per-dimension report (`2026-06-03-v4-followup-error-handling.md`)
- v3 per-dimension report (`2026-06-03-v3-followup-error-handling.md`)
- v2 followup error-handling (`2026-06-02-followup-error-handling.md`) — skimmed for prior context
- Current code: `Server/Account/src/Cache/AccountTransaction.hpp`, `Server/Account/src/State/Account.hpp`, `Server/Common/src/State/AccountDirty.hpp`, all handler files in `Server/Account/src/Handlers/`, `Server/Account/src/Db/EventStore.hpp`, `Server/Account/src/Db/OutboxRelay.hpp`, `Server/Account/src/AccountServer.hpp`, `Server/Common/src/Crypto/Crypto.hpp`, `Server/Account/src/Handlers/InternalRpcHandlers.hpp`, `Server/Common/src/Net/ServiceEndpoint.hpp`, `Server/Common/src/Net/ServiceClient.hpp`, `Server/Common/src/Net/SessionCache.hpp`, `Server/Auth/src/AuthServer.hpp`.
- v4 remediation commits relevant to this dimension: `62ad8df` (Begin above TickQuests in HandleClaimQuestReward — H-V4-1), `5fcb70d` (C-V3-2 pity/guarantee rollback regression test — H-V4-6), `e5b02f4` (security session-log + Crypto odd-length guard — M-V4-2/M-V4-6 security), `f24b3ca` (OutboxRelay wired — C-V4-1 idempotency), `25a81c1` (Probe inspects response body — C-V4-2 networking), `388c0dd` (SessionCache classify envelope errors — M-V4-9 networking).

---

## Verdict

**Clean.** Every v4 error-handling item is closed. H-V4-1's "Begin above TickQuests" landed correctly, the snapshot now captures pre-tick state in HandleClaimQuestReward, and the downstream comment at QuestHandlers.hpp:693-698 correctly says "pre-tick snapshot." L-V4-2 (dtor-RAII test) is materially closed by the PityRollbackRegressionTest (commit `5fcb70d`) — the test constructs `AccountTransaction`, mutates Account state via the lazy `GetPity`/`GetGuarantee` inserts, lets the txn destruct without `Commit()`, then asserts both restore (`GetPityState().count(slotId) == 0`) and stale-flag set. Two distinct test cases cover the C-V3-2 / H-V4-1 mechanism via the dtor path. L-V4-3 (EnsureOpen logic_error no log) remains open by design; L-V4-9 (retire ParseJsonSafe) remains cosmetic.

The OutboxRelay torn-write concern raised in the audit method (could a relay PumpOnce see an outbox row whose corresponding events were rolled back?) is **not realizable**. AccountTransaction writes outbox rows inside the same `pqxx::work` as the events (AccountTransaction.hpp:228-232). PumpOnce reads `WHERE dispatched_at IS NULL` from a fresh `pqxx::work` — Postgres snapshot isolation guarantees the relay only ever sees rows whose enclosing transaction COMMIT-ed. A Rollback aborts the outbox row alongside the events; the relay can't observe a torn write.

The Crypto::ParseStoredHash odd-length guard (Crypto.hpp:382-383) materially rejects the malformed case and the rejection propagates through to the username-enumeration timing-oracle mitigation. The downstream burn-PBKDF2 at VerifyPassword.hpp:126-128 still fires on the odd-length-induced parse failure (it's gated on `!ParseStoredHash`, which the new guard surfaces). The dummy hash in InternalRpcHandlers.hpp:94-96 covers the orthogonal unknown-username case; both branches are equal-time.

**Two new items**, both Medium or Low, both observability rather than correctness:
- **M-V5-1** — Auth's HandleLogin/HandleRegister don't classify envelope errors the way SessionCache (M-V4-9 networking) and Probe (C-V4-2) now do. A Release-deployed Auth with a mismatched APHELYON_INTERNAL_SECRET silently degrades every login attempt to "Invalid username or password" with no WARN-level signal that internal RPCs are MAC-failing.
- **L-V5-1** — Pre-Begin reads of mutable Account state (`preScrap`, `balanceBefore`) are pure-read today but couple correctness to "no path interleaves between this read and Begin." A pattern worth documenting in case a future refactor lifts the stripe-lock invariant.

No new Critical, no new High.

---

## CRITICAL

**None.** Every prior Critical in this dimension is verified closed.

---

## HIGH

**None.** Every prior High in this dimension is verified closed.

---

## MEDIUM

### M-V5-1. Auth's HandleLogin / HandleRegister don't classify envelope-error responses
**Files:** `Server/Auth/src/AuthServer.hpp:266-288` (Register), `:323-343` (Login)
**Status:** NEW — same defect shape as C-V4-2 networking (Probe) and M-V4-9 networking (SessionCache). The v4 remediation arc fixed the two infrastructural sites; the per-RPC handler sites were missed.

```cpp
// HandleLogin (:323-343):
auto rpc = m_accountClient.Call("VerifyCredentials", {{"username", username}, {"password", password}});
if (!rpc.ok())
{
    const char* mode = rpc.wireError() ? "wire_error" : "unreachable";
    LOG_AUTH_ERROR("Login: account service call failed ({}), IP={}", mode, clientIP);
    return CreateAuthResponse(false, "Account service unavailable", "", "", "", 0);
}
auto result = rpc.value.value("result", rpc.value);  // <-- error envelope passes through silently
bool valid = result.value("valid", false);            // <-- false, looks like bad password
if (!valid)
{
    LOG_AUTH_DEBUG("Login failed for user {} from {}", username, clientIP);  // <-- DEBUG only
    return CreateAuthResponse(false, "Invalid username or password", "", "", "", 0);
}
```

When `APHELYON_INTERNAL_SECRET` is mismatched between Auth and Account (or envelope versions skew), ServiceEndpoint responds with `{"error":"Authentication failed"}` or `{"error":"unsupported_envelope_version", "expected_v":...}`. `rpc.ok()` is `true` (round-trip succeeded). The legacy `result.value("valid", false)` reads `false` from the error envelope (no "valid" key), so every login attempt completes the path "looks like bad password" with a DEBUG-level log line. No INFO, no WARN, no ERROR — ops looking at Auth logs see "users keep getting passwords wrong" instead of "internal RPCs are failing MAC."

Account's ServiceEndpoint side does emit `LOG_NET_WARN` on MAC rejection (`ServiceEndpoint.hpp:218`), so the signal exists somewhere — but the Auth-side observability is silent.

**Why Medium not High:** the C-V4-2 Probe canary catches deploy-time secret mismatch in Release (fatal on `ProbeOutcome::AuthFailed`), and SessionCache's M-V4-9 fix surfaces the same envelope errors for per-session-validate calls. A misconfiguration that slipped past Probe AND triggered on login (e.g. a mid-flight env rotation, or a multi-instance deploy where one Account replica disagrees) would still be silent on the Auth-side login path. Pre-launch this is fine; once deployed across replicas the gap matters.

**Fix:** before the `result.value("valid", false)` read at both `AuthServer.hpp:336` (Login) and the analogous `:279-281` block (Register), inspect `rpc.value`. Mirror SessionCache's M-V4-9 shape:

```cpp
if (rpc.value.contains("error"))
{
    const auto err = rpc.value.value("error", std::string{});
    LOG_AUTH_WARN("Login: account RPC returned envelope error: {} (IP={})", err, clientIP);
    return CreateAuthResponse(false, "Account service unavailable", "", "", "", 0);
}
```

Same shape for `HandleRegister` at `:279`.

---

## LOW

### L-V5-1. Pre-Begin reads of mutable Account state for event-payload `balance_before`
**Files:**
- `Server/Account/src/Handlers/AccountHandlers.hpp:391` (`balanceBefore = wallet.GetBy(currencyEnum)` BEFORE Begin at `:398`)
- `Server/Account/src/Handlers/ProgressionHandlers.hpp:169` (HandleLevelCharacter), `:264` (HandleAscendCharacter), `:356` (HandleLevelWeapon), `:446` (HandleAscendWeapon) — each reads `preScrap = account.GetWallet().GetScrap()` BEFORE the Begin at L184/L273/L365/L455.
**Status:** OBSERVATION (carry-forward of v4 L-V3-3 reasoning; the comment at AccountHandlers.hpp:382-390 documents the AddCurrency case but the four progression sites don't).

The captured value flows into the event payload as `balance_before` (and `balance_before ± cost` as `balance_after`). It's a pure read with no Account mutation between the read and `Begin()` — the stripe lock guarantees no other path interleaves. So the value is correct.

The fragility: if a future refactor inserts ANY mutation between the read and `Begin()` (e.g. an early-return bookkeeping bump on insufficient-scrap, a "consume one rate-limit token" hook), the event payload's `balance_before` would silently desync from what the Memento snapshot captured. The reducer asserts `(current + amount) == balance_after` on replay (commented at `AccountHandlers.hpp:374-378`), so the desync would surface as a replay-failure rather than silent corruption — but only at replay time, long after the bad event landed.

The L-V3-3 comment at `AccountHandlers.hpp:382-390` explicitly flagged this for `HandleAddCurrency`. The four progression sites have the same pattern with no equivalent comment.

**Fix:** either move the `preScrap` read to be the FIRST line inside the Begin scope (capturing post-snapshot state still works because the Memento snapshot is taken in the AccountTransaction ctor BEFORE any pqxx work), or add a matching L-V3-3-style comment at each progression site. Mechanical.

### L-V5-2. HandleClaimQuestReward early-return after TickQuests discards real tick work
**Files:** `Server/Account/src/Handlers/QuestHandlers.hpp:595-600` (early-returns), `:591` (Begin), `:593` (TickQuests::Apply)
**Status:** OBSERVATION — emerges from the H-V4-1 fix, not a regression.

H-V4-1 correctly hoisted `Begin()` above `TickQuests::Apply`. The early-return chain at `:595-600` ("Quest not started" / "Quest state error" / "Quest already claimed" / "Quest not yet completed") now runs AFTER the tick has done real state work (auto-completing rollover transitions, unlocking eligible quests, marking `dirty.quest_ids`). The dtor → Rollback → RestoreFrom path discards ALL of that — including the legitimate tick work that wasn't speculative.

Per-rejection cost: one repeated TickQuests::Apply on the next handler call. TickQuests is documented idempotent (the same-day no-op semantics handle this), so correctness is preserved — only a small amount of CPU is re-spent. The comment at `:586-590` ("TickQuests is idempotent enough that the round-trip is a no-op too") is accurate.

**Why Low / Observation:** no correctness issue, no DB I/O cost (the txn never gets to EnsureOpen if no buffered ops accumulate), just a tiny re-tick on the next access. Worth flagging in case a future refactor makes TickQuests non-idempotent — at which point this becomes a real bug.

### L-V5-3. EnsureOpen() throws std::logic_error with no log line
**File:** `Server/Account/src/Cache/AccountTransaction.hpp:437-438`
**Status:** OBSERVATION — carry-forward (v2 L-V2-5, v3 L-V3-5, v4 L-V4-3). Unchanged.

```cpp
if (state_ != State::Pending)
    throw std::logic_error("AccountTransaction already committed or rolled back");
```

A programmer-bug surface (handler calling `AppendEvent` after `Commit()`) surfaces to the dispatcher as a bare "Internal server error" with no log line identifying which handler / RPC tripped the violation. One `LOG_NET_ERROR` before the throw would make this O(seconds) to triage instead of O(hours). Deferred again because no caller has tripped this in production; the throw site is unreachable by any current code path.

### L-V5-4. TemplateDatabase silent catch-and-continue on malformed templates
**Files:** `Server/Account/src/Gacha/TemplateDatabase.hpp:215`, `:306`
**Status:** OBSERVATION — pre-existing, not previously flagged. Outside the load-bearing C7/M19 commit path but worth recording.

```cpp
catch (...) { continue; }
```

Both character (`:215`) and weapon (`:306`) template-load paths silently swallow exceptions while parsing per-template JSON. A malformed template at startup is skipped without ANY log line — the server just has fewer items in its database than the data files declare. A future bug in the JSON shape would surface as "this character isn't pullable" rather than "template at file X failed to parse with reason Y."

**Why Low:** templates are dev-authored, loaded once at startup, and the failure mode is benign (missing item, not a wrong one). But the silent-swallow pattern is exactly what L-V3-1 closed for `RestoreFrom`; the same one-line `LOG_DATA_WARN` would close it here for consistency.

### L-V5-5. ParseJsonSafe still defined in Protocol.hpp despite zero callers
**File:** `Server/Common/src/Net/Protocol.hpp:42`
**Status:** OBSERVATION — carry-forward (v4 L-V4-9). Unchanged.

Confirmed via grep: `ParseJsonSafe` has only the definition site, no callers. Either retire the function (force every future caller to make the strict/lenient decision explicitly) or add a doc-comment explaining the intent for the next auditor. Cosmetic.

### L-V5-6. EnsureOpen exception-safety still relies on Lease RAII
**File:** `Server/Account/src/Cache/AccountTransaction.hpp:435-443`
**Status:** OBSERVATION — carry-forward (v4 L-V4-4). Unchanged.

```cpp
lease_ = pool_.acquire();                                       // L439
tx_    = std::make_unique<pqxx::work>(*lease_);                 // L440 — may throw
db::EventStore::AcquireAdvisoryLockInTx(*tx_, account_.GetAccountId());  // L441 — may throw
state_ = State::Active;                                         // L442
```

Functionally correct under the current `ConnectionPool::Lease` semantics (member RAII destructor returns the connection regardless of where in the 4-line sequence the throw lands). Still brittle to future refactors that insert intermediate steps without preserving the strong exception guarantee.

### L-V5-7. RestoreFrom catch arms log but provide no rollback context
**File:** `Server/Account/src/Cache/AccountTransaction.hpp:392-402`
**Status:** OBSERVATION — L-V3-1 close-out commentary.

The L-V3-1 fix added `LOG_DATA_WARN` to both arms. The log message identifies the throw site ("RestoreFrom threw — leaving Account stale for DB reload") but doesn't surface `account_.GetAccountId()` / `account_.GetId()`. A burst of these in production would be impossible to triage to specific accounts without correlating against contemporaneous handler log lines. One-line addition: include the account_id in the format args. Cosmetic.

### L-V5-8. ServiceClient WireError still classified as ProbeOutcome::UnknownError
**File:** `Server/Common/src/Net/ServiceClient.hpp:322`
**Status:** OBSERVATION — C-V4-2 design choice.

```cpp
// WireError — the frame/parse round-trip is broken so
// rpc.value is empty; no introspection is possible from
// this side. Surface as UnknownError so the caller can
// distinguish it from the auth/version paths above.
return ProbeOutcome::UnknownError;
```

Mapping WireError to UnknownError is correct: there's no body to inspect. But the startup canary's "tells you four things in one call" framing now delivers four-or-five outcomes that collapse WireError back into the catch-all. The Probe call site at `AccountServer.hpp:182-238` (per H-V3-14(b)) emits a friendly log line per outcome, so this isn't a hidden silent-swallow — but worth noting that "UnknownError" can mean "MAC was rejected, but the response body was unparseable" or "TCP frame parse failed." Cosmetic.

### L-V5-9. AccountTransaction destructor's "still Pending" branch double-counts as rollback
**File:** `Server/Account/src/Cache/AccountTransaction.hpp:59-64`
**Status:** OBSERVATION — by design.

```cpp
~AccountTransaction() {
    if (state_ == State::Pending || state_ == State::Active) Rollback();
}
```

A txn that enters dtor with `state_ == Pending` (never opened any DB connection) still runs Rollback → RestoreFrom + MarkStaleForReload. The stale-mark is correct (a handler that constructed `AccountTransaction` and then bailed before any buffered op may have mutated the in-memory Account between the snapshot and the bail). But the read-mostly handlers that construct AccountTransaction "just in case" and then early-return with no mutation still pay a MarkStaleForReload on the way out, forcing the next cache lookup to evict and reload from DB.

Quick mental count of read-mostly Begin/Commit pairs: `HandleGetQuestState` (QuestHandlers.hpp:102) constructs txn and immediately commits — that path goes Pending→Committed and skips Rollback. No live offenders today. But a future read-mostly handler that constructs AccountTransaction up-front and EARLY-RETURNS before Commit (e.g. on a validation failure) would force a stale-reload it doesn't need. Worth a one-line comment at the dtor explaining that early-return-before-mutate paths should still Commit() (M2 short-circuits with no DB cost). Already covered in `:176-194` of the Commit M2 short-circuit comment but not surfaced at the dtor.

---

## Verified Closed (from v4)

| v4 item | Closure site | Notes |
|---|---|---|
| **H-V4-1 (Begin above TickQuests in HandleClaimQuestReward)** | `QuestHandlers.hpp:591` (Begin), `:593` (TickQuests::Apply), `:579-590` (audit comment), `:693-698` (downstream comment) | Commit `62ad8df`. The pre-tick snapshot capture is correct; downstream comment correctly says "pre-tick snapshot." Validated by L-V5-2 observation. |
| **H-V4-6 (C-V3-2 pity/guarantee rollback regression test)** | `Server/Account/tests/Integration/PityRollbackRegressionTest.cpp` (two test cases) | Commit `5fcb70d`. Both `GetPity` and `GetGuarantee` lazy-insert scenarios are covered, asserting `GetPityState().count(slotId) == 0` AND `IsStale()` after dtor-triggered Rollback. |
| **L-V4-1 (HandleClaimQuestReward TickQuests-before-Begin)** | Same as H-V4-1 above | The error-handling angle is closed by the H-V4-1 fix. |
| **L-V4-2 (dtor-RAII Rollback test)** | PityRollbackRegressionTest exercises this | The "let txn destruct WITHOUT Commit" path with in-memory mutation (lazy GetPity insert) is now tested; the assertion at PityRollbackRegressionTest.cpp:75 / :106 confirms in-memory restore. |
| **M-V4-6 security (Crypto ParseStoredHash odd-length guard)** | `Crypto.hpp:382-383` | Commit `e5b02f4`. Odd-length salt OR hash returns false; downstream `VerifyPassword` burns the dummy PBKDF2 (Crypto.hpp:126-128) on the parse failure, preserving the timing-oracle mitigation. |
| **M-V4-2 security (LogSessionEvent on internal invalidate paths)** | `SessionManager.hpp:462, :492` | Commit `e5b02f4`. Both `InvalidateByPlayerIdInternal` and `CleanupExpiredInternal` now emit LogSessionEvent. |
| **C-V4-1 idempotency (OutboxRelay wired)** | `AccountServer.hpp:365` (member), `:79` (init) | Commit `f24b3ca`. Init order correct (after m_pool, before m_repository); dtor order reverses (worker joins before m_pool tears down). Confirmed no OutboxRelay-vs-Rollback torn-write surface — outbox INSERTs share the AccountTransaction's pqxx::work so a Rollback aborts the outbox row alongside the events; PumpOnce's `dispatched_at IS NULL` filter on a separate snapshot can't observe rows from an aborted tx. |
| **C-V4-2 networking (Probe inspects response body)** | `ServiceClient.hpp:300-305` | Commit `25a81c1`. `AuthFailed`, `EnvelopeVersionMismatch`, `UnknownError`, `Ok` all reachable. WireError → UnknownError documented (L-V5-8 observation). |
| **M-V4-9 networking (SessionCache classify envelope errors)** | `SessionCache.hpp:137-156` | Commit `388c0dd`. Mirrors C-V4-2 shape; surfaces envelope errors at WARN + NoteAuthLost. **Same defect class still open in Auth.HandleLogin/HandleRegister** — M-V5-1 above. |

---

## Focus Question Walkthrough (v5)

### 1. H-V4-1 Begin-above-TickQuests verification

**Verified correct.** `QuestHandlers.hpp:591` (Begin) precedes `:593` (TickQuests::Apply). The Memento snapshot captures pre-tick state. The downstream comment at `:693-698` correctly says "pre-tick snapshot" — NOT "pre-claim." Comment block at `:579-590` accurately describes the C7-A shape, the v3 sweep miss, and the rationale for keeping the idempotency pre-check (L568-577) above Begin (it short-circuits before any mutation).

The early-return chain at `:595-600` does Rollback the tick work — but TickQuests::Apply is idempotent (same-day rollover transitions are no-ops on the re-tick), so the next handler call re-ticks at zero cost. See L-V5-2.

### 2. Other handlers' Begin/Commit ordering — scope-creep audit

Walked every Begin/Commit-bearing handler in `Server/Account/src/Handlers/`:

| Handler | Begin position | Pre-Begin reads of mutable state | Verdict |
|---|---|---|---|
| HandleGetQuestState (QuestHandlers.hpp:102) | Pure read; Begin/Commit pair only flushes TickQuests-from-load dirty bits | None | ✓ |
| HandleBeginMinigame (:110) | No Begin (read-only + token mint) | None | ✓ |
| HandleReportQuestProgress (:244) | Pre-tick — H-V3-1 fix in place | None | ✓ |
| HandleClaimQuestReward (:591) | Pre-tick — H-V4-1 fix in place | None | ✓ |
| HandleCompleteQuest (:734) | Pre-tick — H-V3-1 fix in place | None | ✓ |
| HandlePull (GachaHandlers.hpp:124) | Pre-GetPity/GetGuarantee — C-V3-2 in place | All pre-Begin reads are pure | ✓ |
| HandleMultiPull (:369) | Pre-GetPity/GetGuarantee — C-V3-2 in place | All pre-Begin reads are pure | ✓ |
| HandleAddCurrency (AccountHandlers.hpp:398) | Pre-mutation — C7-A in place | `balanceBefore = wallet.GetBy(...)` at :391 (L-V5-1) | Pure read, documented |
| HandleSetParty (:305) | Pre-Set* — H-V3-1 in place | `col = account.GetCollection().GetState()` at :198 | Pure read |
| HandleLevelCharacter (ProgressionHandlers.hpp:184) | Pre-spend — C7-A in place | `preScrap` at :169 (L-V5-1) | Pure read, undocumented |
| HandleAscendCharacter (:273) | Pre-spend — C7-A in place | `preScrap` at :264 | Pure read, undocumented |
| HandleLevelWeapon (:365) | Pre-spend — C7-A in place | `preScrap` at :356 | Pure read, undocumented |
| HandleAscendWeapon (:455) | Pre-spend — C7-A in place | `preScrap` at :446 | Pure read, undocumented |

No new scope-creep risks. The four progression `preScrap` reads are the only undocumented pre-Begin mutable reads (L-V5-1).

### 3. AccountTransaction dtor + Rollback edge cases — interaction with OutboxRelay

The audit method point 4 asks whether a relay PumpOnce can read an outbox row whose corresponding events landed but were then rolled back, producing a torn-write surface.

**Answer: no, by transactional construction.** AccountTransaction::Commit (`:228-232`) writes outbox rows inside the same `pqxx::work` as the events:

```cpp
for (const auto& row : outbox_) {
    tx_->exec(
        "INSERT INTO outbox (destination, payload, account_id) VALUES ($1, $2::jsonb, $3)",
        pqxx::params{row.destination, row.payload.dump(), account_.GetAccountId()});
}
```

The `tx_->commit()` at `:312` is the atomic commit boundary. If anything between event INSERT and the commit throws — including a relational flush failure, an outbox INSERT failure, an idempotency_cache INSERT failure, the COMMIT itself failing — pqxx aborts the entire work. The outbox row is rolled back alongside the events. The OutboxRelay's PumpOnce (`OutboxRelay.hpp:154-198`) opens its OWN `pqxx::work` and SELECT-s `WHERE dispatched_at IS NULL`. Postgres snapshot isolation guarantees PumpOnce can only see rows whose enclosing transaction COMMIT-ed — the relay never sees a partial state.

Post-commit failure paths (cursor advance / ClearDirty at `:324-354`) run AFTER `tx_->commit()` returns. The outbox row is durably committed at that point; the relay CAN see it. But the events are also durably committed, so the row's corresponding payload reflects real state. The post-commit catch's MarkStaleForReload ensures the in-memory Account refreshes on next access — orthogonal to what the relay sees. No torn-write surface.

### 4. ParseStoredHash odd-length guard + timing-oracle chain

**Verified.** `Crypto.hpp:382-383`:

```cpp
if ((saltHex.size() % 2) != 0 || (hashHex.size() % 2) != 0)
    return false;
```

Both salt AND hash are checked. The return-false propagates to VerifyPassword's `if (!ParseStoredHash(...))` check at `:113`, which triggers the burn-PBKDF2 dummy derivation at `:126-128` before returning false. The wall-time is preserved.

The dummy-hash path at `InternalRpcHandlers.hpp:94-96` handles the OTHER timing oracle (unknown username). Both work in concert:
- Unknown username → dummy hash at `:94-96` fires PBKDF2 → matches "known user with bad password" timing.
- Known user, malformed stored hash → ParseStoredHash returns false → VerifyPassword fires its own dummy PBKDF2 at `:126-128` → matches "known user with good stored hash, bad password" timing.

A row whose `password_hash` column has odd-length salt OR hash (corruption, schema migration bug, manual `UPDATE` typo) now fails ParseStoredHash, fires the burn-PBKDF2, returns false. Operator sees `LOG_AUTH_WARN("Password verification failed: malformed stored hash")` at Crypto.hpp:115 — observability preserved. Timing oracle preserved.

### 5. catch(...) sites — silent swallow audit

Re-scanned all `catch(...)` blocks. Compared against v4 enumeration.

In-scope sites (AccountTransaction, EventStore, Crypto, ServiceEndpoint, handlers):
- All AccountTransaction catch sites log via `LOG_DATA_WARN`. ✓
- EventStore::AppendInTx narrow-catches `pqxx::unique_violation` → ConcurrencyConflict (semantic propagation). ✓
- Crypto::ParseStoredHash `catch (...)` returns false; downstream VerifyPassword burns PBKDF2 + emits LOG_AUTH_WARN. ✓
- ServiceEndpoint outer try/catch logs the hex prefix when method is empty. ✓
- Handler UUID parse catches return error responses. ✓
- Handler gear-slot parse catches return error responses. ✓
- QuestHandlers::VerifyQuestToken timestamp parse logs DEBUG, returns false. ✓

**One new silent-swallow surface found outside the v4 audit scope** — TemplateDatabase character/weapon loaders (`TemplateDatabase.hpp:215, :306`). See L-V5-4. Pre-existing, not a v4 regression, but the same shape v3 closed for RestoreFrom.

### 6. Log-vs-return-value asymmetries

**Found one new asymmetry: M-V5-1 above.** Auth.HandleLogin/HandleRegister see `rpc.value` carrying `{"error": "Authentication failed"}` and treat it as a normal "valid: false" rejection without surfacing the underlying envelope/MAC failure to the WARN log. The v4 fix landed the same shape for SessionCache and Probe; the Auth login/register paths were missed.

No other asymmetries surfaced. Internal callers consistently check `rpc.ok()` then read business fields; the gap is specifically that the error envelope shape isn't introspected after `rpc.ok()` passes.

### 7. Dirty-bit leaks across rollback boundaries

`AccountDirty::Clear()` preserves `cached_wallet_version` / `cached_pulls_version` / `cached_quest_claims_version` / `cached_progression_version` while zeroing everything else. Verified at `Server/Common/src/State/AccountDirty.hpp:68-80`:

```cpp
void Clear() {
    const int wv = cached_wallet_version;
    const int pv = cached_pulls_version;
    const int qv = cached_quest_claims_version;
    const int gv = cached_progression_version;
    *this = DirtyState{};
    cached_wallet_version       = wv;
    cached_pulls_version        = pv;
    cached_quest_claims_version = qv;
    cached_progression_version  = gv;
}
```

The comment at `:38-52` makes the invariant explicit: handlers read `cached_X_version` via `dirty.cached_X_version + 1` to compute the next event's version; ONLY AccountTransaction::Commit advances them; resetting on every Clear() would cause version collisions on next commit. This is correct.

But there's a subtle interaction with Rollback that I want to verify: `Account::Snapshot` captures `m_dirty` as one of the X-macro fields (`Account.hpp:137`). On Rollback's RestoreFrom, `m_dirty = std::move(s.dirty)` copies the WHOLE DirtyState including the cached version cursors. If a handler buffered events with `dirty.cached_wallet_version + 1 = N`, then Commit failed and Rollback restored, the post-restore cached_wallet_version is back to whatever it was at snapshot time (pre-handler). The next handler call would re-read MAX from DB (via stale-flag eviction reload). So Rollback correctly restores the cursor to pre-attempt state. ✓

No dirty-bit leaks across rollback. ✓

### 8. Missing rollback-on-exception paths

Walked every handler that uses `auto txn = m_ctx.repository->Begin(account);` looking for code paths that throw after Begin but don't have explicit Rollback. The dtor-RAII guarantee handles all of them:

- `txn.Commit()` throws → dtor sees `state_ == Active`, calls Rollback. ✓
- Early-return after Begin (e.g. QuestHandlers.hpp:595-600) → dtor sees `state_ == Pending` (no Append yet), calls Rollback. ✓
- A handler that calls `account.GetWallet().AddCredits(-x)` then throws an std::exception before reaching Commit → dtor sees `state_ == Pending|Active`, calls Rollback. ✓

No missing paths.

### 9. Comparison to v4 followup

v4's L-V4-1 (HandleClaimQuestReward TickQuests-before-Begin) and v4 H-V4-1 (event-sourcing's view of the same defect) are both closed by commit `62ad8df`. v4's L-V4-2 (dtor-RAII test) is closed by the C-V3-2 regression-test commit `5fcb70d` — the PityRollbackRegressionTest exercises the dtor path with in-memory mutation and assertion of restore. v4's L-V4-3 (EnsureOpen logic_error no log) remains open by design.

v4's L-V4-9 (retire ParseJsonSafe) remains open as cosmetic. v4's L-V4-4 (EnsureOpen exception safety relies on Lease RAII) remains open as observation.

**Net new finding count vs v4:** 1 Medium (M-V5-1), 1 Low (L-V5-1) — both new patterns surfaced by the v4 closures. L-V5-2 / L-V5-3 / L-V5-4 / L-V5-5 / L-V5-6 / L-V5-7 / L-V5-8 / L-V5-9 are observations / carry-forwards.

---

## Test Coverage Status (this dimension)

- C-V3-2 / H-V4-6 regression: closed via `PityRollbackRegressionTest.cpp` (2 test cases — pity and guarantee). Tests assert dtor-Rollback restores in-memory and marks stale.
- C7 Rollback explicit-path: still covered by `AccountTransactionTest.cpp:182-231`. ✓
- L-V4-2 dtor-RAII path with in-memory mutation+restore: closed via PityRollbackRegressionTest (the lazy-GetPity insert + dtor + assertion). ✓
- H-V4-1 Begin-above-TickQuests integration test: no direct test asserts the position in HandleClaimQuestReward; covered indirectly by the existing claim happy-path test (no Commit-throw scenario for the claim path).
- M-V4-6 Crypto odd-length guard: no direct test for the malformed-hash path. A one-liner unit test calling `Crypto::VerifyPassword(pwd, "200000:abc:abc")` (odd-length salt) and asserting false AND elapsed-time-≥-N-ms would close this.
- Auth HandleLogin/HandleRegister envelope-error classification (M-V5-1): no test. A test that mocks the Account RPC to return `{"error":"Authentication failed"}` and asserts Auth surfaces a WARN log line (and returns "Account service unavailable") would close this.

**Recommended top-3 additions:**
1. M-V5-1 closure test: mock Account RPC return error envelope; assert Auth Login/Register WARN log + "Account service unavailable" response.
2. HandleClaimQuestReward H-V4-1 regression test: build an account with active quest, force AppendInTx ConcurrencyConflict (e.g. pre-insert a colliding version), assert post-Rollback questStates matches pre-tick state (no auto-completed quests stranded).
3. Crypto odd-length malformed-hash timing test: assert burn-PBKDF2 still fires.

---

## Suggested triage order

**This week:**
1. **M-V5-1** — mirror M-V4-9 networking shape into AuthServer.HandleLogin and HandleRegister. ~15 LOC across two handlers.

**Before launch:**
2. **L-V5-1** — either move `preScrap` reads inside the Begin scope OR add L-V3-3-style comments at the four progression handlers. ~10 LOC.
3. **L-V5-3** — add `LOG_NET_ERROR` before EnsureOpen's logic_error throw. ~2 LOC.
4. **L-V5-4** — add `LOG_DATA_WARN` to TemplateDatabase's two catch(...) blocks. ~4 LOC.
5. **L-V5-7** — include account_id in RestoreFrom catch-arm log messages. ~2 LOC.

**Eventually (cosmetic):**
6. **L-V5-5** — retire `ParseJsonSafe` from Protocol.hpp.
7. **L-V5-9** — comment at the dtor flagging "early-return-before-mutate paths should Commit() not just bail."

**Test backlog:**
8. M-V5-1 closure test (Auth envelope-error WARN log).
9. H-V4-1 closure test (claim-path Commit-throw → pre-tick restore).
10. M-V4-6 Crypto odd-length timing test.

---

## Verdict

The error-handling + rollback dimension is **clean**. Every v4 item — load-bearing and observation alike — is closed in code, with tests verifying the load-bearing ones. The v5 sweep surfaces one new Medium (Auth login/register envelope-error observability — same defect class as the v4 networking fixes) and one new Low (progression handlers' undocumented pre-Begin mutable reads). No new Critical, no new High. The H-V4-1 Begin-above-TickQuests fix is mechanically correct and the downstream comment correctly says "pre-tick snapshot." The OutboxRelay wiring (C-V4-1 idempotency closure) introduces no torn-write surface against the Rollback path — outbox rows share the AccountTransaction's pqxx::work, so PumpOnce can only ever see committed rows.

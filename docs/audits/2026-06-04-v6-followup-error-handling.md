# v6 followup — Error Handling + Propagation

**Date:** 2026-06-04
**Auditor:** Auditor v6 (follow-up to v5 2026-06-03)
**Scope:** Status of v5 items in this dimension + the eight audit-method focus questions.

Sources reviewed:
- v5 per-dimension report (`2026-06-03-v5-followup-error-handling.md`)
- v5 per-dimension security (`2026-06-03-v5-followup-security.md`) — overlap on H-V5-1
- Current code: `Server/Auth/src/AuthServer.hpp`, `Server/Common/src/Net/SessionCache.hpp`,
  `Server/Common/src/Net/ServiceEndpoint.hpp`, `Server/Common/src/Net/TcpServerBase.hpp`,
  `Server/Account/src/Cache/AccountTransaction.hpp`, `Server/Account/src/Cache/AccountRepository.hpp`,
  `Server/Account/src/Db/OutboxRelay.hpp`, `Server/Account/src/Db/ConnectionPool.hpp`,
  `Server/Account/src/Db/EventStore.hpp`, `Server/Account/src/Handlers/InternalRpcHandlers.hpp`,
  `Server/Account/src/State/Account.hpp`, `Server/Account/src/Gacha/BannerConfig.hpp`,
  `Server/Account/src/Gacha/TemplateDatabase.hpp`, `Server/Account/src/Effects/EffectDispatcher.hpp`.

---

## Verdict

**Clean.** Both v5 items are closed in code, and the eight focus-area sweeps surface
no new Critical or High. One Low (BannerConfig silent-swallow on the canonical `_meta.json`,
sibling of v5 L-V5-4 but slightly louder in impact) and three observations.

---

## Status of v5 items in this dimension

| v5 item | Status | Closure site |
|---|---|---|
| **H-V5-1** Auth HandleLogin/HandleRegister envelope-error classify | **CLOSED** (commit `88a9346`) | `AuthServer.hpp:295-300` (Register), `:366-371` (Login). Both now inspect `rpc.value.contains("error")` BEFORE the `value("result", rpc.value)` fallback. Log shape matches SessionCache (M-V4-9 / M-V5-2). |
| **M-V5-1** error-classification 4-string split (Unknown method / Internal error / Authentication failed / unsupported_envelope_version) | **CLOSED** at SessionCache (`SessionCache.hpp:162-184`). Split deployment-skew (LOG_AUTH_ERROR, no NoteAuthLost) from channel-integrity-loss (LOG_AUTH_WARN + NoteAuthLost). The Auth-side H-V5-1 closure uses the simpler classify (single WARN bucket) — acceptable because Auth doesn't gate on NoteAuthLost the way SessionCache does. |
| L-V5-1 progression `preScrap` pre-Begin reads | UNCHANGED (carry-forward; documented in v5 L-V5-1, no code change). |
| L-V5-3 EnsureOpen logic_error no log | UNCHANGED (carry-forward; design-deferred). |
| L-V5-4 TemplateDatabase silent catch(...) | UNCHANGED (still at `:215`, `:306`; v6 surfaces a louder sibling at `BannerConfig.hpp:79` — see L-V6-1 below). |
| L-V5-7 RestoreFrom catch-arm log without account_id | UNCHANGED. |

---

## NEW findings

### [L-V6-1] BannerConfig silently swallows `_meta.json` parse failure with empty `catch(...)` (no log)
**File:** `Server/Account/src/Gacha/BannerConfig.hpp:79`
**Status:** NEW (sibling of L-V5-4, but worse impact)
**Finding:** The canonical banner-slot config file `data/banners/_meta.json` is parsed inside
a `try` block whose `catch (...) {}` is fully empty — no log line, no fallback signal.
If the file is malformed or contains a `Json::parse` throw, the function continues to the
layouts loop. The trailing summary log (`LOG_DATA_INFO("BannerConfig: Loaded {} slots and {} layouts", ...)`)
reports `0` slots, but the only way ops would notice is by reading the startup log carefully
and remembering that `_meta.json` was supposed to define slots. The L-V5-4 sites at TemplateDatabase
have the same shape but they're per-template files inside a directory iteration; the BannerConfig
site is the **single** canonical slot-definition file, so a malformed `_meta.json` zeroes the
entire banner system silently.

The sibling at `:98` (`catch (...) { continue; }`) inside the per-layout file loop is closer
to the TemplateDatabase pattern — a single bad layout file is skipped silently, the surviving
files still load. Both deserve a one-line `LOG_DATA_WARN`.

**Impact:** A typo in `_meta.json` (e.g. trailing comma, missing brace) → every banner request
hits an empty slot table → every gacha pull silently fails (or, depending on caller path,
silently returns wrong-rate items). Forensically the operator sees "Loaded 0 slots and 4 layouts"
on startup but no error log line — exactly the failure mode v5 L-V5-4 flagged for character/weapon
templates, but here the blast radius is the entire gacha-rate machine, not a single missing item.

**Fix sketch:** Mirror the L-V5-4 fix shape — add `LOG_DATA_WARN("BannerConfig: failed to parse _meta.json at {}: {}", metaPath.string(), e.what());`
inside the first catch arm (use `catch (const std::exception& e)`). Same for the layouts-loop catch
at `:98`. ~4 LOC total.

---

## Observations / Lows

- **OBS-V6-1 (TcpServerBase::ProcessMessage error log omits playerId).** `TcpServerBase.hpp:625-632`
  resolves `playerId` from the session BEFORE the try block, but the catch-arm log at line 629
  formats only `msg.type` and `clientIP` — not the playerId that was successfully resolved at
  line 620. When an authenticated handler throws (e.g. pqxx::sql_error during Pull commit,
  EventStore::ConcurrencyConflict, ConnectionPool::PoolExhausted), the error line reads
  `Exception handling message type 42 from 10.0.0.1: ...` with no player association. Cross-correlation
  to the affected account requires joining against the message-level access log. One added
  `playerId` format arg would close the gap without leaking PII (playerId is the internal
  account string, not the username). Note: for non-authenticated handlers (Login/Register routed
  through Auth) playerId is empty and the gap doesn't apply — but those go through Auth's
  own dispatcher anyway.

- **OBS-V6-2 (EventStore::ConcurrencyConflict propagates to dispatcher catch as generic
  "Internal server error").** No handler catches `ConcurrencyConflict` explicitly; it bubbles
  through `AccountTransaction::Commit()` → handler → TcpServerBase::ProcessMessage's
  std::exception arm → returns `"Internal server error"`. Correctness is fine — the dtor's
  Rollback + MarkStaleForReload runs along the way, so the next handler call starts from a
  clean DB-read; the client retries via the idempotency key and gets the cached response if
  one was committed, or re-tries fresh. The observability gap: ops see "Internal server error"
  for what was actually a write-write race. A dedicated catch-arm in ProcessMessage that
  classifies ConcurrencyConflict → "Concurrent modification detected, please retry" would
  distinguish race from real bugs in the logs. Pre-launch this is signal-deferred. Worth
  noting because v5 introduced ConcurrencyConflict as a semantic propagation channel
  (audit C6) but the dispatcher never learned the new vocabulary.

- **OBS-V6-3 (OutboxRelay::PumpOnce commit-throw → at-least-once handler re-fire).** The
  `tx.commit()` at `OutboxRelay.hpp:247` is outside any try block in PumpOnce. If it throws,
  the surrounding try in `Run()` (line 176-178) catches and logs WARN. The UPDATEs marking
  rows dispatched_at=now() are then rolled back along with the SELECT FOR UPDATE — the rows
  stay queued and the relay reprocesses them next iteration, including re-invoking handlers
  that already succeeded. This is the standard at-least-once outbox semantic and is correct,
  but it's load-bearing for the eventual handler contract: every handler registered via
  `Register()` MUST be idempotent. Pre-launch there are zero registered handlers (per the
  H-V5-2 DEFER comment at `:57-86`), so the surface is dormant. Worth flagging because the
  contract is implicit in the code rather than enforced at the Register signature. Future
  handler authors should look at this comment before adding a non-idempotent dispatch path.

- **OBS-V6-4 (CaptureSnapshot allocation-failure path).** `Account::CaptureSnapshot()` at
  `Account.hpp:190-200` allocates a fresh DirtyState + CollectionState copy + sub-object
  copies in the member-init list of AccountTransaction. If any allocation throws (OOM,
  std::bad_alloc), AccountTransaction is never fully constructed → its destructor doesn't run
  → no Rollback, no MarkStaleForReload. The handler's `auto txn = m_ctx.repository->Begin(account)`
  rethrows, propagates to TcpServerBase dispatcher catch → "Internal server error". The
  Account has not been mutated yet (the snapshot is taken FIRST in the ctor, before any
  handler body runs), so there is no in-memory corruption to roll back from. The Account is
  consistent. Correctness is fine. The only failure mode is the same "client sees Internal
  server error, retries" — same idempotency story as everywhere else. Noting because the
  v5 audit flagged the symmetric case (RestoreFrom throwing during Rollback); the Capture
  side of the Memento has equivalent correctness and the same observability gap (no log
  identifying which handler's Begin allocated → OOM).

---

## Focus Question Walkthrough (v6)

### 1. H-V5-1 status

**CLOSED.** Verified `AuthServer.hpp:295-300` (Register) and `:366-371` (Login) both inspect
`rpc.value.contains("error")` BEFORE the legacy `value("result", rpc.value)` fallback. Log shape
matches the SessionCache pattern. Commit `88a9346` ("fix(auth): envelope-error classify in
HandleRegister + HandleLogin (H-V5-1)").

### 2. AccountTransaction error paths

- **CaptureSnapshot in ctor:** can throw on OOM (heap allocation for CollectionState copy).
  If it throws, AccountTransaction is never fully constructed → no dtor → no Rollback. But
  the snapshot is the very first thing taken, BEFORE any mutation in the handler body, so
  the Account has not been touched. Propagating to the dispatcher is correct: nothing to
  roll back. See OBS-V6-4.
- **Commit failure → Rollback via dtor:** verified. `tx_->commit()` throw leaves `state_ == Active`
  (line 351-353 don't get reached); dtor at line 63 sees Active → Rollback. Rollback calls
  `tx_->abort()` which is safe-after-failed-commit per pqxx semantics; restores snapshot; marks
  stale. ✓
- **EnsureOpen pool exhaustion:** `pool_.acquire()` throws `PoolExhausted` (ConnectionPool.hpp:78).
  Propagates out of AppendEvent → handler → dispatcher catch → "Internal server error". The
  AccountTransaction's `state_` stayed `Pending` (line 478 didn't complete); dtor sees Pending
  → Rollback runs → tx_ is null, abort skipped; RestoreFrom + MarkStaleForReload runs. The
  Account snapshot was already captured in the ctor, so restore is a no-op semantically but
  the stale-mark forces next reload. ✓ Acceptable.
- **AppendEvent / EmitToOutbox / StoreIdempotency / RecordAudit when EnsureOpen throws:** all
  four call EnsureOpen as their first line. PoolExhausted propagates; handler dispatcher
  catches. No buffered ops were appended (the throw beat the push_back). Correct.

### 3. AccountRepository error paths

- **Save failure logging (M-V2-1):** verified in place at `AccountRepository.hpp:206-211`.
  ERROR log identifies `account_id` and `e.what()`. Returns false. Callers (idle eviction)
  treat the false return as "skip this eviction tick, retry on next eviction." Stale-account
  early-return at `:177-180` is also in place per H-V2-2-class fix.
- **Create / BumpLastLogin / UpdatePasswordHash / Delete / LoadByAccountId / FindIdempotency:**
  all wrap pqxx ops in `try { ... } catch (const std::exception& e) { LOG_DATA_ERROR/WARN(...) }`
  with `account_id` / `username` context in the message. No silent-swallow surfaces.

### 4. OutboxRelay error paths

- **Run() worker loop:** each of the four periodic ops (`PumpOnce`, `PruneDispatchedOutbox`,
  `SweepExpiredIdempotency`, `RunPartmanMaintenance`) is wrapped in its own try/catch that
  logs WARN and continues to the next iteration. No silent-swallow.
- **PumpOnce internal:** payload-parse failure logs WARN with `outbox_id` + `destination` and
  continues to next row. Handler throw is caught and logs WARN with the same correlation,
  treats as failed dispatch (row stays queued). The `tx.commit()` at line 247 is outside
  the inner try — see OBS-V6-3. Correct at-least-once contract; not a silent-swallow.

### 5. pqxx exception flow

- **`pqxx::sql_error`, `pqxx::broken_connection`, `pqxx::data_exception`** all inherit from
  `std::runtime_error` → `std::exception`. AccountRepository methods catch them broadly
  (`catch (const std::exception& e)`) with `LOG_DATA_ERROR(... e.what() ...)`. The `e.what()`
  for sql_error includes the SQLSTATE, so the error code surfaces in logs even though it's
  not parsed structurally. Acceptable.
- **`pqxx::unique_violation`** is narrow-caught at `AccountRepository.hpp:150-154` (Create)
  and translated to nullopt return (the "username already exists" semantic). Correct.
- **EventStore::AppendInTx translates duplicate-key into ConcurrencyConflict** at `EventStore.hpp:99`.
  This is the only semantic propagation channel above the std::exception generic class. See
  OBS-V6-2 about the dispatcher not classifying it.
- No pqxx-class catches are too narrow or too broad in a way that loses correctness signal.

### 6. Handler dispatch error policy (TcpServerBase::ProcessMessage)

- The catch arm at `TcpServerBase.hpp:627-632` wraps all handler exceptions into "Internal
  server error" and logs at NET_ERROR with `msg.type` + `clientIP`. Verified no exception
  classes are missed — `std::exception` covers everything thrown by any pqxx operation, the
  ConcurrencyConflict, the PoolExhausted, all UuidV7 parse failures, all Json::parse failures.
  Nothing escapes to crash the server thread.
- The cost is observability: every exception masks into the same "Internal server error" wire
  response. That's the correct contract for the client (don't leak internals), but the log
  line at line 629 drops `playerId` — see OBS-V6-1.
- "Should this be DROP-CONNECTION?" — no. The current "send error response, keep the
  connection alive" path is the correct contract: the client retries via idempotency, and
  the underlying issue (transient DB hiccup, advisory-lock conflict, etc.) is independent of
  the TCP session. Force-dropping the socket would make the client reconnect + re-auth,
  amplifying any pool-exhaustion event into a stampede. Keep as-is.

### 7. Internal RPC error envelope — other call sites that don't check for `["error"]`

Three `m_*Client.Call(...)` call sites enumerated across the codebase:
- `Server/Auth/src/AuthServer.hpp:268` (CreateAccount) — CLOSED by `88a9346`.
- `Server/Auth/src/AuthServer.hpp:346` (VerifyCredentials) — CLOSED by `88a9346`.
- `Server/Common/src/Net/SessionCache.hpp:117` (AuthorizeToken) — CLOSED by M-V4-9 + M-V5-2
  (split-classify). Verified at `SessionCache.hpp:162-184`.

**All three sites now classify envelope errors.** No remaining call sites that bypass the
`["error"]` check.

### 8. Verify error-message formats — substantial signal-loss

Two log-line gaps worth flagging:
- **OBS-V6-1** above: TcpServerBase's catch drops playerId.
- The ServiceEndpoint catch at `ServiceEndpoint.hpp:361` formats `method` + `e.what()`, which
  is adequate for triage. The empty-method case at `:343-358` includes the raw hex prefix
  per L-V3-2. Both adequate.

No other substantial signal-loss surfaces in NET_ERROR / AUTH_ERROR / DATA_ERROR sites.

---

## Suggested triage order

**Before launch:**
1. **L-V6-1** — BannerConfig empty `catch(...) {}` at `:79` + `:98`. ~4 LOC.
2. **OBS-V6-1** — Add `playerId` to TcpServerBase catch-arm log. ~1 LOC.
3. **L-V5-4** (carry-forward) — TemplateDatabase silent-swallow at `:215`, `:306`. ~4 LOC.

**Eventually (post-launch ops signal):**
4. **OBS-V6-2** — Dedicated ConcurrencyConflict catch arm in TcpServerBase::ProcessMessage
   for richer "retry"-class log signal. ~5 LOC.

**Out of scope / observation-only:**
- OBS-V6-3 (outbox at-least-once contract) — correct by design, no fix needed; dormant pre-launch.
- OBS-V6-4 (CaptureSnapshot OOM path) — correct by design; the snapshot-before-mutation
  ordering means no in-memory corruption is possible.

---

## Verdict

The error-handling + propagation dimension is **clean**. Both v5 items (H-V5-1 envelope-error
classify in Auth, M-V5-1 4-string deployment-skew split) are closed in code with audit comments
that survive the round-trip. The eight focus-area sweeps surface one new Low (BannerConfig
silent-swallow on the canonical `_meta.json`) and four observations (playerId omission in
dispatcher log; ConcurrencyConflict not classified by dispatcher; outbox at-least-once is
correct-but-implicit; CaptureSnapshot OOM path is correct-by-design). No new Critical, no new
High, no v5 regressions.

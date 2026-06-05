# v4 Follow-up Audit: Idempotency + Retry
**Date:** 2026-06-03 (post v3 remediation arc, commits `f46d496..HEAD`)
**Scope:** End-to-end idempotency posture across `Server/Account/src/Cache/AccountTransaction.hpp`, `Server/Account/src/Cache/AccountRepository.hpp`, `Server/Account/src/Db/OutboxRelay.hpp`, `Server/Account/src/Db/EventStore.hpp`, `Server/Account/src/Handlers/{Gacha,Quest,Account,Progression}Handlers.hpp`, `Server/Account/src/AccountServer.hpp`, `Server/Account/src/main.cpp`, `Server/Common/src/Idempotency/IdempotencyKey.hpp`, `Server/Account/schema.sql`, `Client/src/network_tcp.lua`, `Client/src/services/{ProgressionService,gacha_actions}.lua`, `Server/Account/tests/**`.
**Method:** Single-agent read-only sweep of every named v3 fix site + targeted code-path tracing for the seven focus areas the orchestrator named (client wiring, scope prefix discipline, partition-vs-cache dedup, sibling key construction, TTL, ON CONFLICT semantics, sweeper cadence, M-V2-7 response cache breadth).

---

## Verdict

**Concerning — one Critical, three Highs.** The headline finding is a **complete unwiring of the v3-H-V3-5 sweeper**: `OutboxRelay::SweepExpiredIdempotency` was implemented, doc-commented, and even called out in `schema.sql` as the production sweeper, but `OutboxRelay` is **never instantiated** anywhere outside its own unit test (`Server/Account/tests/Integration/OutboxRelayTest.cpp:37,77,113`). `AccountServer.hpp` constructs `m_pool`, `m_repository`, `m_cache`, `m_internalRpcHandlers`, but never creates an `OutboxRelay`. `main.cpp` likewise has no reference. That means the v3 closures of H-V3-4 (outbox prune), H-V3-5 (idempotency sweeper + partman maintenance), and the partman-cron retirement all live entirely in dead code — production `Account.exe` has none of them running. Every other v3 remediation in the dimension verifies clean. There are no new partition-local dedup defects beyond what v3 already documented; client wiring is unchanged from v3-M1 (still dead in tree, M-V2-7 doc still wallet-only).

---

## CRITICAL

### C-V4-1. `OutboxRelay` is never instantiated in production — v3 H-V3-4 / H-V3-5 closures are dead code
**Files:**
- `Server/Account/src/Db/OutboxRelay.hpp:29-208` (class fully implemented, including `PruneDispatchedOutbox` L112, `SweepExpiredIdempotency` L129, `RunPartmanMaintenance` L143)
- `Server/Account/src/AccountServer.hpp:62-126` (constructor wires `m_pool`, `m_repository`, `m_cache`, `m_internalRpcHandlers` — no `OutboxRelay` member, no instantiation)
- `Server/Account/src/main.cpp` (no `OutboxRelay` reference; the only callers in the tree are tests)
- `Server/Account/schema.sql:408-410` (comment claims "audit H-V3-5 (2026-06-03) added an in-process sweeper (OutboxRelay::SweepExpiredIdempotency) that runs every minute to delete expired rows")
- `Server/Account/tests/Integration/OutboxRelayTest.cpp:37,77,113` (the only sites that construct `OutboxRelay`)

`grep -rn "OutboxRelay\|outbox_relay\|outboxRelay"` across the entire `Server/` tree returns exactly three production files: `Db/OutboxRelay.hpp` (the class itself), `schema.sql` (a comment), `tests/Integration/OutboxRelayTest.cpp` (three test constructions). No `AccountServer` member, no `main.cpp` reference, no service-startup hook. The v3 fix was implemented at the class level but the integration step was never taken.

Real-world consequences in a deployed Account.exe today:

1. **`idempotency_cache` grows monotonically.** The `expires_at > now()` filter on `FindIdempotency` (AccountRepository.hpp:302) hides expired rows from lookups but never deletes them. Estimate from v3-H-V3-2: ~15 MB/day at 1k DAU, ~5 GB/year, plus increasing scan cost as expired-rows pile up in the `idempotency_cache_expires_idx` leaves. The 24h `IDEMPOTENCY_DEFAULT_TTL_SECONDS` (AccountTransaction.hpp:107) does nothing if no DELETE ever runs.
2. **`outbox` grows monotonically.** v3-H-V3-4 documented this as the integration-test pain point ("had to db-reset mid-session because 73 undispatched rows pushed test-seeded rows past LIMIT 64"). The `PruneDispatchedOutbox` fix is implemented but unreachable in production.
3. **`events` partitions stop being created.** `partman.create_parent(..., p_premake=12)` (schema.sql:328) seeds 12 monthly partitions at DB initialization. With no in-process `partman.run_maintenance_proc()` driver and no external pg_cron (per the v3 commit message), the 13th month's first INSERT (≈ December 2027 for a today-initialized DB) fails with "no partition of relation 'events' found for row." Account.exe stops being able to commit any event-sourced mutation past that date.
4. **No retention enforcement.** The 24-month retention configured in `partman.part_config` (schema.sql:341-344) is a no-op without `run_maintenance_proc` running.

The v3 audit doc (`2026-06-03-server-persistence-audit-v3.md`) records H-V3-4 and H-V3-5 in the "Verified Closed" section by implication (the per-dimension idempotency follow-up in v3 even lists them as triage'd). They are not closed in the deployable artifact.

**Fix:** instantiate `OutboxRelay` in `AccountServer` (member after `m_pool` so the relay's `ConnectionPool&` reference is valid; relay's destructor calls `Stop()` which joins the worker thread, so member-declaration order also handles teardown). Add an explicit `m_outboxRelay.Stop()` in `AccountServer::~AccountServer` ahead of `Stop()` for symmetry with H-V3-7's TcpServerBase concern. Or — explicit `std::optional<aphelyon::db::OutboxRelay>` constructed late in the body so the worker doesn't start until banners/templates/quests have finished loading (those file I/O paths today hold up to a few seconds; a 500ms-tick sweeper firing during init would not cause harm, but is also wasted work).

---

## HIGH

### H-V4-1. Client `idempotencyKey` fallback still mints a fresh UUID on every retry — same shape as v3-M1, gap unchanged
**Files:** `Client/src/network_tcp.lua:1205-1283` (Pull / MultiPull), `:1335-1377` (AddCurrency), `:1417-1447` (4 progression wrappers); `Client/src/services/ProgressionService.lua:134-167`; `Client/src/services/gacha_actions.lua:149-169`

Verbatim status from v3-M1: every wrapper still defaults to `idempotencyKey or uuid4()`. `ProgressionService.{level,ascend}{Character,Weapon}` and `gacha_actions.gacha.debug_add_tickets` mint keys at call-time (per-call uuid4 or per-callsite mint, neither of which is a per-gesture cap). Quest claim does it correctly via `NetworkTCP.claimQuestReward` (network_tcp.lua:1859-1907 — payload-level mint at outer scope so `executeWithRetry` reuses the key across retries).

The remaining concern was promoted from v3-M1 to v4-H because:

1. **`executeWithRetry` interaction.** `NetworkTCP.pull` and `multiPull` (lines 1205 / 1245) do NOT wrap themselves in `executeWithRetry`. They take a callback and resolve once. But the retry path that v3-M1 worried about isn't the only failure mode — a network glitch DURING the `conn:send` call (handled at line 1220-1224 via `handleConnectionLoss`) does NOT retry; the client surfaces the error to the UI. If the UI's retry handler then re-invokes `NetworkTCP.pull` without re-supplying the original key, a new uuid4 is minted and the server treats it as a fresh request. With AddCurrency (the canonical wallet-grant message per the project memory), this is a double-spend on the very class of action the cache is supposed to protect.
2. **gacha_actions debug hotkey** mints two keys per hotkey press (one for tickets, one for limited_tickets) at line 162-163. Each fresh hotkey press mints fresh keys. Held-hotkey re-fire (the LÖVE keyrepeat surface) re-enters the handler, mints new keys, gets new grants. Comment at line 145-148 acknowledges this is debug-only — but the ProgressionService level-up wrappers will see the same shape the moment the F3/Inventory rework wires a button to them, and the contract today encourages "let Network mint" rather than "UI must mint and pass."

Promoting because v3-M1 explicitly noted the inventory rework would expose this, and the rework is named in the current memory as planned/imminent (`project_inventory_party_rework`). The lint/CI hook v3-M1 suggested is still absent.

**Fix:** either (a) drop the `or uuid4()` fallbacks in `NetworkTCP.*` (let nil propagate; force callers to mint), or (b) add a pre-commit / Selene hook that flags any call to `Network.{pull,multiPull,addCurrency,level*,ascend*}` missing the 4th arg, or (c) move the per-gesture mint into ProgressionService and gacha_actions's `register("...", function(p) ... end)` body where the user gesture is observable. Same fix universe as v3-M1, just no longer deferrable.

### H-V4-2. M-V2-7 doc-comment still only mentions wallet — every other point-in-time field in the cached pull/claim response remains undocumented
**Files:** `Server/Account/src/Cache/AccountTransaction.hpp:114-126` (doc-comment); cached payloads in `GachaHandlers.hpp:274-291` (pull) / `:611-627` (multi-pull), `QuestHandlers.hpp:640-682` (claim)

The v3 audit verbatim flagged this as `V3-M2`. The doc-comment still reads "any wallet balances / pity counters / story state the handler embedded in its response body" — naming wallet, then waving at the others. The actual point-in-time fields that retried idempotent responses freeze are:

- Pull / MultiPull response: `credits`, `universal_credits`, `tickets`, `limited_tickets`, `scrap`, `pity_counter`, `four_star_pity`, `guaranteed_next`, `pulls_until_hard_pity`, `pulls_until_four_star`, `pulls_until_soft_pity`, `in_soft_pity`, `consecutive_fifty_fifty_losses` (14 fields).
- Quest claim response: `wallet.{credits,universal_credits,tickets,limited_tickets,scrap}`, `login_streak`, `story_level`, `story_xp`, `xp_to_next`, `difficulty_tier`, `updated_quests` (array projection), `bonus_rewards`, `rewards`, `quest` (16 fields including nested arrays).
- AddCurrency response: `BuildStatePayload(account)` — the entire `StateResponse` envelope, which the client's `GachaService:updatePlayerState` writes directly into UI state.

Promoting this to High (was M in v3) because:

1. The `BuildStatePayload` cached on AddCurrency hits the same channel the client uses for its periodic state refresh. A 23h-old cached AddCurrency response on retry overwrites a player's freshest state with their state-as-of-yesterday. The current debug-hotkey AddCurrency caller flows directly into `svc:updatePlayerState(data); svc:notifyStateChanged()` (`gacha_actions.lua:155-158`) — every binding the UI subscribes to gets the stale snapshot.
2. The retry semantic intent ("a true replay of the original") is a defensible position for pull and claim, where the actual minted entities (the 5-star you got) are immutable. It is NOT defensible for AddCurrency, where the response IS the player's current wallet view and the client trusts it as authoritative. The current cache TTL is 24h; a player with a slow connection retrying an in-flight AddCurrency 22h later sees their balance jump backwards.

Promoting also catches a side-effect from C-V4-1: with no sweeper, the 24h TTL no longer bounds the staleness window — the `expires_at > now()` filter at FindIdempotency still rejects the lookup at 24h+1s, so practical staleness is capped at 24h regardless. But that's a coincidental safety net from an unrelated bug; the doc contract should not depend on the sweeper bug.

**Fix:** in priority order — (a) for AddCurrency specifically, do NOT cache `BuildStatePayload` (cache a minimal `{success:true, grant_id:...}` ack instead, force the client to follow up with `GetState` like the pull path already does); (b) extend the doc-comment to enumerate every point-in-time field per handler; (c) add a `_cached_at` epoch field to cache-hit responses so the client can detect retroactivity and follow up with a refresh.

### H-V4-3. No test exercises the `StoreIdempotency` → `FindIdempotency` round-trip; cache-hit semantics are entirely untested
**Files:** `Server/Account/tests/**` (zero matches for `idempotency_cache`, `StoreIdempotency`, `FindIdempotency`, or per-RPC `Idempotent` prefix outside the IntegrationDbFixture cleanup note); `Server/Account/src/Cache/AccountTransaction.hpp:127-133` + `:247-261` (the StoreIdempotency buffer + commit logic); `Server/Account/src/Cache/AccountRepository.hpp:289-320` (FindIdempotency)

The seven RPC sites that depend on cache-hit short-circuiting (pull, multi_pull, admin_grant, claim, level_char, ascend_char, level_wpn, ascend_wpn) all share the same shape: look up cache → on Hit return cached payload, on Miss proceed. None of these "on Hit short-circuit" branches is covered by any test:

- `AccountTransactionTest.cpp` exercises Commit / Rollback / lazy-lease / version-cursor advance but never calls `StoreIdempotency`.
- `EventStoreRoundTripTest.cpp:74-86` covers `AppendIdempotent` (the event-table dedup) which v3-H-V3-12 already flagged as moot for production paths. Doesn't touch `idempotency_cache`.
- No test seeds a `idempotency_cache` row and verifies the in-handler retry returns the cached payload.
- No test verifies `ON CONFLICT DO UPDATE` refresh semantics (H2 in v2, M-V3-3 in v3).
- No test verifies the partition-local-vs-cache dedup direction documented at QuestHandlers.hpp:320-333.
- No test verifies sibling-key construction (the `:wallet:<currency>` and `:progression` suffixes).

This wasn't its own item in v3's per-dimension idempotency follow-up but came in via the v3-test-coverage findings. Promoting to a High in v4 because the lack of coverage masks both C-V4-1 (the sweeper would be caught by any test that verified expired rows disappear) and lets future refactors silently regress the dedup story. A single 80-line integration test (seed a cache row → call handler → assert short-circuit returns the seeded payload) would close the regression-detection gap for the entire idempotency surface.

**Fix:** add at minimum:
1. `IdempotencyCacheRoundTripTest` — write a row via `StoreIdempotency`, read via `FindIdempotency`, assert payload byte-identical.
2. `IdempotencyCacheConflictRefreshTest` — write key K → write key K again with different payload → assert new payload wins and `expires_at` advanced (locks down the v2-H2 → v3-M3 contract).
3. `IdempotencyCacheExpirySweepTest` — write row with TTL=1s, sleep 2s, invoke `OutboxRelay::SweepExpiredIdempotency` (requires C-V4-1 fix), assert row deleted. (Doubles as the regression test for C-V4-1.)
4. `PullCacheHitShortCircuitTest` — seed cache row for `pull:<key>`, invoke `HandlePull` with same client key, assert no event was appended, no wallet was charged, and the returned payload byte-matches the seed.

---

## MEDIUM

### M-V4-1. Sweeper cadence is `% 130 pumps × 500ms = 65 s`, not "every 60s" / "every minute" as the doc claims
**File:** `Server/Account/src/Db/OutboxRelay.hpp:69-71` + `schema.sql:408-410` + `Db/OutboxRelay.hpp:78-106`

Schema comment promises "every minute"; the constant `IDEMPOTENCY_SWEEP_EVERY_PUMPS = 130` with a 500ms `interval_` works out to 65 seconds. Outbox prune at `% 120 = 60s`. partman maintenance at `% 7200 = 3600s = 1h`. The drift values (130 vs 120) were chosen so the three pruners don't stampede on the same tick (per the comment at line 67-68), but the schema doc and the OutboxRelay doc-comment at line 410 should both say "every ~65s" or the constant should be 120 to match.

This is M-class because (a) the absolute drift is small and (b) the wider problem is C-V4-1 (the sweeper doesn't run at all). Closes naturally if the C-V4-1 wiring chooses a different cadence; flagging for completeness.

### M-V4-2. `ON CONFLICT DO UPDATE` semantic on `idempotency_cache` corrupts on horizontal-scale stripe-lock bypass
**Files:** `Server/Account/src/Cache/AccountTransaction.hpp:247-261`

Verbatim continuation of v3-M3 / v2-followup-idempotency-M2. The `DO UPDATE SET response_payload = EXCLUDED.response_payload, expires_at = EXCLUDED.expires_at, created_at = now()` is correct for the documented use case (refresh on long-tail retry-after-expiry). On a future horizontal-scale path that bypasses the per-player stripe lock, two concurrent commits with the same `scoped_key` would each write — the second wins, the first's payload is gone. Pre-H2 `DO NOTHING` would have preserved the first.

Status unchanged from v3. Single-instance today, so still defensive. Worth flagging because the C-V4-1 fix will involve more touch to AccountTransaction-adjacent code; if a maintainer cleans up the `ON CONFLICT` clause at the same time, they should pick the policy that survives multi-instance ("preserve fresh rows from racers" = `DO UPDATE ... WHERE idempotency_cache.expires_at < now()`) rather than the current "newest wins."

### M-V4-3. Per-RPC scope prefix `admin_grant` still misnames the canonical wallet-grant path
**Files:** `Server/Account/src/Handlers/AccountHandlers.hpp:354,423` (AddCurrency); v3-L2 already documented this; promoting to M for v4 visibility.

Per project memory `project_addcurrency_not_debug_only`, AddCurrency is the canonical wallet-grant message used by purchases / quests / achievements — not a debug-only or admin-only path. The scope prefix `admin_grant:` for the cache key + the event's fallback `idempotency_key = "admin_grant:..."` mislabel its real role. Cache rotation requires a coordinated client+server rollout (existing retries during the deploy window would miss the cache and re-execute), so the renaming is non-trivial.

Status unchanged but promoting from L → M because the v3 audit recorded AddCurrency as the canonical grant path with a documented mismatch — every future audit will continue to flag this. Better to rotate once cleanly (during a planned downtime / cache-bypass migration) than to keep dismissing the finding in every dimension's report.

**Fix:** during the next planned wallet-handler refactor (or the inventory rework), rename to `grant:` or `wallet_grant:`. Drain the existing `admin_grant:` rows by waiting one full TTL (24h post-deploy) before the cache logic stops looking at the old prefix.

### M-V4-4. Quest sibling key direction-blindness (M-V3-4) is documented but has no test
**Files:** `Server/Account/src/Handlers/QuestHandlers.hpp:478-492` (sibling wallet event key), `:534` (sibling progression event key)

The v3-M-V3-4 finding noted that sibling keys include currency name (`:wallet:credits`) but not delta direction (`+` vs `-`). v3 added a paragraph-long doc comment at the emitDelta site (QuestHandlers.hpp:480-491) saying "implicit invariant: a single quest-claim produces AT MOST one delta per currency. Claim rewards are additive (no scrap-burn-on-claim path today), so the invariant holds. If a future claim ever needs to spend currency X and grant currency X in the same atomic commit, both deltas would collide on this idempotency_key and the second one would be rejected by UNIQUE — extend the sibling key (e.g. `:wallet:<currency>:<direction>`) BEFORE adding such a flow."

The invariant is doc-only. No test asserts that a same-currency-burn-and-grant claim trips the UNIQUE constraint (which would be the canary regression). A future contributor adding a scrap-burn quest claim wouldn't see the failure in unit tests; it would surface as a Postgres unique-violation at runtime, which AccountTransaction promotes to ConcurrencyConflict and then to a 500-class response — the player sees "Internal server error" rather than a meaningful diagnostic.

**Fix:** add an integration test that constructs a hypothetical claim event bundle with two same-currency deltas, attempts `txn.Commit()`, and asserts the unique-violation surfaces. Adds the regression guard against the "future claim that spends X and grants X" footgun without requiring the cost of the actual schema extension today.

### M-V4-5. `request.value("idempotency_key", "")` throws on non-string field type
**Files:** All 8 handler sites (`GachaHandlers.hpp:65,316`, `AccountHandlers.hpp:331`, `QuestHandlers.hpp:555`, `ProgressionHandlers.hpp:130,225,314,404`)

Verbatim continuation of v3-M4 / v2-followup-idempotency-M4. Unchanged. `nlohmann::json::value(key, default)` throws `type_error::302` if the field exists but isn't a string. Surfaces as "Internal server error" to the client. A misbehaving SDK that JSON-encodes a UUID as integer gets an opaque 500 instead of a clear error. State is safe (the throw happens pre-Begin), but the diagnostic is terrible.

Status unchanged from v3-M4. Carrying forward.

**Fix:** centralize in `IdempotencyKey::Scoped` by overloading to `Scoped(std::string_view prefix, const Json& request)` — pulls the field with explicit type-check, returns empty + logs WARN on type mismatch. Eliminates the throw class at all 8 sites in one helper change.

### M-V4-6. `IdempotencyKey::MAX_CLIENT_KEY_BYTES = 128` undocumented in client wrappers
**Files:** `Server/Common/src/Idempotency/IdempotencyKey.hpp:52`; `Client/src/network_tcp.lua:1205-1444`

H3 (v1) added the 128-byte cap on the server side; the rejection path returns empty scoped key → "request still executes uncached" (good, no DoS surface). But the client wrappers all default to `uuid4()` (36 chars) and the doc-comments don't note the limit, so a future client that decides to embed structured info in the key (e.g. `pull-<screenId>-<buttonId>-<userClickTimestamp>-<retryAttempt>` for analytics) could silently cross 128 bytes and lose dedup without the player or client developer ever knowing.

**Fix:** add a single-line comment to each `NetworkTCP.{pull,multiPull,addCurrency,level*,ascend*}` wrapper: `-- idempotencyKey: max 128 bytes; longer is silently uncached server-side`. Add a Lua-side assert in dev builds: `assert(not idempotencyKey or #idempotencyKey <= 128, "idempotencyKey > 128 bytes")`.

### M-V4-7. Cache-hit path skips rate-limit budget BUT also skips audit log
**Files:** `Server/Account/src/Handlers/AccountHandlers.hpp:354-363` (AddCurrency cache-hit), `:415-440` (Miss path runs the wallet-event commit which triggers `RelationalFlush` but NOT an explicit `txn.RecordAudit` for AddCurrency); `Server/Account/src/Handlers/QuestHandlers.hpp:567-577` (claim cache-hit) — no `RecordAudit` on the cache-hit short-circuit but the original commit's `RecordAudit` from CommitProgressionScrapSpend (ProgressionHandlers.hpp:543) and any other `RecordAudit` site are bypassed too.

The audit log captures mutating action history. A cache-hit short-circuit by design re-emits no events and no audit row — that's correct (the original action is the audit). But ops investigating "did the player retry this?" has no signal: no audit row, no log message above DEBUG level. The hit-path logs are `LOG_DATA_DEBUG("Idempotent ClaimQuestReward retry: ...")` — only visible at the noisiest log level, and not surfaced into the audit table.

Most service operators won't run at DATA_DEBUG in production. A player ticket "this charge happened twice" investigator looking at the audit_log will see exactly one row (the original) and no signal that a retry was deduped — the deduping looks like the request never happened a second time. Forensically blind.

**Fix:** on the cache-hit path, write a lightweight audit row (`action="idempotent_retry"`, `actor=playerId`, `target={scoped_key}`, `before=null`, `after=null`) outside the AccountTransaction (best-effort INSERT, swallow errors). Cheap, ops-visible, no semantic concerns.

---

## LOW / OBSERVATION

### L-V4-1. Test fixture cleans `accounts WHERE id > snapshot` — `idempotency_cache` rows ride the CASCADE
`IntegrationDbFixture.hpp:64-70` only DELETEs `accounts` (with FK CASCADE) and `outbox` (no FK). idempotency_cache has FK to accounts.account_id ON DELETE CASCADE (schema.sql:412), so the rows do get cleaned. Confirmation, not bug — just worth documenting the chain in the fixture comment for future maintainers who add a tx-flush-rollback test that wants to seed idempotency_cache rows on an existing (low-watermark) account: those WON'T be cleaned by the fixture.

### L-V4-2. `IDEMPOTENCY_DEFAULT_TTL_SECONDS = 86400` (24h) still arguably short for mobile multi-day suspend
Verbatim from v3-L1. Mobile background-and-resume over a weekend / holiday > 24h misses the cache on AddCurrency, double-credits a one-time IAP grant. AddCurrency caller surface today is debug hotkey only; future IAP integration would expose. The IAP server-side receipt dedup is the actual layer of defense for that flow, so this is moot for the named risk. Carrying forward.

### L-V4-3. `H-V2-9` deterministic keys reset across daily quest reset cycles
Verbatim from v3-L3 / v2-followup-idempotency-L3. `claim:Q:accountId` is reused day-after-day for daily quests; events table writes one row per cycle (UNIQUE doesn't fire across partitions). Reducer would throw on from-genesis replay; the genesis replay path isn't wired. Carrying forward.

### L-V4-4. `FindIdempotency` doesn't side-effect delete expired rows
Verbatim from v3-L4. Filter `expires_at > now()` hides expired rows; the sweeper (C-V4-1, currently broken) is the only delete path. Carrying forward with the now-stronger implication that as long as C-V4-1 is open, expired rows are visible only via direct SQL — they pile up forever from the application layer's perspective.

### L-V4-5. Sibling key direction-blindness paragraph (`QuestHandlers.hpp:480-491`) is 13 lines of doc-comment in the middle of a 78-line lambda
Doc-comment for a future hypothetical contributor is great; placement in the middle of `emitDelta`'s body makes the function feel longer than it is. Hoist it above the lambda or extract to a class-level doc-comment.

### L-V4-6. `payload.idempotency_key = payload.idempotency_key or uuid4()` in `NetworkTCP.claimQuestReward` (network_tcp.lua:1864) is the only client-side caller that does the right thing (mint once at outer scope, reuse across executeWithRetry attempts)
Documenting the pattern as the canonical client-side example. The other 7 wrappers (per H-V4-1) should follow this shape if/when they wrap themselves in executeWithRetry.

### L-V4-7. Per-RPC `LOG_DATA_DEBUG("Idempotent X retry: player={} key={}", playerId, clientKey)` leaks the full client key into logs
All 7 cache-hit sites. If client key is a UUID (the in-tree default), leaking it is benign. If a future client embeds structured info (a session token fragment, a purchase receipt ID), the log line becomes a leak surface. Prefix-truncate to first 8 chars on the log path, same shape as v2-M-V2-14's token-prefix concern.

### L-V4-8. The schema.sql comment at line 408 claiming "in-process sweeper... runs every minute" should be updated to reality once C-V4-1 is fixed
Either to "every 65 seconds" or whatever the chosen cadence ends up being. Currently misleading.

---

## Verified Closed from v3

The following v3 items are verified closed in code by direct trace through the current sources:

- **H-V2-9 (deterministic sibling keys)** — `QuestHandlers.hpp:457` (`claim:` + questId + `:` + accountId), `:492` (`+ ":wallet:" + cidStr`), `:534` (`+ ":progression"`). Confirmed unchanged from v3.
- **C7-A (snapshot timing)** — verified by trace. Every event-sourced handler calls `m_ctx.repository->Begin(account)` BEFORE the first mutation. Pull / MultiPull verified per v3-C-V3-2 fix (Begin at GachaHandlers.hpp:124 and :369, both precede the lazy-mutating GetPity / GetGuarantee). Claim verified at QuestHandlers.hpp:605 before any mutation. AddCurrency at AccountHandlers.hpp:398 before the wallet write. Progression handlers at ProgressionHandlers.hpp:184/273/365/455, all before SpendScrap.
- **C7-C (wire format)** — confirmed via v3, no shape change in v4 sweep.
- **M-V2-7 (point-in-time semantics)** — doc exists at AccountTransaction.hpp:114-126. Promoted to H-V4-2 because the doc enumeration is incomplete; the contract itself is correctly documented.
- **H-V2-6 (client-side threading)** — wired through ProgressionService and gacha_actions. Promoted to H-V4-1 because the threading is correct but the fallback behavior masks the real callsite contract.
- **H-V3-12 (claim partition-local dedup documented)** — QuestHandlers.hpp:320-333 has the partition-local-vs-idempotency_cache-cross-time clarification, matching what v3-H-V3-12 specified.
- **M-V3-4 (sibling key direction)** — documented at QuestHandlers.hpp:478-492. Promoted to M-V4-4 because the doc has no test backstop.
- **H-V3-5 (idempotency_cache sweeper SweepExpiredIdempotency every 60s)** — implementation exists at OutboxRelay.hpp:129-136, BUT see C-V4-1 — the OutboxRelay class is never instantiated in production. Fix is dead code in the deployable artifact.
- **H-V3-4 (outbox prune)** — same story: `PruneDispatchedOutbox` exists at OutboxRelay.hpp:112-123, dead in production. See C-V4-1.
- **H-V3-2 (advisory lock at EnsureOpen)** — verified at AccountTransaction.hpp:390-398 (`AcquireAdvisoryLockInTx` called once per commit). M-V2-5 moved out of AppendInTx as v3 specified; AppendInTx's old advisory-lock call removed (EventStore.hpp:51-101 confirms it's gone, comment at :29-46 references the move). Closed.

---

## Focus-question summary table

| # | Focus | Verdict |
|---|---|---|
| 1 | Client idempotencyKey wiring (network_tcp.lua → server) | Verified threaded through all 7 mutating wrappers; the `or uuid4()` fallback is the same gap as v3-M1. Promoted to H-V4-1 because the inventory rework will land soon. |
| 2 | Per-RPC scope prefix discipline | 8 prefixes (`pull`, `multi_pull`, `admin_grant`, `claim`, `level_char`, `ascend_char`, `level_wpn`, `ascend_wpn`) all distinct; no collisions possible at the cache PK layer. `admin_grant` mislabeling is M-V4-3. |
| 3 | Partition-local vs cross-partition dedup | Documented at QuestHandlers.hpp:320-333 per v3-H-V3-12. No new defects. |
| 4 | Sibling key construction | `:wallet:<currency>` and `:progression` suffixes confirmed at QuestHandlers.hpp:492/534. Direction-blindness invariant is doc-only (M-V4-4). |
| 5 | TTL on idempotency_cache (24h default) | Constant correct at AccountTransaction.hpp:107. Effective TTL is unbounded due to C-V4-1 (no sweep). |
| 6 | ON CONFLICT DO UPDATE semantics | Verified at AccountTransaction.hpp:247-261. `DO UPDATE` refreshes payload + expires_at + created_at. Correct for single-instance; M-V4-2 horizontal-scale concern carried from v3-M3. |
| 7 | Sweeper correctness + cadence | **Critical: sweeper logic is correct but never runs.** OutboxRelay class implemented, never instantiated. C-V4-1. Cadence drift is M-V4-1. |
| 8 | M-V2-7 point-in-time response cache breadth | Wallet-only doc; full surface is 14 fields per pull, 16 per claim, full StateResponse for AddCurrency. H-V4-2. |

---

## Suggested triage order

**Today / immediate (before next deploy):**
1. **C-V4-1** — wire `OutboxRelay` into `AccountServer`. ~20 LoC including member declaration + Stop() in dtor + handler registration if downstream destinations need them. Unblocks the entire v3-H-V3-4/5 closure and the partition-rollover failure 13 months out.
2. **H-V4-3 (#1, #3)** — add `IdempotencyCacheRoundTripTest` + `IdempotencyCacheExpirySweepTest` (the latter doubles as C-V4-1 regression). ~80 LoC.

**This week:**
3. **H-V4-2** — change AddCurrency to cache minimal ack + force client refresh, OR enumerate the field list in the doc-comment. Document-first is acceptable as long as M-V4-7 / H-V4-3 push for actual coverage.
4. **H-V4-1** — drop client `or uuid4()` fallbacks for the high-risk wrappers (AddCurrency at least). Decision falls naturally out of the F3/Inventory rework planning.

**Before launch:**
5. **M-V4-2** — switch `ON CONFLICT` policy to `DO UPDATE ... WHERE expires_at < now()` if horizontal scale is on the roadmap; otherwise document the single-instance contract more loudly.
6. **M-V4-5** — centralize the type-safe key pull in `IdempotencyKey::Scoped(prefix, request)` helper.
7. **M-V4-3** — plan the `admin_grant` → `grant` cache prefix rotation for the next coordinated downtime.
8. **M-V4-4** — sibling-key direction-blindness test (50-line claim-collision integration).
9. **M-V4-7** — write an `idempotent_retry` audit row on the cache-hit short-circuit path.

**Pre-launch quality bar (cosmetic):**
10. L-V4-* — schema doc updates (L-V4-8), log-key-prefix truncation (L-V4-7), doc-comment hoisting (L-V4-5), client-side max-bytes assert (M-V4-6 / L-V4-2).

---

## Sweep methodology notes

- Read v3 synthesis + v3 per-dimension idempotency follow-up + v2 synthesis (orchestrator-mandated).
- Traced every `IdempotencyKey::Scoped` call site (8 hits, all in Account handlers).
- Traced every `StoreIdempotency` call site (matches per-handler shape; 7 production callers + 1 helper CommitProgressionScrapSpend).
- Traced `FindIdempotency` (1 declaration in AccountRepository.hpp; 8 call sites in handlers).
- Traced `OutboxRelay` references across `Server/` — found 0 production instantiations.
- Cross-checked `AccountServer::AccountServer` ctor body + `main.cpp` for any missed wiring — none.
- Searched `tests/**` for any test exercising the idempotency cache lookup or `SweepExpiredIdempotency` — none.
- Verified client wiring through `network_tcp.lua:1205-1447` + `ProgressionService.lua:134-167` + `gacha_actions.lua:149-169`. No UI caller yet.
- Spot-checked `QuestHandlers.hpp:457,492,534` to confirm sibling-key construction matches the v3 closure narrative.

Read-only audit. Zero source modifications.

# aws-cli responses the fake replays

The fake `aws` (one folder up) does not invent output. It replays these files,
overriding only the per-object fields: `ETag`, `ContentLength`, `Metadata`,
`ContentType` and `CacheControl` for head-object and get-object; `ETag` for
put-object; `Key`, `ETag` and `Size` for each `Contents` entry and `Prefix`
for list-objects-v2. Everything else, including field order, the extra fields
and the error texts, is the file's.

| File | The call |
|---|---|
| `put-object.stdout.json` | `put-object` that succeeds |
| `get-object.stdout.json` | `get-object KEY OUTFILE` that succeeds (the body goes to OUTFILE) |
| `head-object.stdout.json` | `head-object` of an object uploaded with `--metadata sha256=...` |
| `list-objects-v2.stdout.json` | `list-objects-v2 --prefix P` with at least one match (`Contents[0]` is the entry template) |
| `list-objects-v2-empty.stdout.json` | `list-objects-v2 --prefix P` with no match |
| `put-object-precondition-failed.stderr` | `put-object --if-none-match '*'` over an existing key, or `--if-match` with a stale ETag |
| `get-object-no-such-key.stderr` | `get-object` of a missing key |
| `head-object-not-found.stderr` | `head-object` of a missing key |
| `put-object-internal-error.stderr` | a failed `put-object` (synthetic: R2 can't be made to fail on demand) |

Every error exits **254**, aws-cli v2's code for an error the service
returned.

**Recorded from R2** on 2026-10-13 with aws-cli 2.37.13 (the pinned version),
by `scripts/release/tests/record-r2-responses.sh` against `arcane-downloads`
(all but the synthetic internal error, written in the same format). Real
aws-cli differs from its documentation in ways a hand-written fake would have
missed: errors start with `aws: [ERROR]: ` and end with a period, put-object
also returns `ChecksumCRC64NVME` and `VersionId`, and list-objects-v2 times
carry microseconds. Record again after an aws-cli bump: with the R2 token in
the environment, run the script, review the diff, run
`scripts/release/tests/test-r2.sh` and commit.

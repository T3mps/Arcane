# Releasing Arcane

How an engine release reaches `downloads.starworks.dev` (design:
`docs/superpowers/specs/2026-10-12-arcane-distribution-design.md`; plan:
`docs/superpowers/plans/2026-10-12-arcane-distribution-phase2-pipeline.md`).

## Cutting a release

1. Set the version in `Arcane/Version.hpp` (`kVersionMajor`, `kVersionMinor`,
   `kVersionPatch`, `kVersionPrerelease`: `""` for stable, `"preview.N"` for
   a preview).
2. Write `docs/releases/<version>.md` (see `docs/releases/README.md`).
3. Merge both to `main`.
4. Tag and push it:

   ```bash
   git tag -a v0.3.1 -m "Arcane 0.3.1"
   git push origin v0.3.1
   ```
5. In Actions → release, approve the `release` environment's jobs (signing,
   then publishing). The run builds, signs, packs, uploads, signs
   `releases.json`, creates the GitHub Release and rebuilds the website.

Everything fails closed. A tag that doesn't match `Version.hpp`, missing
notes, an unpublished CI image, a failed test, signing or signature check, an
object that already exists with different content, or a manifest that doesn't
verify each stops the run before `releases.json` changes. The publish job
verifies the current manifest and signs the new one before it uploads
anything, so a wrong key or a broken manifest uploads nothing. Archives
uploaded before a later failure stay and are never overwritten; the job
summary lists them.

**Re-running.** Use **Re-run failed jobs**, never **Re-run all jobs**: a fresh
signing gives the Windows binaries new bytes, and their archive names are
already taken. A re-run of the publish job is safe at any point:
- identical archives already uploaded are accepted;
- a version already in `releases.json` with the same archives is a no-op, so
  the run goes on to the GitHub Release and the website rebuild (both also
  skip what's already done);
- a manifest serial an interrupted run left in `history/` is skipped (the
  next manifest takes a higher serial; Hubs only need serials to rise).

If the run says `releases.json` was not written for a reason other than
PreconditionFailed, run manifest-admin with `op=repair` first. Two runs that
write the manifest at once (a release and the monthly re-sign) don't wait for
each other: the later one fails without overwriting anything and says to
re-run it.

`released_at` is when the run started. The archive's LICENSE (phase 1) and the
manifest share it, and it is never rewritten (spec s12).

## Dry runs and rehearsals

Actions → release → Run workflow:
- **dry-run** (any branch): builds a fixture, packs it and prints the manifest
  entry it would publish. No secrets, no upload.
- **rehearsal** (main only): the same fixture, really signed with Artifact
  Signing, published to `downloads.starworks.dev/staging/` with the TEST
  manifest key. Use it after changing the pipeline or the hosting.

## Maintaining the manifest

`releases.json` expires 90 days after it is signed, and Hubs refuse an
expired manifest (installed engines keep working; installs and updates
stop). On the 1st of each month `manifest-admin` re-signs it. **Approve that
run.** Two missed months are still survivable; three are not.

GitHub disables scheduled workflows in a public repository after 60 days
without repository activity. If Arcane goes quiet that long, re-enable
manifest-admin (Actions → manifest-admin → Enable) or run it by hand.

By hand (Actions → manifest-admin → Run workflow, from `main`):
- **resign**: sign again now.
- **yank** (version and reason): marks it yanked. The archives stay, so
  projects pinned to it still install it; the Hub and the website show the
  reason. `released_at` is never touched.
- **init**: writes serial 1. For the first publish to a target only; it
  refuses if a manifest exists.
- **repair**: if `releases.json` and its `.minisig` disagree (a publish failed
  between the two writes, and the run says so), this restores both from the
  newest `history/<serial>.json` pair that verifies. Then re-run whatever
  failed.

Every serial is also stored, immutably, at `history/<serial>.json(.minisig)`:
a record of every manifest ever published.

## Keys

- **Manifest key** `arcane-manifest-1`: signs `releases.json`. Secret:
  `MINISIGN_SECRET_KEY` + `MINISIGN_PASSWORD` in the `release` environment.
  Its public half is `ArcaneHub/crates/arcane_release/keys/` and
  `keys::TRUSTED_MANIFEST_KEYS`, built into every Hub.
- **Tauri updater key** `arcane-hub-updater`: signs Hub updates (phase 4).
  Secret: `TAURI_SIGNING_PRIVATE_KEY` + `_PASSWORD`.
- Both have offline backups (password manager + offline drive). Losing either
  strands installed Hubs (spec s12).

**Rotating the manifest key:**
1. Generate `arcane-manifest-2`.
2. Add it to `TRUSTED_MANIFEST_KEYS` beside key 1.
3. Ship a Hub release.
4. Wait until the Hub's `min_supported` is at or past that release.
5. Switch the `release` secrets to key 2.
6. Remove key 1 in a later Hub release.

Never put a test key in the trusted list (a test enforces this).

## Hosting

- Bucket `arcane-downloads`, served only at `https://downloads.starworks.dev/`
  (r2.dev disabled).
- Cache Rules on starworks.dev: `.json` and `.minisig` bypass the cache (a
  cached manifest beside a fresh signature, or the reverse, would fail
  verification); archives are cached, immutable.
- `staging/` holds rehearsals, signed with the test key no Hub trusts.
- `history/<serial>.json(.minisig)` holds every manifest ever published.
- The R2 token can read and write `arcane-downloads` only. CI uploads through
  the S3 API (aws-cli, pinned in `scripts/release/install-aws-cli.sh`), never
  Wrangler, whose `r2 object put` caps objects at 315 MB.
- Check the setup after a rehearsal: `bash scripts/release/check-hosting.sh`.

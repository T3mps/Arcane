# Release notes

One file per release, named for the version without the `v`:
`0.3.1.md` or `0.4.0-preview.2.md`. It must exist at the tag; the release
workflow fails without it. The same text becomes the GitHub Release body and
the `notes` field in `releases.json`, which the Hub and
starworks.dev/arcane/releases show. Write for users: what changed, what to
watch for when upgrading, and whether the ABI changed.

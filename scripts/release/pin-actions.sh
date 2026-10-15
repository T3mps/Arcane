#!/usr/bin/env bash
# pin-actions.sh FILE... -- rewrites each `uses: owner/repo[/path]@ref` that is
# not already a commit SHA to `uses: owner/repo[/path]@<sha> # <ref>`. Needs an
# authenticated gh. Run it after adding or bumping an action; check-pinned.sh
# enforces the result in CI.
set -euo pipefail
for f in "$@"; do
    for ref in $(sed -n 's/^[[:space:]]*-\{0,1\}[[:space:]]*uses:[[:space:]]*\([^[:space:]#]*\).*/\1/p' "$f" | sort -u); do
        case $ref in ./*) continue ;; esac
        action=${ref%@*} tag=${ref#*@}
        [[ $tag =~ ^[0-9a-f]{40}$ ]] && continue
        repo=$(cut -d/ -f1-2 <<<"$action")
        sha=$(gh api "repos/$repo/commits/$tag" --jq .sha)
        [[ $sha =~ ^[0-9a-f]{40}$ ]] || { echo "pin-actions: could not resolve $ref" >&2; exit 1; }
        sed -i "s|uses:[[:space:]]*$ref[[:space:]]*\(#.*\)\{0,1\}\$|uses: $action@$sha # $tag|" "$f"
        echo "$f: $ref -> $sha"
    done
done

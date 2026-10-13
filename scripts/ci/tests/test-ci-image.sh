#!/usr/bin/env bash
# Tests for scripts/ci/ci-image.sh with a stub `docker` on PATH (no daemon,
# no registry): a published tag is reused, a missing one is built + pushed +
# verified on a same-repo run, and refused -- never built -- on a fork PR.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fails=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1" >&2; fails=$((fails + 1)); }
DIGEST=sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa

# STATE dir: calls.txt records every docker call; "published" exists once the
# image is in the (fake) registry. BAD_DIGEST=1 makes inspect print junk.
mkstub() {
    local d=$1
    mkdir -p "$d/bin"
    cat > "$d/bin/docker" <<EOF
#!/bin/sh
echo "docker \$*" >> "$d/calls.txt"
case "\$*" in
  "buildx imagetools inspect"*)
    [ -f "$d/published" ] || exit 1
    case "\$*" in *--format*)
      if [ -n "\${BAD_DIGEST:-}" ]; then echo '{"mediaType":"x"}'
      else echo '{"mediaType":"application/vnd.oci.image.manifest.v1+json","digest":"$DIGEST","size":1234}'; fi ;;
    esac
    exit 0 ;;
  "buildx build"*) touch "$d/published"; exit 0 ;;
  run*) exit 0 ;;
esac
exit 0
EOF
    chmod +x "$d/bin/docker"
}
run() {   # run <state> [VAR=value...]
    local d=$1; shift
    : > "$d/calls.txt"; : > "$d/out.txt"
    set +e
    env -u SAME_REPO -u GITHUB_OUTPUT PATH="$d/bin:$PATH" GITHUB_OUTPUT="$d/gh_output" "$@" \
        "$REPO/scripts/ci/ci-image.sh" ghcr.io/starworksdev/arcane-ci build-0123456789abcdef > "$d/out.txt" 2>&1
    RC=$?
    set -e
}

d=$(mktemp -d); mkstub "$d"; touch "$d/published"; run "$d" SAME_REPO=true
[ "$RC" -eq 0 ] && ! grep -q "buildx build" "$d/calls.txt" && grep -qx "digest=$DIGEST" "$d/gh_output" \
    && ok "published tag reused, digest output" || bad "published tag reused (rc $RC): $(cat "$d/out.txt")"

d=$(mktemp -d); mkstub "$d"; run "$d" SAME_REPO=true
grep -q "buildx build --target build --provenance=false --push" "$d/calls.txt" \
    && grep -q -- "--build-context scripts=$REPO/scripts" "$d/calls.txt" \
    && grep -q "verify-image.sh" "$d/calls.txt" && [ "$RC" -eq 0 ] && grep -qx "digest=$DIGEST" "$d/gh_output" \
    && ok "missing tag built, pushed, verified" || bad "missing tag built (rc $RC): $(cat "$d/calls.txt" "$d/out.txt")"

d=$(mktemp -d); mkstub "$d"; run "$d" SAME_REPO=false
[ "$RC" -eq 1 ] && ! grep -q "buildx build" "$d/calls.txt" && grep -q "build-0123456789abcdef" "$d/out.txt" \
    && ok "fork PR refused, names the tag, builds nothing" || bad "fork PR refused (rc $RC): $(cat "$d/calls.txt" "$d/out.txt")"

d=$(mktemp -d); mkstub "$d"; run "$d"
[ "$RC" -eq 1 ] && ! grep -q "buildx build" "$d/calls.txt" && ok "unset SAME_REPO counts as a fork" || bad "unset SAME_REPO counts as a fork (rc $RC)"

d=$(mktemp -d); mkstub "$d"; touch "$d/published"; run "$d" SAME_REPO=true BAD_DIGEST=1
[ "$RC" -eq 1 ] && [ ! -s "$d/gh_output" ] && ok "unreadable digest fails" || bad "unreadable digest fails (rc $RC)"

echo
[ "$fails" -eq 0 ] && echo "test-ci-image: all passed" || echo "test-ci-image: $fails failure(s)"
[ "$fails" -eq 0 ]

#!/usr/bin/env bash
# Tests for scripts/ci/image-tag.sh, and its twin image-tag.ps1 when pwsh (or
# Windows PowerShell, on the desk) is on PATH -- spec s3.1. Runs in the
# image-tag job of .github/workflows/ci.yml and on the desk (Git Bash).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
TAG="$REPO/scripts/ci/image-tag.sh"
PS=""
if command -v pwsh > /dev/null 2>&1; then PS=pwsh
elif command -v powershell.exe > /dev/null 2>&1; then PS=powershell.exe; fi
fails=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1" >&2; fails=$((fails + 1)); }

inputs() { tr -d '\r' < "$REPO/ci/linux/image-inputs.txt" | sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e '/^$/d'; }
# fixture: a temp root holding copies of the real inputs.
fixture() {
    local d f
    d=$(mktemp -d)
    for f in $(inputs); do mkdir -p "$d/$(dirname "$f")"; cp "$REPO/$f" "$d/$f"; done
    echo "$d"
}
tag_of() { "$TAG" --root "$1" 2> /dev/null; }
ps_tag_of() {
    local root=$1
    if [ "$PS" = powershell.exe ]; then root=$(cygpath -w "$root"); fi
    "$PS" -NoProfile -ExecutionPolicy Bypass -File "$REPO/scripts/ci/image-tag.ps1" -Root "$root" 2> /dev/null | tr -d '\r'
}

real=$("$TAG")
case "$real" in build-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ok "format: $real" ;; *) bad "format: '$real'" ;; esac
[ "$("$TAG")" = "$real" ] && ok "stable across runs" || bad "stable across runs"

base=$(fixture)
[ "$(tag_of "$base")" = "$real" ] && ok "fixture copy = repo" || bad "fixture copy = repo"

for f in $(inputs); do
    d=$(fixture); echo "# edit" >> "$d/$f"
    t=$(tag_of "$d" || true)
    [ -n "$t" ] && [ "$t" != "$real" ] && ok "edit changes the tag: $f" || bad "edit changes the tag: $f ($t)"
done

d=$(fixture)
for f in $(inputs); do sed 's/$/\r/' "$d/$f" > "$d/$f.crlf" && mv "$d/$f.crlf" "$d/$f"; done
[ "$(tag_of "$d")" = "$real" ] && ok "CRLF-only change keeps the tag" || bad "CRLF-only change keeps the tag"
crlf=$d

d=$(fixture); echo 'COPY --from=scripts extra.sh /tmp/ci/scripts/' >> "$d/ci/linux/Dockerfile"
if out=$("$TAG" --root "$d" 2>&1); then bad "unlisted COPY refused"; else
    case "$out" in *extra.sh*) ok "unlisted COPY refused" ;; *) bad "unlisted COPY refused, message: $out" ;; esac; fi

d=$(fixture); echo 'scripts/run-tests.sh' >> "$d/ci/linux/image-inputs.txt"; mkdir -p "$d/scripts"; cp "$REPO/scripts/run-tests.sh" "$d/scripts/"
if out=$("$TAG" --root "$d" 2>&1); then bad "listed but not copied refused"; else
    case "$out" in *run-tests.sh*) ok "listed but not copied refused" ;; *) bad "listed but not copied, message: $out" ;; esac; fi

d=$(fixture); echo 'ADD https://example.com/x /x' >> "$d/ci/linux/Dockerfile"
"$TAG" --root "$d" > /dev/null 2>&1 && bad "ADD refused" || ok "ADD refused"

d=$(fixture); echo 'COPY somefile /x' >> "$d/ci/linux/Dockerfile"
"$TAG" --root "$d" > /dev/null 2>&1 && bad "COPY from the context refused" || ok "COPY from the context refused"

d=$(fixture); printf 'COPY --from=scripts build-sdl3.sh \\\n    /tmp/x/\n' >> "$d/ci/linux/Dockerfile"
"$TAG" --root "$d" > /dev/null 2>&1 && bad "continued COPY refused" || ok "continued COPY refused"

d=$(fixture); printf 'FROM build AS runtime\nCOPY --from=build /usr/local/lib/libSDL3.so.0 /usr/lib/\n' >> "$d/ci/linux/Dockerfile"
"$TAG" --root "$d" > /dev/null 2>&1 && ok "COPY from a declared stage allowed" || bad "COPY from a declared stage allowed"

d=$(fixture); rm "$d/scripts/sha256-check.sh"
"$TAG" --root "$d" > /dev/null 2>&1 && bad "missing input refused" || ok "missing input refused"

if git -C "$REPO" rev-parse --git-dir > /dev/null 2>&1; then
    case "$(git -C "$REPO" check-attr eol -- ci/linux/Dockerfile)" in *": eol: lf") ok ".gitattributes: Dockerfile eol=lf" ;; *) bad ".gitattributes: Dockerfile eol=lf" ;; esac
    case "$(git -C "$REPO" check-attr eol -- scripts/ci/image-tag.sh)" in *": eol: lf") ok ".gitattributes: *.sh eol=lf" ;; *) bad ".gitattributes: *.sh eol=lf" ;; esac
else
    echo "skip .gitattributes checks (no git checkout)"
fi

if [ -n "$PS" ]; then
    [ "$(ps_tag_of "$REPO")" = "$real" ] && ok "image-tag.ps1 = image-tag.sh (repo)" || bad "image-tag.ps1 = image-tag.sh (repo): $(ps_tag_of "$REPO")"
    [ "$(ps_tag_of "$crlf")" = "$real" ] && ok "image-tag.ps1 = image-tag.sh (CRLF copy)" || bad "image-tag.ps1 = image-tag.sh (CRLF copy)"
    d=$(fixture); echo 'COPY --from=scripts extra.sh /tmp/ci/scripts/' >> "$d/ci/linux/Dockerfile"
    [ -z "$(ps_tag_of "$d")" ] && ok "image-tag.ps1 refuses an unlisted COPY" || bad "image-tag.ps1 refuses an unlisted COPY"
else
    echo "skip image-tag.ps1 checks (no pwsh / powershell.exe)"
fi

echo
[ "$fails" -eq 0 ] && echo "test-image-tag: all passed" || echo "test-image-tag: $fails failure(s)"
[ "$fails" -eq 0 ]

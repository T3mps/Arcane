#!/bin/sh
# image-tag.sh [--root DIR] -- prints the Linux CI image tag, build-<16 hex>:
# the first 16 hex digits of SHA-256 over every input listed in
# ci/linux/image-inputs.txt, in that order, each as <path> NUL <bytes with
# every CR removed> NUL (so a CRLF checkout hashes like an LF one). The one
# definition of "which image" -- ci.yml's image-tag job and the desk use it;
# scripts/ci/image-tag.ps1 is the byte-for-byte PowerShell twin. Image:
# ghcr.io/starworksdev/arcane-ci:<tag>.
#
# The list is checked, not trusted: every repo file the Dockerfile copies
# must be listed, and every listed scripts/ file must be copied. Repo files
# may only enter through ONE-LINE `COPY --from=scripts <file>... <dest>`;
# ADD, a continued COPY, and a COPY from the build context are refused
# (`COPY --from=<a stage declared with FROM ... AS>` is fine).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
if [ "${1:-}" = "--root" ]; then ROOT=$(cd "${2:?--root needs a directory}" && pwd); fi
LIST="$ROOT/ci/linux/image-inputs.txt"
DOCKERFILE="$ROOT/ci/linux/Dockerfile"
fail() { echo "image-tag: $*" >&2; exit 1; }
[ -f "$LIST" ] || fail "missing $LIST"
[ -f "$DOCKERFILE" ] || fail "missing $DOCKERFILE"

inputs=$(tr -d '\r' < "$LIST" | sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e '/^$/d')

refs=$(tr -d '\r' < "$DOCKERFILE" | awk '
    /^[[:space:]]*#/ { next }
    toupper($1) == "FROM" { for (i = 2; i < NF; i++) if (toupper($i) == "AS") stage[$(i + 1)] = 1; next }
    toupper($1) == "ADD"  { print "!ADD " $0; next }
    toupper($1) == "COPY" {
        if ($NF == "\\") { print "!CONT " $0; next }
        from = ""; n = 0
        for (i = 2; i <= NF; i++) {
            if (substr($i, 1, 2) == "--") { if (substr($i, 1, 7) == "--from=") from = substr($i, 8); continue }
            arg[++n] = $i
        }
        if (from != "" && (from in stage)) next
        if (from != "scripts") { print "!COPY " $0; next }
        for (i = 1; i < n; i++) print "scripts/" arg[i]
    }')

bad=$(printf '%s\n' "$refs" | grep '^!' | head -n 1 || true)
case "$bad" in
    '') ;;
    '!ADD '*)  fail "ci/linux/Dockerfile uses ADD (use a one-line COPY --from=scripts): ${bad#!ADD }" ;;
    '!CONT '*) fail "a COPY in ci/linux/Dockerfile continues onto the next line (keep it on one line): ${bad#!CONT }" ;;
    *)         fail "every repo file must come in as COPY --from=scripts <file>... <dest>: ${bad#!COPY }" ;;
esac

in_list() { printf '%s\n' "$2" | grep -qxF "$1"; }
for r in $refs; do
    in_list "$r" "$inputs" || fail "ci/linux/Dockerfile copies $r, which is not in ci/linux/image-inputs.txt"
done
in_list ci/linux/Dockerfile "$inputs" || fail "ci/linux/image-inputs.txt must list ci/linux/Dockerfile"
in_list ci/linux/image-inputs.txt "$inputs" || fail "ci/linux/image-inputs.txt must list itself"
for f in $inputs; do
    case "$f" in
        scripts/*) in_list "$f" "$refs" || fail "$f is listed in ci/linux/image-inputs.txt but ci/linux/Dockerfile does not copy it" ;;
    esac
    [ -f "$ROOT/$f" ] || fail "missing input $f"
done

hash() { if command -v sha256sum > /dev/null 2>&1; then sha256sum; else shasum -a 256; fi; }
sum=$(for f in $inputs; do printf '%s\000' "$f"; tr -d '\r' < "$ROOT/$f"; printf '\000'; done | hash | cut -c1-16)
echo "build-$sum"

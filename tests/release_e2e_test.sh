#!/usr/bin/env bash
# End-to-end run of scripts/release-macos.sh --upload in a sandbox (macOS only):
# a scratch clone tagged at HEAD, a local stand-in for the GitHub remote, and stub
# codesign / xcrun notarytool+stapler / hdiutil / spctl / gh on PATH. Nothing is ever
# really signed, notarized or uploaded: the script's test mode forces ad-hoc identity,
# and the stub gh keeps "uploads" in a directory. The real build (Xcode, universal,
# JUCE download) does run, so this takes a while.
#
#   tests/release_e2e_test.sh            # full run, plus the tag-check refusals
#   tests/release_e2e_test.sh --quick    # only the refusals that stop before the build
set -euo pipefail
[[ "$(uname -s)" == Darwin ]] || { echo "macOS only"; exit 0; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
VERSION="$(sed -n 's/^project(FlayrTune VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
TAG="v$VERSION"

git clone --quiet --no-local "$ROOT" "$T/repo"
git -C "$T/repo" tag -f "$TAG" HEAD >/dev/null
SHA="$(git -C "$T/repo" rev-parse HEAD)"
git init --quiet --bare "$T/remote.git"

mkdir -p "$T/stubs" "$T/released" "$T/fixtures"
printf 'windows installer from CI\n' > "$T/fixtures/FlayrTune-$VERSION-Windows-Setup.exe"
printf 'linux tarball from CI\n' > "$T/fixtures/FlayrTune-$VERSION-Linux-x64.tar.gz"

for tool in codesign spctl; do
    printf '#!/bin/sh\necho "%s $*" >> "$FLAYR_E2E_LOG"\nexit 0\n' "$tool" > "$T/stubs/$tool"
done
cat > "$T/stubs/xcrun" <<'EOF'
#!/bin/sh
echo "xcrun $*" >> "$FLAYR_E2E_LOG"
case "$1" in notarytool|stapler) exit 0 ;; esac
exec /usr/bin/xcrun "$@"
EOF
cat > "$T/stubs/hdiutil" <<'EOF'
#!/bin/sh
echo "hdiutil $*" >> "$FLAYR_E2E_LOG"
for last; do :; done
echo "stub dmg" > "$last"
EOF
# The stub gh sees only the sealed environment plus FLAYR_TEST_* (the script's test hook).
cat > "$T/stubs/gh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
D="$FLAYR_TEST_DIR"
echo "gh $*" >> "$D/e2e.log"
arg() { local w="$1"; shift; while [[ $# -gt 0 ]]; do [[ "$1" == "$w" ]] && { echo "$2"; return; }; shift; done; }
assets() { # CI fixtures + whatever was "uploaded", with digests
    local first=1; printf '['
    for f in "$D"/fixtures/* "$D"/released/*; do
        [[ -f "$f" ]] || continue
        [[ $first == 1 ]] || printf ','; first=0
        printf '{"name":"%s","digest":"sha256:%s"}' "$(basename "$f")" "$(shasum -a 256 "$f" | cut -d' ' -f1)"
    done
    printf ']\n'
}
case "$1 $2" in
    "attestation verify")
        if [[ "${3:-}" == --help ]]; then echo "--signer-workflow --source-ref --source-digest --deny-self-hosted-runners --cert-oidc-issuer --format"; exit 0; fi
        f="$3"
        echo "[{\"verificationResult\":{\"signature\":{\"certificate\":{\"issuer\":\"https://token.actions.githubusercontent.com\",
          \"sourceRepositoryDigest\":\"$FLAYR_TEST_SHA\",\"sourceRepositoryRef\":\"refs/tags/$FLAYR_TEST_TAG\",\"runnerEnvironment\":\"github-hosted\",
          \"githubWorkflowTrigger\":\"push\",\"runInvocationURI\":\"https://github.com/FlayrLabs/flayr-tune/actions/runs/77/attempts/1\"}},
          \"statement\":{\"subject\":[{\"name\":\"$(basename "$f")\",\"digest\":{\"sha256\":\"$(shasum -a 256 "$f" | cut -d' ' -f1)\"}}]}}}]" ;;
    "run list")
        echo "[{\"databaseId\":77,\"headSha\":\"$FLAYR_TEST_SHA\",\"headBranch\":\"$FLAYR_TEST_TAG\",\"status\":\"completed\",\"conclusion\":\"success\"}]" ;;
    "run download")
        dir="$(arg --dir "$@")"; mkdir -p "$dir/ci"; cp "$D"/fixtures/* "$dir/ci/" ;;
    "release view")
        if [[ "$*" == *isDraft* ]]; then echo true; else assets; fi ;;
    "release download")
        dir="$(arg --dir "$@")"; p="$(arg --pattern "$@")"
        if [[ -f "$D/fixtures/$p" ]]; then cp "$D/fixtures/$p" "$dir/"; else cp "$D/released/$p" "$dir/"; fi ;;
    "release upload")
        shift 3; while [[ $# -gt 0 ]]; do case "$1" in --*) shift ;; *) [[ -f "$1" ]] && cp "$1" "$D/released/"; shift ;; esac; done ;;
    *) echo "stub gh: unexpected $*" >&2; exit 3 ;;
esac
EOF
chmod +x "$T/stubs/"*

failures=0
release() { # runs the release script in the sandbox; extra env first
    (cd "$T/repo" && env -u CMAKE_GENERATOR -u CMAKE_BUILD_TYPE "$@" FLAYR_RELEASE_TEST=1 \
        FLAYR_RELEASE_TEST_REMOTE="file://$T/remote.git" FLAYR_E2E_LOG="$T/e2e.log" \
        FLAYR_TEST_DIR="$T" FLAYR_TEST_SHA="$SHA" FLAYR_TEST_TAG="$TAG" \
        PATH="$T/stubs:/usr/bin:/bin:/usr/sbin:/sbin:$(dirname "$(command -v cmake)"):$(dirname "$(command -v jq)")" \
        scripts/release-macos.sh --upload)
}
expect_refusal() {
    local what="$1" pattern="$2"
    if out="$(release 2>&1)"; then
        echo "  [FAIL] $what: the script went ahead"; failures=$((failures + 1))
    elif grep -q -- "$pattern" <<< "$out"; then
        echo "  [PASS] $what: refused ($(grep -- "$pattern" <<< "$out" | tail -1))"
    else
        echo "  [FAIL] $what: refused for another reason:"; tail -5 <<< "$out"; failures=$((failures + 1))
    fi
}

echo "GitHub tag check (ls-remote)"
expect_refusal "tag missing on the remote" "differs from the local tag"
# ls-remote patterns match suffixes: a branch ending in refs/tags/<tag> must not count.
git -C "$T/repo" push --quiet "$T/remote.git" "HEAD:refs/heads/x/refs/tags/$TAG"
expect_refusal "only a branch named x/refs/tags/$TAG" "differs from the local tag"
git -C "$T/remote.git" update-ref "refs/tags/$TAG" "$(git -C "$T/repo" rev-parse HEAD~1 2>/dev/null || echo "$SHA")"
if [[ "$(git -C "$T/remote.git" rev-parse "refs/tags/$TAG")" != "$SHA" ]]; then
    expect_refusal "the remote's tag at another commit" "differs from the local tag"
fi
git -C "$T/repo" push --quiet --force "$T/remote.git" "refs/tags/$TAG"

if [[ "${1:-}" == --quick ]]; then
    echo; [[ $failures -eq 0 ]] && echo "ALL PASSED (quick)" || { echo "FAILURES ($failures)"; exit 1; }
    exit 0
fi

echo "Full run with stub signing, notarization and gh"
if release > "$T/run.log" 2>&1; then
    dmg="$(ls "$T/repo/dist/"*.dmg 2>/dev/null | head -1)"
    sums="$T/released/SHA256SUMS.txt"
    if [[ -n "$dmg" && -f "$T/released/$(basename "$dmg")" && -f "$sums" ]] \
       && [[ "$(wc -l < "$sums" | tr -d ' ')" == 3 ]] \
       && grep -q "codesign --force --options runtime --timestamp --sign - " "$T/e2e.log" \
       && grep -q "xcrun notarytool submit" "$T/e2e.log" \
       && grep -q "OK: no networking" "$T/run.log" && grep -q "JUCE patch present" "$T/run.log"; then
        echo "  [PASS] completed: patches and networking checked, ad-hoc signed, DMG + SHA256SUMS.txt uploaded"
        sed 's/^/    /' "$sums"
    else
        echo "  [FAIL] finished but the results are wrong"; tail -30 "$T/run.log"; failures=$((failures + 1))
    fi
else
    echo "  [FAIL] the release script failed:"; tail -40 "$T/run.log"; failures=$((failures + 1))
fi

echo
[[ $failures -eq 0 ]] && echo "ALL PASSED" || { echo "FAILURES ($failures)"; exit 1; }

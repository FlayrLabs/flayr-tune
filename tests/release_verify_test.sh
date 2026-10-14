#!/usr/bin/env bash
# ctest: scripts/verify-release-assets.sh against a fake `gh` (no network), one passing
# scenario (which also checks gh ran sealed) and one per way an asset or its provenance can be wrong. Every failure must
# exit non-zero and leave the output directory empty (no partial SHA256SUMS.txt).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERIFY="$ROOT/scripts/verify-release-assets.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

TAG=v9.9.9 VERSION=9.9.9 SHA=0123456789abcdef0123456789abcdef01234567 RUN=4242
WIN="FlayrTune-$VERSION-Windows-Setup.exe" LIN="FlayrTune-$VERSION-Linux-x64.tar.gz"
mkdir -p "$T/fixtures" "$T/bin"
printf 'windows installer bytes\n' > "$T/fixtures/$WIN"
printf 'linux tarball bytes\n' > "$T/fixtures/$LIN"
printf 'notarized dmg bytes\n' > "$T/FlayrTune-$VERSION-macOS.dmg"
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
WIN_SHA="$(sha "$T/fixtures/$WIN")" LIN_SHA="$(sha "$T/fixtures/$LIN")"

# The fake gh: behaviour switched by $SCENARIO, every call logged to $T/calls.
cat > "$T/bin/gh" <<'FAKE'
#!/usr/bin/env bash
set -euo pipefail
echo "$*" >> "$FLAYR_TEST_T/calls"
echo "ENV PATH=$PATH CANARY=${LEAK_CANARY:-absent}" >> "$FLAYR_TEST_T/env"
arg_after() { local want="$1"; shift; while [[ $# -gt 0 ]]; do [[ "$1" == "$want" ]] && { echo "$2"; return; }; shift; done; }
case "$1 $2" in
    "attestation verify")
        if [[ "${3:-}" == "--help" ]]; then
            if [[ "$FLAYR_TEST_SCENARIO" == old-gh ]]; then echo "--signer-workflow --source-ref --format"; else
            echo "--signer-workflow --source-ref --source-digest --deny-self-hosted-runners --cert-oidc-issuer --format"; fi
            exit 0
        fi
        file="$3"; name="$(basename "$file")"; digest="$(shasum -a 256 "$file" | cut -d' ' -f1)"
        [[ "$FLAYR_TEST_SCENARIO" == attestation-fails ]] && { echo "verification failed" >&2; exit 1; }
        src="$FLAYR_TEST_SHA"; ref="refs/tags/$FLAYR_TEST_TAG"; runner=github-hosted; run="$FLAYR_TEST_RUN"; trigger=push
        case "$FLAYR_TEST_SCENARIO" in
            wrong-source-digest) src=ffffffffffffffffffffffffffffffffffffffff ;;
            wrong-ref) ref=refs/heads/main ;;
            self-hosted) runner=self-hosted ;;
            other-run) run=1 ;;
            wrong-subject) digest=0000 ;;
            workflow-dispatch) trigger=workflow_dispatch ;;
        esac
        cat <<JSON
[{"verificationResult":{"signature":{"certificate":{"issuer":"https://token.actions.githubusercontent.com",
  "sourceRepositoryDigest":"$src","sourceRepositoryRef":"$ref","runnerEnvironment":"$runner","githubWorkflowTrigger":"$trigger",
  "runInvocationURI":"https://github.com/FlayrLabs/flayr-tune/actions/runs/$run/attempts/1"}},
  "statement":{"subject":[{"name":"$name","digest":{"sha256":"$digest"}}]}}}]
JSON
        ;;
    "run list")
        sha="$FLAYR_TEST_SHA"; [[ "$FLAYR_TEST_SCENARIO" == no-run ]] && sha=ffffffffffffffffffffffffffffffffffffffff
        echo "[{\"databaseId\":$FLAYR_TEST_RUN,\"headSha\":\"$sha\",\"headBranch\":\"$FLAYR_TEST_TAG\",\"status\":\"completed\",\"conclusion\":\"success\"},
               {\"databaseId\":9999,\"headSha\":\"$FLAYR_TEST_SHA\",\"headBranch\":\"$FLAYR_TEST_TAG\",\"status\":\"completed\",\"conclusion\":\"failure\"}]"
        ;;
    "release view")
        wd="sha256:$FLAYR_TEST_WIN_SHA"; [[ "$FLAYR_TEST_SCENARIO" == release-digest-mismatch ]] && wd="sha256:0000"
        extra=""; [[ "$FLAYR_TEST_SCENARIO" == extra-asset ]] && extra=',{"name":"FlayrTune-evil.pkg","digest":"sha256:00"}'
        lin=",{\"name\":\"$FLAYR_TEST_LIN\",\"digest\":\"sha256:$FLAYR_TEST_LIN_SHA\"}"; [[ "$FLAYR_TEST_SCENARIO" == missing-asset ]] && lin=""
        echo "[{\"name\":\"$FLAYR_TEST_WIN\",\"digest\":\"$wd\"}$lin$extra]"
        ;;
    "run download")
        dir="$(arg_after --dir "$@")"; mkdir -p "$dir/windows" "$dir/linux"
        cp "$FLAYR_TEST_FIX/$FLAYR_TEST_WIN" "$dir/windows/"; cp "$FLAYR_TEST_FIX/$FLAYR_TEST_LIN" "$dir/linux/"
        [[ "$FLAYR_TEST_SCENARIO" == run-artifact-differs ]] && printf 'rebuilt elsewhere\n' > "$dir/windows/$FLAYR_TEST_WIN"
        exit 0
        ;;
    "release download")
        dir="$(arg_after --dir "$@")"; pattern="$(arg_after --pattern "$@")"
        cp "$FLAYR_TEST_FIX/$pattern" "$dir/"
        [[ "$FLAYR_TEST_SCENARIO" == swapped-download && "$pattern" == "$FLAYR_TEST_WIN" ]] && printf 'swapped\n' > "$dir/$pattern"
        exit 0
        ;;
    *) echo "fake gh: unexpected call: $*" >&2; exit 3 ;;
esac
FAKE
chmod +x "$T/bin/gh"
export FLAYR_RELEASE_TEST=1 FLAYR_TEST_T="$T" FLAYR_TEST_TAG="$TAG" FLAYR_TEST_SHA="$SHA" FLAYR_TEST_RUN="$RUN" \
       FLAYR_TEST_WIN="$WIN" FLAYR_TEST_LIN="$LIN" FLAYR_TEST_WIN_SHA="$WIN_SHA" FLAYR_TEST_LIN_SHA="$LIN_SHA" \
       FLAYR_TEST_FIX="$T/fixtures"

failures=0
run_case() {
    local scenario="$1" expect="$2" out="$T/out-$1"
    mkdir -p "$out"
    : > "$T/calls"; : > "$T/env"
    local override=()
    [[ "$scenario" == gh-host-override ]] && override=(GH_HOST=evil.example)
    if env ${override[@]+"${override[@]}"} LEAK_CANARY=leaked FLAYR_TEST_SCENARIO="$scenario" PATH="$T/bin:$PATH" "$VERIFY" "$TAG" "$SHA" "$VERSION" "$T/FlayrTune-$VERSION-macOS.dmg" "$out" \
           > "$T/log-$scenario" 2>&1; then rc=0; else rc=$?; fi
    if [[ "$expect" == pass ]]; then
        if [[ $rc -eq 0 ]] && grep -q "$WIN_SHA  $WIN" "$out/SHA256SUMS.txt" && grep -q "$LIN_SHA  $LIN" "$out/SHA256SUMS.txt" \
             && grep -q "$(sha "$T/FlayrTune-$VERSION-macOS.dmg")  FlayrTune-$VERSION-macOS.dmg" "$out/SHA256SUMS.txt" \
             && ! grep -qv "^ENV PATH=/usr/bin:/bin:/usr/sbin:/sbin CANARY=absent$" "$T/env" \
             && grep -q -- "--signer-workflow FlayrLabs/flayr-tune/.github/workflows/release.yml --source-ref refs/tags/$TAG --source-digest $SHA --deny-self-hosted-runners --cert-oidc-issuer https://token.actions.githubusercontent.com" "$T/calls"; then
            echo "  [PASS] $scenario: verified, SHA256SUMS.txt over the verified bytes, strict flags, gh sealed"
        else
            echo "  [FAIL] $scenario: expected success"; cat "$T/log-$scenario"; failures=$((failures + 1))
        fi
    else
        if [[ $rc -ne 0 && -z "$(ls -A "$out")" ]]; then
            echo "  [PASS] $scenario: refused ($(tail -1 "$T/log-$scenario"))"
        else
            echo "  [FAIL] $scenario: expected refusal with no output (rc=$rc)"; cat "$T/log-$scenario"; failures=$((failures + 1))
        fi
    fi
}

run_case ok pass
for s in gh-host-override old-gh no-run extra-asset missing-asset release-digest-mismatch swapped-download run-artifact-differs \
         attestation-fails wrong-source-digest wrong-ref self-hosted other-run wrong-subject workflow-dispatch; do
    run_case "$s" fail
done

echo
[[ $failures -eq 0 ]] && echo "ALL PASSED" || { echo "FAILURES ($failures)"; exit 1; }

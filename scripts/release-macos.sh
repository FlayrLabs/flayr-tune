#!/usr/bin/env bash
# Builds the macOS release (universal AU + VST3), signs it with a Developer ID,
# notarizes and staples it, and wraps it in a drag-to-install DMG.
#
#   scripts/release-macos.sh            # build dist/FlayrTune-<version>-macOS.dmg
#   scripts/release-macos.sh --upload   # ...and attach it, with SHA256SUMS.txt, to the draft release
#                                       # v<version> (see scripts/verify-release-assets.sh)
#
# Needs a "Developer ID Application" identity in the keychain and a notarytool
# keychain profile (xcrun notarytool store-credentials <profile> ...).
#
# Only signs a tagged release: HEAD must sit exactly on a v<version> tag that matches
# CMakeLists.txt and the same tag on GitHub. The source is not a checkout: it is
# `git archive` of the tag's commit, taken from a fresh bare copy of the repository,
# and the extracted files must hash to exactly the tag's tree before anything is built.
# Every git command runs sealed (env -i, no system or global config, no templates, no
# hooks, no fsmonitor, no attributes file), so hooks, templates, smudge filters,
# insteadOf rewrites or a modified working tree can't change what gets built. Nothing
# from the environment reaches the build either: configure and build run under env -i
# with a minimal PATH, and the script refuses to start if CMake/FetchContent/compiler
# overrides are set (CXXFLAGS=-UFLAYR_NO_NETWORK or FETCHCONTENT_SOURCE_DIR_JUCE=<unpatched
# JUCE> would otherwise reach the signed build). The result is checked for networking
# code and for every JUCE patch before anything is signed.
set -euo pipefail

IDENTITY="${IDENTITY:-Developer ID Application: Flayr Labs LLC (NRNU83UJ68)}"
REMOTE_URL="https://github.com/FlayrLabs/flayr-tune.git"
# Test mode (tests/release_e2e_test.sh): a stand-in remote instead of GitHub, with
# ad-hoc signing forced so nothing it produces can pass for a release.
if [[ "${FLAYR_RELEASE_TEST:-}" == 1 ]]; then
    echo "==> TEST MODE: remote ${FLAYR_RELEASE_TEST_REMOTE:?}, ad-hoc signing only; this is not a release" >&2
    REMOTE_URL="$FLAYR_RELEASE_TEST_REMOTE"
    IDENTITY="-"
fi
NOTARY_PROFILE="${NOTARY_PROFILE:-flayr-notary}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^project(FlayrTune VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
TAG="v$VERSION"
DIST="$ROOT/dist"
DMG="$DIST/FlayrTune-$VERSION-macOS.dmg"

die() { echo "error: $*" >&2; exit 1; }

UPLOAD=0
case "${1:-}" in
    "") ;;
    --upload) UPLOAD=1 ;;
    *) die "unknown argument $1" ;;
esac
# Fail before the long build if the upload step couldn't run.
REPO="FlayrLabs/flayr-tune" # pinned
if [[ "$UPLOAD" == 1 ]]; then
    for tool in gh jq shasum; do command -v "$tool" >/dev/null || die "$tool is needed for --upload"; done
    # gh runs sealed, as in scripts/verify-release-assets.sh: absolute path, fixed PATH,
    # HOME and token variables only; host/repo/config overrides stop the script.
    GH_BIN="$(command -v gh)"
    for v in GH_HOST GH_REPO GH_ENTERPRISE_TOKEN GITHUB_ENTERPRISE_TOKEN GH_CONFIG_DIR; do
        [[ -z "${!v:-}" ]] || die "$v is set; unset it (releases only talk to github.com/$REPO)"
    done
    gh() {
        local pass=() var
        [[ -n "${GH_TOKEN:-}" ]] && pass+=("GH_TOKEN=$GH_TOKEN")
        [[ -n "${GITHUB_TOKEN:-}" ]] && pass+=("GITHUB_TOKEN=$GITHUB_TOKEN")
        if [[ "${FLAYR_RELEASE_TEST:-}" == 1 ]]; then
            while IFS= read -r var; do pass+=("$var=${!var}"); done < <(compgen -e | grep -E '^FLAYR_TEST_[A-Z0-9_]+$' || true)
        fi
        env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin "HOME=$HOME" GH_PROMPT_DISABLED=1 GH_NO_UPDATE_NOTIFIER=1 \
            ${pass[@]+"${pass[@]}"} "$GH_BIN" "$@"
    }
    help="$(gh attestation verify --help 2>&1)" || die "gh has no 'attestation verify'; update gh"
    for flag in --signer-workflow --source-ref --source-digest --deny-self-hosted-runners --cert-oidc-issuer; do
        grep -q -- "$flag" <<< "$help" || die "gh does not support attestation verify $flag; update gh"
    done
fi

overrides="$(env | grep -E '^(FETCHCONTENT_|CMAKE_|CTEST_|CXXFLAGS|CFLAGS|CPPFLAGS|LDFLAGS|OBJCFLAGS|OBJCXXFLAGS|CC|CXX|OBJC|OBJCXX|SDKROOT|MACOSX_DEPLOYMENT_TARGET|LIBRARY_PATH|CPATH|C_INCLUDE_PATH|CPLUS_INCLUDE_PATH|DYLD_)[A-Z_]*=' | cut -d= -f1 || true)"
[[ -z "$overrides" ]] || die "build overrides are set in the environment, unset them first: $(echo $overrides)"

[[ -n "$VERSION" ]] || die "could not read the version from CMakeLists.txt"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/src"
BUILD="$WORK/build"
EMPTY="$WORK/empty"
mkdir -p "$EMPTY" "$SRC"

# Sealed git: nothing from the environment, system or global config, templates or hook
# directories. Repository-local config is only ever read from $ROOT for plumbing
# (rev-parse); the archive comes from a bare copy whose config git itself just wrote.
GIT="$(command -v git)" || die "git not found"
g() {
    env -i "PATH=/usr/bin:/bin:/usr/sbin:/sbin" "HOME=$EMPTY" GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
        GIT_TEMPLATE_DIR="$EMPTY" GIT_ATTR_NOSYSTEM=1 GIT_TERMINAL_PROMPT=0 \
        "$GIT" -c core.hooksPath=/dev/null -c core.fsmonitor=false -c core.attributesFile=/dev/null "$@"
}

HEAD_SHA="$(g -C "$ROOT" rev-parse --verify "HEAD^{commit}")"
TAG_SHA="$(g -C "$ROOT" rev-parse -q --verify "refs/tags/$TAG^{commit}" || true)"
[[ -n "$TAG_SHA" ]] || die "tag $TAG does not exist; tag the release commit first"
[[ "$HEAD_SHA" == "$TAG_SHA" ]] || die "HEAD is not at $TAG (HEAD $HEAD_SHA, $TAG $TAG_SHA)"
# Asked of GitHub by URL from an empty directory: no local remote or insteadOf can redirect it.
# ls-remote's patterns match ref-name suffixes (a branch "x/refs/tags/v1.3.0" would match),
# so list all tags and require exactly one exact refs/tags/<tag>^{} (or, for a lightweight
# tag, refs/tags/<tag>).
remote_tags="$(cd "$EMPTY" && g ls-remote --tags "$REMOTE_URL")" || die "could not list the tags at $REMOTE_URL"
exact_ref() { awk -v ref="$1" -F '\t' '$2 == ref { print $1 }' <<< "$remote_tags"; }
REMOTE_SHA="$(exact_ref "refs/tags/$TAG^{}")"
[[ -n "$REMOTE_SHA" ]] || REMOTE_SHA="$(exact_ref "refs/tags/$TAG")"
[[ "$(wc -l <<< "$REMOTE_SHA" | tr -d ' ')" == 1 ]] || die "$TAG is not a single tag at $REMOTE_URL"
[[ "$REMOTE_SHA" == "$TAG_SHA" ]] || die "$TAG at $REMOTE_URL (${REMOTE_SHA:-missing}) differs from the local tag; push it first"

echo "==> Exporting $TAG ($TAG_SHA)"
g clone --quiet --bare --no-local --no-tags "file://$ROOT" "$WORK/mirror.git"
g -C "$WORK/mirror.git" fetch --quiet "file://$ROOT" "+refs/tags/$TAG:refs/tags/$TAG"
[[ "$(g -C "$WORK/mirror.git" rev-parse --verify "refs/tags/$TAG^{commit}")" == "$TAG_SHA" ]] || die "the copy's $TAG is not $TAG_SHA"
TREE="$(g -C "$WORK/mirror.git" rev-parse --verify "$TAG_SHA^{tree}")"
g -C "$WORK/mirror.git" archive --format=tar "$TAG_SHA" | tar -x -f - -C "$SRC"
# Hash what was extracted (in a throwaway index) and require exactly the tag's tree:
# export-ignore/export-subst attributes or anything else that changed a byte fails here.
g init --quiet --bare "$WORK/verify.git"
g --git-dir="$WORK/verify.git" --work-tree="$SRC" add --all --force .
[[ "$(g --git-dir="$WORK/verify.git" --work-tree="$SRC" write-tree)" == "$TREE" ]] \
    || die "the exported files don't match $TAG's tree $TREE"
[[ "$(sed -n 's/^project(FlayrTune VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")" == "$VERSION" ]] \
    || die "$TAG's CMakeLists.txt is not version $VERSION"
rm -rf "$WORK/mirror.git" "$WORK/verify.git"

echo "==> Building Flayr Tune $VERSION (universal)"
CMAKE="$(command -v cmake)" || die "cmake not found"
# FetchContent's git (the JUCE download) runs sealed too: no global/system config,
# templates, hooks, fsmonitor or attributes file.
BUILD_ENV=(env -i "PATH=$(dirname "$CMAKE"):/usr/bin:/bin:/usr/sbin:/sbin" "HOME=$HOME" "TMPDIR=${TMPDIR:-/tmp}"
           GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TEMPLATE_DIR="$EMPTY" GIT_ATTR_NOSYSTEM=1
           GIT_TERMINAL_PROMPT=0 GIT_CONFIG_COUNT=3
           GIT_CONFIG_KEY_0=core.hooksPath GIT_CONFIG_VALUE_0=/dev/null
           GIT_CONFIG_KEY_1=core.fsmonitor GIT_CONFIG_VALUE_1=false
           GIT_CONFIG_KEY_2=core.attributesFile GIT_CONFIG_VALUE_2=/dev/null)
"${BUILD_ENV[@]}" "$CMAKE" -S "$SRC" -B "$BUILD" -G Xcode \
    -DFLAYR_TUNE_COPY_AFTER_BUILD=OFF -DFLAYR_TUNE_BUILD_TESTS=OFF -DFLAYR_TUNE_BUILD_TOOLS=OFF >/dev/null
"${BUILD_ENV[@]}" "$CMAKE" --build "$BUILD" --config Release --target FlayrTune_AU FlayrTune_VST3 -- -quiet

ART="$BUILD/FlayrTune_artefacts/Release"
echo "==> Checking the JUCE patches and for networking code"
# JUCE's own project(JUCE) caches JUCE_SOURCE_DIR (FetchContent's juce_SOURCE_DIR isn't cached).
JUCE_SRC="$(sed -n 's/^JUCE_SOURCE_DIR:STATIC=//p' "$BUILD/CMakeCache.txt")"
[[ -n "$JUCE_SRC" && -d "$JUCE_SRC" ]] || die "could not find JUCE_SOURCE_DIR in $BUILD/CMakeCache.txt"
[[ "$(cd "$JUCE_SRC" && pwd -P)" == "$(cd "$BUILD/_deps/juce-src" && pwd -P)" ]] \
    || die "JUCE came from $JUCE_SRC, not from the pinned download"
patches="$(cd "$SRC" && ls patches/*.patch | sed "s|^|$SRC/|" | paste -sd'|' -)"
(cd "$JUCE_SRC" && "${BUILD_ENV[@]}" "$CMAKE" "-DPATCHES=$patches" -DCHECK_ONLY=ON -P "$SRC/cmake/ApplyPatch.cmake")
"$SRC/scripts/check-no-network.sh" "$ART"
STAGE="$(mktemp -d)/Flayr Tune"
mkdir -p "$STAGE" "$DIST"
cp -R "$ART/AU/Flayr Tune.component" "$ART/VST3/Flayr Tune.vst3" "$STAGE/"

echo "==> Signing"
for bundle in "$STAGE/Flayr Tune.component" "$STAGE/Flayr Tune.vst3"; do
    codesign --force --options runtime --timestamp --sign "$IDENTITY" "$bundle"
    codesign --verify --strict --verbose=1 "$bundle"
done

echo "==> Notarizing the plug-ins"
ZIP="$(mktemp -d)/plugins.zip"
ditto -c -k --keepParent "$STAGE" "$ZIP"
xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$STAGE/Flayr Tune.component"
xcrun stapler staple "$STAGE/Flayr Tune.vst3"

echo "==> Building the DMG"
ln -s "/Library/Audio/Plug-Ins/Components" "$STAGE/Drag AU here (Logic, GarageBand)"
ln -s "/Library/Audio/Plug-Ins/VST3" "$STAGE/Drag VST3 here (Ableton, FL, Cubase, Reaper...)"
INSTALL_TXT="$SRC/packaging/macos/Install.txt"
[[ -f "$INSTALL_TXT" && ! -L "$INSTALL_TXT" ]] || die "Install.txt is missing or a symlink"
cp -P "$INSTALL_TXT" "$STAGE/Install.txt"
rm -f "$DMG"
hdiutil create -volname "Flayr Tune $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
codesign --force --timestamp --sign "$IDENTITY" "$DMG"

echo "==> Notarizing the DMG"
xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$DMG"
spctl --assess --type open --context context:primary-signature --verbose "$DMG"

echo "==> Done: $DMG"

if [[ "$UPLOAD" == 1 ]]; then
    # Only ever add to a draft: replacing files on a published release would swap what
    # people already downloaded and verified. (The repository also has immutable
    # releases enabled, so GitHub itself refuses changes once a release is published.)
    [[ "$(gh release view "$TAG" --repo "$REPO" --json isDraft --jq .isDraft)" == "true" ]] \
        || die "release $TAG is already published (or missing); refusing to change its files"

    # Verify the CI assets (strict provenance, bound to the tag's own release.yml run) and
    # checksum exactly the verified bytes plus our DMG. Nothing is uploaded unless all of
    # it passes. The verifier comes from the verified tag tree, not the working tree.
    sums_dir="$(mktemp -d)"
    "$SRC/scripts/verify-release-assets.sh" "$TAG" "$TAG_SHA" "$VERSION" "$DMG" "$sums_dir" \
        || die "release assets did not verify; nothing was uploaded"
    cat "$sums_dir/SHA256SUMS.txt"

    gh release upload "$TAG" "$DMG" "$sums_dir/SHA256SUMS.txt" --clobber --repo "$REPO"

    # Afterwards the draft must hold exactly the allowed assets, each with the digest that
    # was checksummed, and the uploaded SHA256SUMS.txt must be byte-for-byte ours.
    assets="$(gh release view "$TAG" --repo "$REPO" --json assets --jq '.assets')"
    allowed="$(cut -d' ' -f3 "$sums_dir/SHA256SUMS.txt"; echo SHA256SUMS.txt)"
    while IFS= read -r name; do
        grep -qxF -- "$name" <<< "$allowed" || die "unexpected asset $name appeared on the draft; do not publish"
    done < <(jq -r '.[].name' <<< "$assets")
    while read -r sum name; do
        [[ "$(jq -r --arg n "$name" '.[] | select(.name == $n) | .digest // empty' <<< "$assets")" == "sha256:$sum" ]] \
            || die "$name on the draft no longer matches SHA256SUMS.txt; do not publish"
    done < "$sums_dir/SHA256SUMS.txt"
    mkdir "$sums_dir/check"
    gh release download "$TAG" --repo "$REPO" --pattern SHA256SUMS.txt --dir "$sums_dir/check"
    cmp -s "$sums_dir/SHA256SUMS.txt" "$sums_dir/check/SHA256SUMS.txt" \
        || die "the SHA256SUMS.txt on the draft is not the one just written; do not publish"
    rm -rf "$sums_dir"
    echo "==> Uploaded $(basename "$DMG") and SHA256SUMS.txt to the draft release $TAG"
fi

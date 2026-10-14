#!/usr/bin/env bash
# Builds the macOS release (universal AU + VST3), signs it with a Developer ID,
# notarizes and staples it, and wraps it in a drag-to-install DMG.
#
#   scripts/release-macos.sh            # build dist/FlayrTune-<version>-macOS.dmg
#   scripts/release-macos.sh --upload   # ...and attach it to the GitHub release v<version>
#
# Needs a "Developer ID Application" identity in the keychain and a notarytool
# keychain profile (xcrun notarytool store-credentials <profile> ...).
#
# Only signs a tagged release: the checkout must be clean, HEAD must sit exactly on a
# v<version> tag that matches CMakeLists.txt and the tag on GitHub, and the build runs
# in a fresh clone of that tag with tests, tools and git hooks off. Nothing from the
# working tree (local edits, untracked files, build dirs) reaches the signed bundle,
# and nothing from the environment either: configure and build run under env -i with a
# minimal PATH, and the script refuses to start if CMake/FetchContent/compiler overrides
# are set (CXXFLAGS=-UFLAYR_NO_NETWORK or FETCHCONTENT_SOURCE_DIR_JUCE=<unpatched JUCE>
# would otherwise reach the signed build). The result is checked for networking code and
# for every JUCE patch before anything is signed.
set -euo pipefail

# No git hooks, fsmonitor or other config-driven commands for any git run below, and
# FetchContent's git runs inherit the same (env -i keeps these variables, see BUILD_ENV).
export GIT_CONFIG_COUNT=2
export GIT_CONFIG_KEY_0=core.hooksPath GIT_CONFIG_VALUE_0=/dev/null
export GIT_CONFIG_KEY_1=core.fsmonitor GIT_CONFIG_VALUE_1=false

IDENTITY="${IDENTITY:-Developer ID Application: Flayr Labs LLC (NRNU83UJ68)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-flayr-notary}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^project(FlayrTune VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
TAG="v$VERSION"
DIST="$ROOT/dist"
DMG="$DIST/FlayrTune-$VERSION-macOS.dmg"

die() { echo "error: $*" >&2; exit 1; }

overrides="$(env | grep -E '^(FETCHCONTENT_|CMAKE_|CTEST_|CXXFLAGS|CFLAGS|CPPFLAGS|LDFLAGS|OBJCFLAGS|OBJCXXFLAGS|CC|CXX|OBJC|OBJCXX|SDKROOT|MACOSX_DEPLOYMENT_TARGET|LIBRARY_PATH|CPATH|C_INCLUDE_PATH|CPLUS_INCLUDE_PATH|DYLD_)[A-Z_]*=' | cut -d= -f1 || true)"
[[ -z "$overrides" ]] || die "build overrides are set in the environment, unset them first: $(echo $overrides)"

[[ -n "$VERSION" ]] || die "could not read the version from CMakeLists.txt"
[[ -z "$(git -C "$ROOT" status --porcelain)" ]] || die "the working tree has uncommitted or untracked changes"
HEAD_SHA="$(git -C "$ROOT" rev-parse HEAD)"
TAG_SHA="$(git -C "$ROOT" rev-parse -q --verify "refs/tags/$TAG^{commit}" || true)"
[[ -n "$TAG_SHA" ]] || die "tag $TAG does not exist; tag the release commit first"
[[ "$HEAD_SHA" == "$TAG_SHA" ]] || die "HEAD is not at $TAG (HEAD $HEAD_SHA, $TAG $TAG_SHA)"
REMOTE_SHA="$(git -C "$ROOT" ls-remote origin "refs/tags/$TAG^{}" | cut -f1)"
[[ -n "$REMOTE_SHA" ]] || REMOTE_SHA="$(git -C "$ROOT" ls-remote origin "refs/tags/$TAG" | cut -f1)"
[[ "$REMOTE_SHA" == "$TAG_SHA" ]] || die "$TAG on origin (${REMOTE_SHA:-missing}) differs from the local tag; push it first"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/src"
BUILD="$WORK/build"

echo "==> Cloning $TAG ($TAG_SHA) into a clean directory"
git -c core.hooksPath=/dev/null clone --quiet --depth 1 --branch "$TAG" --no-recurse-submodules "file://$ROOT" "$SRC"
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$TAG_SHA" ]] || die "the clean clone is not at $TAG"

echo "==> Building Flayr Tune $VERSION (universal)"
CMAKE="$(command -v cmake)" || die "cmake not found"
BUILD_ENV=(env -i "PATH=$(dirname "$CMAKE"):/usr/bin:/bin:/usr/sbin:/sbin" "HOME=$HOME" "TMPDIR=${TMPDIR:-/tmp}"
           GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_COUNT=2
           GIT_CONFIG_KEY_0=core.hooksPath GIT_CONFIG_VALUE_0=/dev/null
           GIT_CONFIG_KEY_1=core.fsmonitor GIT_CONFIG_VALUE_1=false)
"${BUILD_ENV[@]}" "$CMAKE" -S "$SRC" -B "$BUILD" -G Xcode \
    -DFLAYR_TUNE_COPY_AFTER_BUILD=OFF -DFLAYR_TUNE_BUILD_TESTS=OFF -DFLAYR_TUNE_BUILD_TOOLS=OFF >/dev/null
"${BUILD_ENV[@]}" "$CMAKE" --build "$BUILD" --config Release --target FlayrTune_AU FlayrTune_VST3 -- -quiet

ART="$BUILD/FlayrTune_artefacts/Release"
echo "==> Checking the JUCE patches and for networking code"
JUCE_SRC="$(sed -n 's/^juce_SOURCE_DIR:STATIC=//p' "$BUILD/CMakeCache.txt")"
[[ "$JUCE_SRC" == "$BUILD/_deps/juce-src" ]] || die "JUCE came from $JUCE_SRC, not from the pinned download"
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

if [[ "${1:-}" == "--upload" ]]; then
    REPO=FlayrLabs/flayr-tune
    # Only ever add to a draft: replacing files on a published release would swap what
    # people already downloaded and verified.
    [[ "$(gh release view "$TAG" --repo "$REPO" --json isDraft --jq .isDraft)" == "true" ]] \
        || die "release $TAG is already published (or missing); refusing to change its files"
    gh release upload "$TAG" "$DMG" --clobber --repo "$REPO"
    echo "==> Uploaded to release $TAG"

    # Checksums only for files we can vouch for: the DMG we just built and notarized
    # (byte-identical to the local copy), and CI builds whose provenance attestation
    # verifies. Anything else on the draft stops the script.
    sums_dir="$(mktemp -d)"
    gh release download "$TAG" --repo "$REPO" --dir "$sums_dir" --pattern 'FlayrTune-*'
    for asset in "$sums_dir"/FlayrTune-*; do
        name="$(basename "$asset")"
        if [[ "$name" == "$(basename "$DMG")" ]]; then
            cmp -s "$asset" "$DMG" || die "$name on the release differs from the DMG just built"
        else
            gh attestation verify "$asset" --repo "$REPO" >/dev/null || die "$name has no valid build provenance attestation"
        fi
        echo "    verified $name"
    done
    (cd "$sums_dir" && shasum -a 256 FlayrTune-* > SHA256SUMS.txt && cat SHA256SUMS.txt)
    gh release upload "$TAG" "$sums_dir/SHA256SUMS.txt" --clobber --repo "$REPO"
    rm -rf "$sums_dir"
    echo "==> Uploaded SHA256SUMS.txt"
fi

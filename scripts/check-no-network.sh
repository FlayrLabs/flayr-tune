#!/usr/bin/env bash
# Fails if a built Flayr Tune plug-in binary links against any networking API.
#
#   scripts/check-no-network.sh [artefacts dir]   (default: build/FlayrTune_artefacts/Release)
#
# Tune never uses the network, and the JUCE networking code is compiled out
# (FLAYR_NO_NETWORK, patches/juce-no-network.patch). This checks the result in
# the binaries themselves, with the platform's own tools:
#   macOS    nm -u / otool -L    (per architecture)
#   Linux    nm -D --undefined-only
#   Windows  dumpbin /imports     (from the Visual Studio install, found via vswhere)
# Every binary is also searched for the names of networking libraries, which
# would show up if anything tried to load one at run time.
set -euo pipefail

DIR="${1:-build/FlayrTune_artefacts/Release}"
[[ -d "$DIR" ]] || { echo "error: no artefacts at $DIR" >&2; exit 2; }

# BSD socket and resolver calls. socketpair() is deliberately absent: on Linux,
# JUCE's message loop wakes itself through an AF_LOCAL socketpair, which never
# leaves the process. gethostname() is absent too: it reads the machine name.
SOCKET_FNS='socket|connect|bind|listen|accept|accept4|getaddrinfo|freeaddrinfo|getnameinfo|gethostbyname|gethostbyname2|gethostbyname_r|gethostbyaddr|getifaddrs|send|sendto|sendmsg|recv|recvfrom|recvmsg|setsockopt|getsockopt|inet_addr|inet_ntoa|inet_ntop|inet_pton|res_query|res_search'
LIBS='wininet|winhttp|ws2_32|wsock32|mswsock|iphlpapi|netapi32|urlmon|dnsapi|libcurl|CFNetwork|Network\.framework'

failed=0
report() { echo "  FAIL: $*"; failed=1; }

check_strings() {
    local hits
    hits="$(LC_ALL=C grep -aoiE "($LIBS)(\.dll|\.so|\.framework)?" "$1" | sort -u || true)"
    [[ -z "$hits" ]] || report "names of networking libraries inside $1: $(echo $hits)"
}

binaries=()
case "$(uname -s)" in
    Darwin)
        while IFS= read -r -d '' f; do binaries+=("$f"); done < <(find "$DIR" -type f -path '*/Contents/MacOS/*' -print0)
        for f in "${binaries[@]}"; do
            echo "== $f"
            for arch in $(lipo -archs "$f"); do
                syms="$(nm -u -arch "$arch" "$f")"
                echo "  $arch: $(wc -l <<< "$syms" | tr -d ' ') undefined symbols"
                grep -qx '_malloc' <<< "$syms" || report "$arch: symbol listing looks wrong (no _malloc)"
                bad="$(grep -E "^_($SOCKET_FNS)\$|NSURLSession|NSURLConnection|NSURLDownload|_CFHTTP|_CFSocket|_CFHost|_CFStreamCreatePairWithSocket|_CFNetwork|_nw_|OBJC_CLASS_\\\$_WK|WebView" <<< "$syms" || true)"
                [[ -z "$bad" ]] || report "$arch imports: $(echo $bad)"
            done
            libs="$(otool -L "$f" | grep -E 'CFNetwork|Network\.framework|libcurl' || true)"
            [[ -z "$libs" ]] || report "links: $libs"
            check_strings "$f"
        done
        ;;
    Linux)
        while IFS= read -r -d '' f; do binaries+=("$f"); done < <(find "$DIR" -type f -name '*.so' -print0)
        for f in "${binaries[@]}"; do
            echo "== $f"
            syms="$(nm -D --undefined-only "$f" | awk '{print $NF}' | sed 's/@.*//')"
            echo "  $(wc -l <<< "$syms" | tr -d ' ') undefined symbols; allowed, not networking: $(grep -xE 'socketpair|gethostname' <<< "$syms" | tr '\n' ' ')"
            grep -qx 'malloc' <<< "$syms" || report "symbol listing looks wrong (no malloc)"
            bad="$(grep -xE "$SOCKET_FNS" <<< "$syms" || true)"
            [[ -z "$bad" ]] || report "imports: $(echo $bad)"
            libs="$( (readelf -d "$f" | grep NEEDED | grep -iE "$LIBS") || true)"
            [[ -z "$libs" ]] || report "links: $libs"
            check_strings "$f"
        done
        ;;
    MINGW*|MSYS*|CYGWIN*)
        vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
        vs="$("$vswhere" -latest -products '*' -property installationPath | tr -d '\r')"
        dumpbin="$(ls "$(cygpath -u "$vs")"/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe | tail -1)"
        while IFS= read -r -d '' f; do binaries+=("$f"); done < <(find "$DIR" -type f \( -name '*.vst3' -o -name '*.dll' \) -print0)
        for f in "${binaries[@]}"; do
            echo "== $f"
            imports="$("$dumpbin" -nologo -imports "$(cygpath -w "$f")" | tr -d '\r')"
            dlls="$(grep -iE '^ +[A-Za-z0-9_.-]+\.dll$' <<< "$imports" | tr -d ' ' | sort -fu)"
            echo "  imports from: $(echo $dlls)"
            grep -qix 'KERNEL32.dll' <<< "$dlls" || report "import listing looks wrong (no KERNEL32.dll)"
            bad="$(grep -iwE "$LIBS" <<< "$dlls" || true)"
            [[ -z "$bad" ]] || report "imports from $(echo $bad)"
            check_strings "$f"
        done
        ;;
    *) echo "error: unsupported platform $(uname -s)" >&2; exit 2 ;;
esac

[[ ${#binaries[@]} -gt 0 ]] || { echo "error: no plug-in binaries under $DIR" >&2; exit 2; }
if [[ $failed -ne 0 ]]; then
    echo "Networking code found in the plug-in." >&2
    exit 1
fi
echo "OK: no networking imports in ${#binaries[@]} binaries."

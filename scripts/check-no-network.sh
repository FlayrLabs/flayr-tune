#!/usr/bin/env bash
# Fails if a built Flayr Tune plug-in binary shows any sign of networking code.
#
#   scripts/check-no-network.sh [artefacts dir]   (default: build/FlayrTune_artefacts/Release)
#
# Tune never uses the network, and JUCE's networking code is compiled out
# (FLAYR_NO_NETWORK, patches/juce-no-network.patch). This is a tripwire for that,
# not a proof: it checks, with the platform's own tools,
#   - the imported symbols and libraries (macOS nm -u / otool -L per architecture,
#     Linux nm -D / readelf -d, Windows dumpbin /imports), and
#   - the raw bytes, for the names of networking APIs, classes and libraries, which
#     is where a run-time lookup (dlsym, GetProcAddress, objc_getClass, a selector
#     string) would show up.
# Code that builds those names at run time, or makes system calls directly, would
# get past it. Every tool's exit status is checked, and a file of an unknown type
# fails the check rather than passing it.
set -euo pipefail

DIR="${1:-build/FlayrTune_artefacts/Release}"
[[ -d "$DIR" ]] || { echo "error: no artefacts at $DIR" >&2; exit 2; }

# BSD socket and resolver calls, matched whole with the variants libc uses
# (macOS $NOCANCEL / $UNIX2003 suffixes, glibc _chk wrappers, res_9_ / __res_ prefixes).
# socketpair() is deliberately absent: on Linux, JUCE's message loop wakes itself
# through an AF_LOCAL socketpair, which never leaves the process. gethostname() is
# absent too: it reads the machine name.
SOCKET_FNS='socket|connect|connectx|bind|listen|accept|accept4|getaddrinfo|getaddrinfo_a|freeaddrinfo|getnameinfo|gethostbyname|gethostbyname2|gethostbyname_r|gethostbyname2_r|gethostbyaddr|gethostbyaddr_r|getifaddrs|send|sendto|sendmsg|sendmmsg|recv|recvfrom|recvmsg|recvmmsg|setsockopt|getsockopt|getpeername|getsockname|shutdown|inet_addr|inet_aton|inet_ntoa|inet_ntop|inet_pton|res_query|res_search|res_init|res_nquery|res_nsearch|res_ninit|dn_expand'
# Starting other programs (curl, a helper) would be a way round all of the above.
PROCESS_FNS='popen|system|posix_spawn|posix_spawnp|fork|vfork|execv|execve|execvp|execvP|execl|execle|execlp|execvpe|fexecve'
SYMBOL_RE="^_*(res_9_|__res_)?($SOCKET_FNS|$PROCESS_FNS)(\\\$[A-Z0-9_]+|_chk)?(@.*)?\$"
# Objective-C / CoreFoundation / Network.framework / Bonjour symbols.
APPLE_RE='NSTask|_CFURLCreateDataAndPropertiesFromResource|_CFURLCopyResourcePropertyForKey_NETWORK|NSURLSession|NSURLConnection|NSURLDownload|NSURLRequest|NSStream|_CFHTTP|_CFSocket|_CFHost|_CFStreamCreatePairWithSocket|_CFNetwork|_CFReadStreamCreateWithFTPURL|_nw_|_DNSService|_SCNetworkReachability|OBJC_CLASS_\$_WK|WebView'
# Names anywhere in the file (strings for run-time lookups, import tables, selectors).
NAMES_RE='NSURLSession|NSURLConnection|NSURLDownload|NSURLRequest|CFNetwork|CFHTTP[A-Za-z]*|CFSocket[A-Za-z]*|CFStreamCreatePairWithSocket|CFHost[A-Za-z]*|getaddrinfo|gethostbyname|getnameinfo|WSAStartup|WSASocket|WinHttp[A-Za-z]*|InternetOpen|InternetConnect|InternetReadFile|HttpOpenRequest|HttpSendRequest|URLDownloadToFile|URLOpenStream|libcurl|curl_easy_[a-z_]*|dataWithContentsOfURL|initWithContentsOfURL|stringWithContentsOfURL|dictionaryWithContentsOfURL|arrayWithContentsOfURL|CFURLCreateDataAndPropertiesFromResource|NSTask|launchedTaskWithLaunchPath|/usr/bin/curl|/usr/bin/wget|DNSService[A-Za-z]*|nw_connection_[a-z_]*|nw_endpoint_[a-z_]*|res_9_query|getStreamsToHost|SCNetworkReachability[A-Za-z]*|wininet|winhttp|ws2_32|wsock32|mswsock|iphlpapi|netapi32|urlmon|dnsapi|Network\.framework'
# Function names as stand-alone C strings (what dlsym / GetProcAddress would be handed).
# ("system" is left out here: it is also an ordinary word in strings, e.g. font names;
# it is still refused as an import above.)
DYNAMIC_NAMES='socket|connect|getaddrinfo|gethostbyname|getnameinfo|send|sendto|recv|recvfrom|bind|listen|accept|popen|posix_spawn|posix_spawnp|fork|vfork|execv|execve|execvp|NSTask|NSURLSession|WSAStartup|InternetOpenW|InternetOpenA|WinHttpOpen|CreateProcessW|CreateProcessA|WinExec'
# Windows imported functions that start programs.
WIN_PROCESS_RE='CreateProcess(A|W|AsUserA|AsUserW|WithLogonW|WithTokenW)|WinExec'
# Import libraries (Windows DLLs, ELF NEEDED, Mach-O load commands).
LIBS_RE='wininet|winhttp|ws2_32|wsock32|mswsock|iphlpapi|netapi32|urlmon|dnsapi|libcurl|libresolv|CFNetwork|Network\.framework|WebKit'

failed=0
report() { echo "  FAIL: $*"; failed=1; }
die() { echo "error: $*" >&2; exit 2; }

# Runs a tool; any failure ends the check (a listing we couldn't make proves nothing).
ERR="$(mktemp)"
trap 'rm -f "$ERR"' EXIT
run() {
    local out
    if ! out="$("$@" 2>"$ERR")"; then
        die "$1 failed on ${*: -1}: $(cat "$ERR")"
    fi
    printf '%s\n' "$out"
}

check_names() {
    local hits
    hits="$(LC_ALL=C grep -aoE "$NAMES_RE" "$1" | sort -u || true)"
    [[ -z "$hits" ]] || report "networking names inside the binary: $(echo $hits)"
    hits="$(LC_ALL=C grep -aoE "(^|[^A-Za-z0-9_])(curl|wget|NSTask)([^A-Za-z0-9_]|$)" "$1" | tr -cd 'A-Za-z\n' | sort -u || true)"
    [[ -z "$hits" ]] || report "names of download tools or process launchers: $(echo $hits)"
    # URLs: only identifiers that are never fetched may appear: XML and RDF/LV2 vocabulary
    # namespaces, JUCE's LV2 extension URI, file:// (local paths) and the project's own
    # page (VST3/LV2 metadata).
    hits="$(LC_ALL=C grep -aoE "(https?|ftps?|wss?|file|smb|afp|nfs|ssh|sftp|scp|telnet|ldaps?|rtsps?|rtmp|mms|git|svn|gopher|irc|xmpp|sip|stun|turn)://[A-Za-z0-9._~:/?#@!$&'()*+,;=%-]*" "$1" \
            | grep -vE '^http://www\.w3\.org/|^https://github\.com/FlayrLabs/|^file://|^http://lv2plug\.in/ns/|^http://usefulinc\.com/ns/doap|^http://xmlns\.com/foaf/|^https://lv2-extensions\.juce\.com/' \
            | sort -u || true)"
    [[ -z "$hits" ]] || report "URLs inside the binary: $(echo $hits)"
    hits="$(perl -0777 -ne 'while (/\x00_?('"$DYNAMIC_NAMES"')\x00/g) { print "$1\n" }' "$1" | sort -u)" \
        || die "perl failed on $1"
    [[ -z "$hits" ]] || report "function names as stand-alone strings (run-time lookup?): $(echo $hits)"
}

# An AU's Info.plist can ask a sandboxed host for network access (resourceUsage
# network.client); entitlements can too. Tune must ask for neither.
check_plist() {
    if LC_ALL=C grep -aqE 'network\.client|network\.server|com\.apple\.security\.network' "$1"; then
        report "declares network access: $1"
    fi
}

file_kind() {
    local magic
    magic="$(LC_ALL=C od -An -tx1 -N4 "$1" | tr -d ' \n')"
    case "$magic" in
        cafebabe|bebafeca|cffaedfe|feedfacf|cefaedfe|feedface) echo macho ;;
        7f454c46) echo elf ;;
        4d5a*) echo pe ;;
        *) echo unknown ;;
    esac
}

case "$(uname -s)" in
    Darwin|Linux) ;;
    MINGW*|MSYS*|CYGWIN*)
        vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
        vs="$(run "$vswhere" -latest -products '*' -property installationPath | tr -d '\r')"
        [[ -n "$vs" ]] || die "no Visual Studio installation found"
        dumpbin="$(ls "$(cygpath -u "$vs")"/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe | tail -1)"
        [[ -x "$dumpbin" ]] || die "dumpbin not found"
        ;;
    *) die "unsupported platform $(uname -s)" ;;
esac

# Every file in the artefacts: each executable format is checked (wherever it sits in a
# bundle), every plist is checked, and the plug-in binaries themselves must be one of
# the executable formats.
binaries=()
plists=0
while IFS= read -r -d '' f; do
    kind="$(file_kind "$f")"
    case "$f" in
        */Contents/MacOS/*|*.so|*/Contents/x86_64-win/*.vst3)
            [[ "$kind" != unknown ]] || report "plug-in binary of unknown type: $f" ;;
    esac
    case "$f" in *.plist) check_plist "$f"; plists=$((plists + 1)) ;; esac
    [[ "$kind" == unknown ]] || binaries+=("$f")
done < <(find "$DIR" -type f ! -name '*.a' ! -name '*.lib' -print0)
[[ ${#binaries[@]} -gt 0 ]] || die "no plug-in binaries under $DIR"
echo "checked $plists plists"

for f in "${binaries[@]}"; do
    echo "== $f"
    kind="$(file_kind "$f")"
    case "$(uname -s):$kind" in
        Darwin:macho)
            archs="$(run lipo -archs "$f")"
            [[ -n "$archs" ]] || die "lipo found no architectures in $f"
            for arch in $archs; do
                syms="$(run nm -u -arch "$arch" "$f")"
                echo "  $arch: $(wc -l <<< "$syms" | tr -d ' ') undefined symbols"
                grep -qx '_malloc' <<< "$syms" || report "$arch: symbol listing looks wrong (no _malloc)"
                bad="$(grep -E "$SYMBOL_RE" <<< "$syms" || true)$(grep -E "$APPLE_RE" <<< "$syms" || true)"
                [[ -z "$bad" ]] || report "$arch imports: $(echo $bad)"
                libs="$(run otool -L -arch "$arch" "$f")"
                bad="$(tail -n +2 <<< "$libs" | grep -E "$LIBS_RE" || true)"
                [[ -z "$bad" ]] || report "$arch links: $(echo $bad)"
            done
            ;;
        Linux:elf)
            syms="$(run nm -D --undefined-only "$f" | awk '{print $NF}')"
            echo "  $(wc -l <<< "$syms" | tr -d ' ') undefined symbols; allowed, not networking: $(sed 's/@.*//' <<< "$syms" | grep -xE 'socketpair|gethostname' | tr '\n' ' ')"
            grep -qE '^malloc(@|$)' <<< "$syms" || report "symbol listing looks wrong (no malloc)"
            bad="$(grep -E "$SYMBOL_RE" <<< "$syms" || true)"
            [[ -z "$bad" ]] || report "imports: $(echo $bad)"
            needed="$(run readelf -d "$f" | grep NEEDED || true)"
            [[ -n "$needed" ]] || report "no NEEDED entries: dynamic section listing looks wrong"
            bad="$(grep -iE "$LIBS_RE" <<< "$needed" || true)"
            [[ -z "$bad" ]] || report "links: $bad"
            ;;
        MINGW*:pe|MSYS*:pe|CYGWIN*:pe)
            imports="$(run "$dumpbin" -nologo -imports "$(cygpath -w "$f")" | tr -d '\r')"
            dlls="$(grep -iE '^ +[A-Za-z0-9_.-]+\.dll$' <<< "$imports" | tr -d ' ' | sort -fu)"
            echo "  imports from: $(echo $dlls)"
            grep -qix 'KERNEL32.dll' <<< "$dlls" || report "import listing looks wrong (no KERNEL32.dll)"
            bad="$(grep -iwE "$LIBS_RE" <<< "$dlls" || true)"
            [[ -z "$bad" ]] || report "imports from $(echo $bad)"
            bad="$(grep -owE "$WIN_PROCESS_RE" <<< "$imports" | sort -u || true)"
            [[ -z "$bad" ]] || report "imports process-starting functions: $(echo $bad)"
            ;;
        *) report "unexpected file type ($kind) for this platform: $f" ;;
    esac
    check_names "$f"
done

if [[ $failed -ne 0 ]]; then
    echo "Networking code found in the plug-in (or the check could not run properly)." >&2
    exit 1
fi
echo "OK: no networking imports, names or entitlements in ${#binaries[@]} binaries."

#!/usr/bin/env bash
# windows-cross-build.sh — cross-build OpenYog for Windows on Linux: puts
# openyog.exe, every DLL it loads and the Qt plugins it needs in
# build-mingw/deploy/, then (when wine is installed) runs selftests on it.
#
#   tools/windows-cross-build.sh [--zip]
#
# --zip also packs deploy/ into build-mingw/openyog-win-deploy.zip.
#
# Needs: mingw-w64-gcc (Arch: pacman -S mingw-w64-gcc), cmake, ninja,
# python3, and host Qt 6 tools (moc/rcc/uic, Arch: qt6-base) of the same
# version as the Windows Qt. The Windows libraries — Qt, QScintilla,
# QtKeychain, the MariaDB/PostgreSQL/SQLite clients — are the MSYS2 UCRT64
# packages tools/msys2-ucrt64.lock lists, which tools/fetch-msys2-sysroot.py
# unpacks into build-mingw/sysroot.
#
# Environment: BUILD (default build-mingw), QT_HOST_PATH (default /usr),
# WINEPREFIX (default $BUILD/wine: the selftests get a wine prefix of their
# own, and --keychaintest writes to its Credential Manager, not ~/.wine's).
# With OPENYOG_TEST_SSH=host:port:user:password (an SSH server that allows
# forwarding) and OPENYOG_TEST_PG=host:port:user:password:db (a PostgreSQL
# server as seen from it), a query also runs through an SSH tunnel; its
# known_hosts is $BUILD/ssh, not ~/.ssh.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-mingw}"
SYSROOT="$BUILD/sysroot/ucrt64"
DEPLOY="$BUILD/deploy"
QT_HOST_PATH="${QT_HOST_PATH:-/usr}"
TRIPLE=x86_64-w64-mingw32

for tool in $TRIPLE-g++ cmake ninja python3; do
    command -v "$tool" >/dev/null || { echo "$tool missing" >&2; exit 1; }
done

# ---- the Windows libraries ------------------------------------------------
# unpacked again whenever tools/msys2-ucrt64.lock changes
LOCK="$ROOT/tools/msys2-ucrt64.lock"
if ! cmp -s "$LOCK" "$BUILD/sysroot/msys2-ucrt64.lock"; then
    python3 "$ROOT/tools/fetch-msys2-sysroot.py" "$BUILD/sysroot"
    cp "$LOCK" "$BUILD/sysroot/msys2-ucrt64.lock"
fi

# moc/rcc/uic come from the host Qt; their output only fits the same version
qt_version() {
    sed -n 's/^set(PACKAGE_VERSION "\([0-9.]*\)")$/\1/p' "$1/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"
}
host_qt=$(qt_version "$QT_HOST_PATH")
target_qt=$(qt_version "$SYSROOT")
if [ -z "$host_qt" ] || [ "$host_qt" != "$target_qt" ]; then
    echo "host Qt is '$host_qt' ($QT_HOST_PATH), the sysroot's is '$target_qt': they must match." >&2
    echo "Pin the sysroot's qt6-base to the host's version (see tools/fetch-msys2-sysroot.py" >&2
    echo "--update --pin), or point QT_HOST_PATH at a Qt $target_qt." >&2
    exit 1
fi

# ---- configure + build ----------------------------------------------------
# OPENYOG_BUILD_TESTS=OFF: the test executables would only run under wine
cmake -G Ninja -S "$ROOT" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/mingw-w64.cmake" \
    -DOPENYOG_SYSROOT="$SYSROOT" -DQT_HOST_PATH="$QT_HOST_PATH" \
    -DCMAKE_BUILD_TYPE=Release -DOPENYOG_BUILD_TESTS=OFF
cmake --build "$BUILD" --target openyog

# ---- deploy: the exe, its plugins, and every DLL either loads ---------------
rm -rf "$DEPLOY"
mkdir -p "$DEPLOY"
cp "$BUILD/openyog.exe" "$DEPLOY/"
# Qt finds plugins in these folders next to the exe:
#   platforms/   qwindows for a desktop, qoffscreen for headless selftests
#   imageformats/qico   every icon in the UI is a .ico, and QtGui itself
#                       decodes no ICO: without it the toolbars come up blank
#   styles/      qmodernwindowsstyle: the native windows11/windowsvista look
#                instead of Qt's plain fallback style
plugins=(platforms/qwindows.dll platforms/qoffscreen.dll imageformats/qico.dll
         styles/qmodernwindowsstyle.dll)
for p in "${plugins[@]}"; do
    mkdir -p "$DEPLOY/$(dirname "$p")"
    cp "$SYSROOT/share/qt6/plugins/$p" "$DEPLOY/$p"
done

# Walk the imports of the exe and the plugins, and copy each DLL that isn't
# part of Windows. The compiler's own runtime (libstdc++, libgcc_s,
# winpthread) is taken from the toolchain first: the exe was built against it.
python3 - "$DEPLOY" "/usr/$TRIPLE/bin" "$SYSROOT/bin" -- "$DEPLOY/openyog.exe" \
    "${plugins[@]/#/$DEPLOY/}" <<'EOF'
import os, shutil, subprocess, sys
args = sys.argv[1:]
deploy, searchdirs, roots = args[0], args[1:args.index("--")], args[args.index("--") + 1:]
windows = set("""advapi32 authz bcrypt cfgmgr32 comctl32 comdlg32 crypt32 cryptbase
d2d1 d3d11 d3d12 d3d9 dbghelp dcomp dnsapi dwmapi dwrite dxgi gdi32 gdiplus imm32
iphlpapi kernel32 mpr msimg32 msvcrt ncrypt netapi32 normaliz ntdll ole32 oleaut32
opengl32 rpcrt4 secur32 setupapi shell32 shcore shlwapi ucrtbase user32 userenv
usp10 uxtheme version winhttp winmm winspool.drv wintrust wldap32 ws2_32
wtsapi32""".split())
seen, missing = set(), []
def walk(path):
    out = subprocess.run(["objdump", "-p", path], capture_output=True, text=True,
                         check=True).stdout
    for line in out.splitlines():
        if "DLL Name:" not in line:
            continue
        dll = line.split("DLL Name:", 1)[1].strip()
        low = dll.lower()
        if low in seen or low.startswith(("api-ms-", "ext-ms-")) or low.removesuffix(".dll") in windows:
            continue
        seen.add(low)
        for d in searchdirs:
            src = os.path.join(d, dll)
            if os.path.isfile(src):
                shutil.copy(src, deploy)
                walk(src)
                break
        else:
            missing.append(dll)
for r in roots:
    walk(r)
if missing:
    sys.exit("not found in the toolchain or the sysroot: " + " ".join(missing))
print(f"deploy: {len(seen)} DLLs")
EOF
cp "$ROOT/GPL.txt" "$DEPLOY/"
echo "deploy ready: $DEPLOY"

# ---- selftests under wine -------------------------------------------------
if command -v wine >/dev/null; then
    export WINEPREFIX="${WINEPREFIX:-$BUILD/wine}" WINEDEBUG=-all
    export WINEDLLOVERRIDES="mscoree,mshtml=" QT_QPA_PLATFORM=offscreen
    fails=0
    for t in --version --stmtattest --icontest --keychaintest; do
        if (cd "$DEPLOY" && timeout 300 wine openyog.exe "$t"); then
            echo "wine $t: ok"
        else
            echo "wine $t: FAILED"; fails=$((fails + 1))
        fi
    done
    if [ -n "${OPENYOG_TEST_SSH:-}" ] && [ -n "${OPENYOG_TEST_PG:-}" ]; then
        mkdir -p "$BUILD/ssh"
        out=$(cd "$DEPLOY" && OPENYOG_SSH_DIR="$(winepath -w "$BUILD/ssh")" timeout 300 \
              wine openyog.exe "--autoconnectpg=$OPENYOG_TEST_PG" "--ssh=$OPENYOG_TEST_SSH" \
              --sshtrust "--sql=SELECT 'through the tunnel'" --runagain --printresults \
              "--screenshot=$(winepath -w "$BUILD/ssh-tunnel.png")" 2>&1) || true
        if grep -q '1 row(s)' <<<"$out"; then
            echo "wine SSH tunnel: ok"
        else
            echo "$out" | grep -v '^MESA' >&2
            echo "wine SSH tunnel: FAILED"; fails=$((fails + 1))
        fi
    fi
    wineserver -k 2>/dev/null || true
    [ "$fails" = 0 ] || { echo "$fails selftest(s) failed under wine" >&2; exit 1; }
else
    echo "wine not installed: selftests skipped"
fi

if [ "${1:-}" = "--zip" ]; then
    rm -f "$BUILD/openyog-win-deploy.zip"
    (cd "$DEPLOY" && zip -qr "$BUILD/openyog-win-deploy.zip" .)
    echo "zip: $BUILD/openyog-win-deploy.zip"
fi

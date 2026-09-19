#!/usr/bin/env bash
# windows-cross-build.sh — the proven OpenYog Windows cross-build + deploy
# pipeline (first proven end-to-end 2026-09-17, sessions 92-93).
#
# Produces build-mingw/deploy/ (openyog.exe + full DLL closure + Qt plugins)
# and, when wine is present, runs the --casetest selftest on the Windows
# binary as proof of life. `--zip` additionally packs deploy/ into
# build-mingw/openyog-win-deploy.zip for copying to a real Windows box.
#
# One-time prerequisites (already on this machine, see WORKLOG 2026-09-17):
#   1. Arch mingw-w64 toolchain + client libs (pacman -Udd — some deps are
#      deliberately absent: mariadb's curl, harfbuzz's freetype2):
#        mingw-w64-gcc mingw-w64-cmake mingw-w64-ninja mingw-w64-openssl
#        mingw-w64-zlib mingw-w64-pcre2 mingw-w64-zstd mingw-w64-sqlite
#        mingw-w64-mariadb-connector-c mingw-w64-postgresql
#   2. Qt 6.11.2 win64_mingw PREBUILT at ~/Qt-mingw/6.11.2/mingw_64.
#      NOT the AUR qt6-base (its system_freetype=ON hard-force breaks on
#      freetype2/harfbuzz circularity) and NOT the llvm_mingw kit (libc++
#      ABI — unlinkable from gcc). Manual download recipe in WORKLOG;
#      essentials = qtbase + d3dcompiler_47 + opengl32sw 7z archives from
#      online/qtsdkrepository/windows_x86/desktop/qt6_6112/qt6_6112_mingw/
#      qt.qt6.6112.win64_mingw/ (mirror: nluug; verify sizes, curl lies).
#   3. QScintilla 2.14.1 cross-built from the generated CMakeLists
#      (/tmp/qsci-cross recipe in WORKLOG; headers GLOB'd into add_library
#      for AUTOMOC) and installed into the Qt tree with
#      cmake --install build-mingw --prefix ~/Qt-mingw/6.11.2/mingw_64.
#   4. mariadb-connector-c 3.3.10 built from source at $MARIADB_MINGW below
#      — NOT the AUR mingw-w64-mariadb-connector-c package (3.4.8): that
#      version has a security-hardening default (MariaDB 11.4+) that
#      unconditionally demands TLS on every TCP connection, and
#      MYSQL_OPT_SSL_ENFORCE=0 does not disable it (open upstream
#      regression). Recipe: `git clone --branch v3.3.10 https://github.com/
#      mariadb-corporation/mariadb-connector-c.git`, then
#        x86_64-w64-mingw32-cmake -G Ninja -B build-mingw \
#            -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_C_FLAGS=-std=gnu17 \
#            -DWITH_CURL=OFF -DWITH_UNIT_TESTS=OFF -DWITH_SSL=SCHANNEL \
#            -DWITH_MYSQLCOMPAT=OFF -DCMAKE_FIND_ROOT_PATH=/usr/x86_64-w64-mingw32
#        cmake --build build-mingw --target mariadbclient   # static lib
#        cmake --build build-mingw --target libmariadb      # the DLL
#        cmake --install build-mingw --prefix "$MARIADB_MINGW" --component Development
#        cmake --install build-mingw --prefix "$MARIADB_MINGW" --component SharedLibraries
#      (`-DCMAKE_POLICY_VERSION_MINIMUM`/`-std=gnu17`: this 2021-era source
#      predates both a newer CMake's minimum-version floor and a C23 compiler
#      making `bool` a keyword, which collides with the source's own
#      `typedef char bool`.) Then two compatibility fixups this project's
#      layout needs (3.3.10's own install layout is include/mariadb/*.h +
#      lib/mariadb/, not the include/mysql/ + flat lib/ every other package
#      here uses):
#        ln -s mariadb "$MARIADB_MINGW/include/mysql"
#        cp "$MARIADB_MINGW/lib/mariadb/liblibmariadb.dll.a" \
#           "$MARIADB_MINGW/lib/mariadb/libmariadb.dll.a"    # upstream's own
#      CMake names the import lib "liblibmariadb.dll.a" (target "libmariadb"
#      + the "lib" prefix, doubled) — copied to the name find_library()
#      actually looks for.
#      Full story: xnote/2026-09-19-windows-mariadb-ssl-regression.md
#
# Gotchas baked into the flags below (all hit the hard way):
#   - The Arch mingw toolchain is UCRT; official win64_mingw Qt is UCRT too.
#   - The Qt tree must be a FIND_ROOT_PATH entry: under the cross toolchain's
#     MODE=ONLY a plain CMAKE_PREFIX_PATH gets re-rooted into nothing.
#   - QT_HOST_PATH=/usr and wine-as-emulator come from the distro wrapper
#     x86_64-w64-mingw32-cmake; same-version native Qt supplies host moc/rcc.
#   - CMakeLists.txt rebuilds port/shim/mysql on every configure, so native
#     and cross build trees can coexist, but re-verify the NATIVE build after
#     any CMakeLists change (the shims are shared source-tree state).

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QT_MINGW="${QT_MINGW:-$HOME/Qt-mingw/6.11.2/mingw_64}"
# mingw-w64-mariadb-connector-c (AUR, 3.4.8 as of this writing) is NOT used
# for the Windows build — see MARIADB_MINGW below and the long comment at
# this script's DLL-closure step for why.
MARIADB_MINGW="${MARIADB_MINGW:-$HOME/mingw-mariadb-3.3.10}"
BUILD="$ROOT/build-mingw"
DEPLOY="$BUILD/deploy"
QSCINTILLA_LIB="$QT_MINGW/bin/libqscintilla2_qt6.dll"

command -v x86_64-w64-mingw32-cmake >/dev/null || { echo "mingw-w64-cmake missing" >&2; exit 1; }
[ -d "$QT_MINGW/lib/cmake" ] || { echo "Qt mingw tree missing at $QT_MINGW" >&2; exit 1; }
[ -f "$QSCINTILLA_LIB" ] || { echo "QScintilla not installed into the Qt tree ($QSCINTILLA_LIB)" >&2; exit 1; }
[ -f "$MARIADB_MINGW/lib/mariadb/libmariadb.dll" ] || {
    echo "mariadb-connector-c 3.3.10 not built at $MARIADB_MINGW — see" >&2
    echo "xnote/2026-09-19-windows-mariadb-ssl-regression.md to rebuild it" >&2
    exit 1
}

# ---- configure + build -------------------------------------------------
# MDB_MYSQL_DIR/MARIADB_LIBRARY: pre-set these two find_path()/find_library()
# cache variables to skip the root CMakeLists.txt's own search entirely —
# it would otherwise happily find the AUR mingw-w64-mariadb-connector-c
# package's 3.4.8 headers/import-lib on the sysroot instead (same symbol
# names, so it links fine and the bug below wouldn't show up until runtime
# against a real non-TLS server). Native Linux is untouched: these are only
# passed here, never added to CMakeLists.txt itself.
x86_64-w64-mingw32-cmake -G Ninja -B "$BUILD" -S "$ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_FIND_ROOT_PATH="/usr/x86_64-w64-mingw32;$QT_MINGW" \
    -DMDB_MYSQL_DIR="$MARIADB_MINGW/include" \
    -DMARIADB_LIBRARY="$MARIADB_MINGW/lib/mariadb/libmariadb.dll.a"
cmake --build "$BUILD" --target openyog

# ---- deploy: exe + transitive DLL closure ------------------------------
# Wine/Windows search the exe's own directory first; every non-system import
# must physically sit next to the exe. Walk each DLL's "DLL Name:" imports
# and copy what isn't a wine/Windows builtin from our three supply dirs.
mkdir -p "$DEPLOY/platforms"
cp -f "$BUILD/openyog.exe" "$DEPLOY/"

# libmariadb.dll goes in FIRST, explicitly, from our own 3.3.10 build — not
# from the closure walk below. mingw-w64-mariadb-connector-c (AUR) is on
# 3.4.8, which shipped a security-hardening default change (MariaDB 11.4+):
# every TCP connection now demands TLS unconditionally, and MYSQL_OPT_SSL_
# ENFORCE=0 does NOT turn it back off (confirmed upstream regression, still
# open as of this writing — see the xnote for the empirical proof and the
# report links). A user pointed at a plain non-TLS MySQL/MariaDB server
# would get "TLS/SSL error: SSL is required, but the server does not
# support it" on every single connection attempt — this is exactly what
# happened on the owner's real Windows box (session 97). 3.3.10 (built from
# source at $MARIADB_MINGW, see the prereq check above) predates the
# change. If this explicit copy weren't here, the closure walk below would
# just as happily pick up the AUR package's 3.4.8 DLL from
# /usr/x86_64-w64-mingw32/bin — same exported symbols, links fine, bug
# comes back silently at runtime. Whenever this project's own mariadb
# dependency changes, rebuild 3.3.10 the same way, never `pacman -S` a
# newer mingw-w64-mariadb-connector-c into this path without re-testing
# against a real non-TLS server first.
cp -f "$MARIADB_MINGW/lib/mariadb/libmariadb.dll" "$DEPLOY/"

QTBIN="$QT_MINGW/bin"
python3 - "$DEPLOY/openyog.exe" "$DEPLOY" "$QTBIN" "/usr/x86_64-w64-mingw32/bin" <<'EOF'
import subprocess, os, sys
exe, deploy, *searchdirs = sys.argv[1:]
system = set("""ntdll kernel32 kernelbase user32 gdi32 gdi32full shell32 ole32 oleaut32
advapi32 comdlg32 comctl32 ws2_32 winmm imm32 msvcrt ucrtbase version wintrust crypt32
secur32 netapi32 userenv setupapi rpcrt4 shlwapi winspool.drv uxtheme dwmapi msimg32
opengl32 wtsapi32 bcrypt cryptbase mpr winsta d3d11 dxgi d2d1 dwrite authz
api-ms- ext-ms""".split())
seen = set()
def walk(path):
    name = os.path.basename(path).lower()
    if name in seen:
        return
    seen.add(name)
    out = subprocess.run(["objdump", "-p", path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("DLL Name:"):
            continue
        dep = line.split("DLL Name:", 1)[1].strip()
        low = dep.lower()
        stem = low[:-4] if low.endswith(".dll") else low
        if stem in system or low.startswith(("api-ms-", "ext-ms-")):
            continue
        if os.path.isfile(os.path.join(deploy, dep)):   # wine resolves names
            continue                                    # case-insensitively
        for d in searchdirs:
            cand = os.path.join(d, dep)
            if os.path.isfile(cand):
                subprocess.run(["cp", "-f", cand, deploy])
                walk(cand)
                break
        else:
            print("MISSING (not a wine builtin?):", dep)
walk(exe)
EOF

# Qt plugins: qwindows for real desktops (owner's machine), qoffscreen for
# wine/headless selftests. A stray copy of the Qt DLLs inside platforms/
# breaks plugin loading on Windows — keep this dir plugins-only.
cp -f "$QT_MINGW/plugins/platforms/qwindows.dll"   "$DEPLOY/platforms/"
cp -f "$QT_MINGW/plugins/platforms/qoffscreen.dll" "$DEPLOY/platforms/"

# imageformats/qico.dll: EVERY toolbar, menu and tree icon is a .ico from
# include/bitmaps, and ICO decoding lives in this plugin — QtGui only has PNG,
# BMP, PPM, XBM and XPM built in. Linux never noticed because the distro Qt
# installs the plugin system-wide. Without it here the icons are compiled into
# the exe and still render as nothing, so this is not optional.
mkdir -p "$DEPLOY/imageformats"
cp -f "$QT_MINGW/plugins/imageformats/qico.dll" "$DEPLOY/imageformats/"

# styles/qmodernwindowsstyle.dll: without ANY style plugin, Qt falls back to
# its own "Fusion"/basic "windows" style — square flat buttons, no native
# theming, no dark-mode awareness. Looked distinctly last-decade in the
# owner's first real-hardware screenshot (session 98). This one plugin
# provides both "windows11" (Fluent, Win 11) and "windowsvista" (Win 7-10)
# style keys (confirmed via `strings` on the DLL) — Qt's platform plugin
# auto-picks the right one for the running OS, no code change needed.
mkdir -p "$DEPLOY/styles"
cp -f "$QT_MINGW/plugins/styles/qmodernwindowsstyle.dll" "$DEPLOY/styles/"

echo "deploy ready: $DEPLOY"
ls -la "$DEPLOY"

# ---- proof of life under wine ------------------------------------------
if command -v wine >/dev/null; then
    rm -f /tmp/openyog-casetest.png
    (cd "$DEPLOY" && timeout 240 wine openyog.exe --casetest)
    echo "wine --casetest exit=$? (0 = pass; PNG at /tmp/openyog-casetest.png)"
else
    echo "wine not installed — skipping the smoke test"
fi

# ---- optional portable zip ---------------------------------------------
if [ "${1:-}" = "--zip" ]; then
    (cd "$DEPLOY" && zip -r "$BUILD/openyog-win-deploy.zip" . -x "GPL.txt")
    echo "zip: $BUILD/openyog-win-deploy.zip"
fi

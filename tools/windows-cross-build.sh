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
BUILD="$ROOT/build-mingw"
DEPLOY="$BUILD/deploy"
QSCINTILLA_LIB="$QT_MINGW/bin/libqscintilla2_qt6.dll"

command -v x86_64-w64-mingw32-cmake >/dev/null || { echo "mingw-w64-cmake missing" >&2; exit 1; }
[ -d "$QT_MINGW/lib/cmake" ] || { echo "Qt mingw tree missing at $QT_MINGW" >&2; exit 1; }
[ -f "$QSCINTILLA_LIB" ] || { echo "QScintilla not installed into the Qt tree ($QSCINTILLA_LIB)" >&2; exit 1; }

# ---- configure + build -------------------------------------------------
x86_64-w64-mingw32-cmake -G Ninja -B "$BUILD" -S "$ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_FIND_ROOT_PATH="/usr/x86_64-w64-mingw32;$QT_MINGW"
cmake --build "$BUILD" --target openyog

# ---- deploy: exe + transitive DLL closure ------------------------------
# Wine/Windows search the exe's own directory first; every non-system import
# must physically sit next to the exe. Walk each DLL's "DLL Name:" imports
# and copy what isn't a wine/Windows builtin from our three supply dirs.
mkdir -p "$DEPLOY/platforms"
cp -f "$BUILD/openyog.exe" "$DEPLOY/"
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

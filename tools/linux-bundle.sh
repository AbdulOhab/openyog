#!/usr/bin/env bash
# linux-bundle.sh — the Linux packages that carry their own libraries:
#
#   openyog-<version>-x86_64.AppImage   runs as it is on most distributions
#   openyog_<version>_amd64.deb         Debian, Ubuntu, Mint …
#   openyog-<version>-1.x86_64.rpm      Fedora, openSUSE …
#
# The .deb and .rpm hold the AppImage's files under /opt/openyog, with
# /usr/bin/openyog, the desktop entry and the icons in the usual places.
#
#   tools/linux-bundle.sh --container   build in a throwaway Ubuntu 22.04
#                                       container (podman or docker)
#   tools/linux-bundle.sh               build here: an Ubuntu 22.04 system or
#                                       container, as root (CI does this)
#
# Built on Ubuntu 22.04, so the packages need glibc 2.35 or newer (Ubuntu
# 22.04+, Debian 12+, Fedora 36+, current Arch, openSUSE Tumbleweed …). Qt
# is aqtinstall's, at the version tools/msys2-ucrt64.lock pins for Windows;
# QScintilla, QtKeychain and libmongoc 2 are built from source; the database
# clients, libssh and OpenSSL are Ubuntu's. linuxdeploy copies every library
# the program loads into the bundle, except the ones every desktop has (glibc,
# OpenGL, X11, fontconfig …), which the .deb and .rpm then depend on.
#
# Environment: OUT (default build-linux), WORK (default /tmp/openyog-bundle,
# kept between runs: Qt and the libraries built from source are reused).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$ROOT/build-linux}"

if [ "${1:-}" = "--container" ]; then
    engine=$(command -v podman || command -v docker) || { echo "needs podman or docker" >&2; exit 1; }
    mkdir -p "$OUT"
    exec "$engine" run --rm -v "$ROOT:/src" -v "$OUT:/out" -e OUT=/out \
        docker.io/library/ubuntu:22.04 /src/tools/linux-bundle.sh
fi

WORK="${WORK:-/tmp/openyog-bundle}"
VERSION=$(sed -n 's/^project(openyog VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
QT_VERSION=$(sed -n 's/.*mingw-w64-ucrt-x86_64-qt6-base-\([0-9.]*\)-[0-9]*-any.*/\1/p' \
    "$ROOT/tools/msys2-ucrt64.lock")
QSCINTILLA=2.14.1
QTKEYCHAIN=0.17.0
MONGOC=2.5.7
LINUXDEPLOY=1-alpha-20251107-1
LINUXDEPLOY_QT=1-alpha-20250213-1
NFPM=2.48.0
[ -n "$VERSION" ] && [ -n "$QT_VERSION" ] || { echo "no version found" >&2; exit 1; }
grep -qs 'VERSION_ID="22.04"' /etc/os-release ||
    { echo "build on Ubuntu 22.04 (or: $0 --container)" >&2; exit 1; }
mkdir -p "$WORK" "$OUT"

# ---- Ubuntu's part ---------------------------------------------------------
export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 LC_ALL=C.UTF-8
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build git curl ca-certificates file pkg-config \
    python3 python3-venv sqlite3 \
    libgl1-mesa-dev libegl1-mesa-dev libxkbcommon-dev libfontconfig1-dev \
    libfreetype6-dev libdbus-1-dev libsecret-1-dev libssl-dev zlib1g-dev \
    libmariadb-dev libmariadb-dev-compat libpq-dev libsqlite3-dev libssh-dev \
    libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
    libxcb-render-util0 libxcb-shape0 libxcb-shm0 libxcb-sync1 libxcb-xfixes0 \
    libxcb-xinerama0 libxcb-xkb1 libxcb-xinput0 libxkbcommon-x11-0 libx11-xcb1 \
    libwayland-client0 libwayland-cursor0 libwayland-egl1 libcups2 >/dev/null

fetch() { # url sha256 file
    [ -f "$3" ] && echo "$2  $3" | sha256sum -c --quiet 2>/dev/null && return
    curl -fsSL -o "$3" "$1"
    echo "$2  $3" | sha256sum -c --quiet || { echo "$3: checksum mismatch" >&2; rm -f "$3"; exit 1; }
}

# ---- Qt ------------------------------------------------------------------
QT="$WORK/qt/$QT_VERSION/gcc_64"
if [ ! -x "$QT/bin/qmake" ]; then
    python3 -m venv "$WORK/aqt"
    "$WORK/aqt/bin/pip" install -q aqtinstall
    "$WORK/aqt/bin/aqt" install-qt -O "$WORK/qt" linux desktop "$QT_VERSION" linux_gcc_64 \
        --archives qtbase icu qtwayland
fi
DEPS="$WORK/deps"
export CMAKE_PREFIX_PATH="$QT:$DEPS" # the environment variable is :-separated

# ---- built from source ------------------------------------------------------
if [ ! -f "$QT/lib/libqscintilla2_qt6.so" ]; then
    fetch "https://www.riverbankcomputing.com/static/Downloads/QScintilla/$QSCINTILLA/QScintilla_src-$QSCINTILLA.tar.gz" \
        dfe13c6acc9d85dfcba76ccc8061e71a223957a6c02f3c343b30a9d43a4cdd4d "$WORK/qscintilla.tar.gz"
    rm -rf "$WORK/qscintilla" && mkdir -p "$WORK/qscintilla"
    tar -xzf "$WORK/qscintilla.tar.gz" -C "$WORK/qscintilla" --strip-components=1
    (cd "$WORK/qscintilla/src" && "$QT/bin/qmake" qscintilla.pro >/dev/null &&
        make -s -j"$(nproc)" >/dev/null && make -s install >/dev/null)
fi
if [ ! -f "$DEPS/lib/cmake/Qt6Keychain/Qt6KeychainConfig.cmake" ]; then
    fetch "https://github.com/frankosterfeld/qtkeychain/archive/refs/tags/$QTKEYCHAIN.tar.gz" \
        3b85c3929034b0a99da777130c34d99f006fcd3a9d56564159399a33fee0e504 "$WORK/qtkeychain.tar.gz"
    rm -rf "$WORK/qtkeychain" && mkdir -p "$WORK/qtkeychain"
    tar -xzf "$WORK/qtkeychain.tar.gz" -C "$WORK/qtkeychain" --strip-components=1
    cmake -S "$WORK/qtkeychain" -B "$WORK/qtkeychain/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_WITH_QT6=ON -DBUILD_TRANSLATIONS=OFF -DBUILD_TEST_APPLICATION=OFF \
        -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
    cmake --build "$WORK/qtkeychain/build" >/dev/null && cmake --install "$WORK/qtkeychain/build" >/dev/null
fi
if [ ! -f "$DEPS/lib/libmongoc2.so" ]; then
    fetch "https://github.com/mongodb/mongo-c-driver/releases/download/$MONGOC/mongo-c-driver-$MONGOC.tar.gz" \
        fdd7c23b3d95f1ea08400e5a72e48419ce03d478aa12b28b4b66066867ee35ee "$WORK/mongoc.tar.gz"
    rm -rf "$WORK/mongoc" && mkdir -p "$WORK/mongoc"
    tar -xzf "$WORK/mongoc.tar.gz" -C "$WORK/mongoc" --strip-components=1
    cmake -S "$WORK/mongoc" -B "$WORK/mongoc/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_STATIC=OFF -DENABLE_SASL=OFF \
        -DENABLE_SNAPPY=OFF -DENABLE_ZSTD=OFF -DENABLE_ZLIB=SYSTEM -DENABLE_SSL=OPENSSL \
        -DENABLE_SHM_COUNTERS=OFF -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
    cmake --build "$WORK/mongoc/build" >/dev/null && cmake --install "$WORK/mongoc/build" >/dev/null
fi

# ---- OpenYog -------------------------------------------------------------------
BUILD="$WORK/build"
APPDIR="$WORK/AppDir"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DQSCINTILLA_INCLUDE_DIR="$QT/include" -DQSCINTILLA_LIBRARY="$QT/lib/libqscintilla2_qt6.so" >/dev/null
cmake --build "$BUILD"
export LD_LIBRARY_PATH="$QT/lib:$DEPS/lib"
QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure -j"$(nproc)"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD" >/dev/null

# ---- the AppImage --------------------------------------------------------------
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY/linuxdeploy-x86_64.AppImage" \
    c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d "$WORK/linuxdeploy"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$LINUXDEPLOY_QT/linuxdeploy-plugin-qt-x86_64.AppImage" \
    15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724 "$WORK/linuxdeploy-plugin-qt"
chmod +x "$WORK/linuxdeploy" "$WORK/linuxdeploy-plugin-qt"
APP_ID=io.github.abdulohab.openyog
(
    cd "$WORK"
    export APPIMAGE_EXTRACT_AND_RUN=1 QMAKE="$QT/bin/qmake" LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
    # wayland next to xcb; offscreen for running without a display (tests)
    export EXTRA_PLATFORM_PLUGINS="libqwayland.so;libqoffscreen.so" PATH="$WORK:$PATH"
    rm -f ./*.AppImage
    ./linuxdeploy --appdir "$APPDIR" --plugin qt --output appimage \
        --desktop-file "$APPDIR/usr/share/applications/$APP_ID.desktop" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_ID.png"
)
APPIMAGE="$OUT/openyog-$VERSION-x86_64.AppImage"
mv "$WORK"/*.AppImage "$APPIMAGE"

# ---- .deb and .rpm, from the same files ------------------------------------------
fetch "https://github.com/goreleaser/nfpm/releases/download/v$NFPM/nfpm_${NFPM}_Linux_x86_64.tar.gz" \
    6d7f9a28b2f397934403950fcbba83f598b08d02edbe79a0815b3b4897341750 "$WORK/nfpm.tar.gz"
tar -xzf "$WORK/nfpm.tar.gz" -C "$WORK" nfpm
PKG="$WORK/pkg"
rm -rf "$PKG" && mkdir -p "$PKG/opt" "$PKG/usr/share"
cp -a "$APPDIR/usr" "$PKG/opt/openyog"
# only what the desktop looks for goes under /usr/share; the rest (the
# bundled libraries' copyright files among it) stays with them in /opt
for d in applications icons metainfo; do
    mv "$PKG/opt/openyog/share/$d" "$PKG/usr/share/$d"
done
cat > "$WORK/nfpm.yaml" <<EOF
name: openyog
arch: amd64
version: $VERSION
release: 1
maintainer: Abdul Ohab <https://github.com/AbdulOhab>
description: |-
  Database client for MySQL, MariaDB, PostgreSQL, SQLite and MongoDB.
  A Qt 6 fork of the SQLyog Community Edition source, with its libraries
  bundled under /opt/openyog.
homepage: https://github.com/AbdulOhab/openyog
license: GPL-3.0-or-later
section: database
contents:
  - src: $PKG/opt/openyog
    dst: /opt/openyog
    type: tree
  - src: /opt/openyog/bin/openyog
    dst: /usr/bin/openyog
    type: symlink
  - src: $PKG/usr/share
    dst: /usr/share
    type: tree
overrides:
  deb:
    depends:
      - libc6 (>= 2.35)
      - libgl1
      - libopengl0
      - libegl1
      - libfontconfig1
      - libfreetype6
      - libx11-6
      - libx11-xcb1
      - libxcb1
      - libdbus-1-3
      - libcom-err2
      - libgpg-error0
      - libgmp10
      - libwayland-client0
      - zlib1g
      - libstdc++6
  rpm:
    depends:
      - libGL.so.1()(64bit)
      - libOpenGL.so.0()(64bit)
      - libEGL.so.1()(64bit)
      - libfontconfig.so.1()(64bit)
      - libfreetype.so.6()(64bit)
      - libX11.so.6()(64bit)
      - libX11-xcb.so.1()(64bit)
      - libxcb.so.1()(64bit)
      - libdbus-1.so.3()(64bit)
      - libcom_err.so.2()(64bit)
      - libgpg-error.so.0()(64bit)
      - libgmp.so.10()(64bit)
      - libwayland-client.so.0()(64bit)
      - libz.so.1()(64bit)
      - libstdc++.so.6()(64bit)
EOF
rm -f "$OUT"/openyog_*_amd64.deb "$OUT"/openyog-*.x86_64.rpm
"$WORK/nfpm" package -f "$WORK/nfpm.yaml" -p deb -t "$OUT/" >/dev/null
"$WORK/nfpm" package -f "$WORK/nfpm.yaml" -p rpm -t "$OUT/" >/dev/null

# the AppImage starts: its GUI too (here: no display, so offscreen)
export QT_QPA_PLATFORM=offscreen APPIMAGE_EXTRACT_AND_RUN=1
"$APPIMAGE" --version
"$APPIMAGE" --icontest | grep -q "icontest: PASS" || { echo "the AppImage's GUI didn't start" >&2; exit 1; }
ls -l "$OUT"

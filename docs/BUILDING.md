# Building OpenYog

## Linux

Requires a C++17 compiler, CMake ≥ 3.21, Ninja, Qt 6, QScintilla (Qt 6
build), the MariaDB client library, libpq, SQLite 3 and zlib. Optional:

- **QtKeychain**: saved passwords go to the system keychain (GNOME Keyring,
  KWallet …); without it they stay base64-encoded in `connections.ini`.
- **libssh**: SSH tunnels.
- **libmongoc 2** (mongo-c-driver): MongoDB connections.

On Arch / Manjaro / CachyOS:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-tools qt6-svg \
    qscintilla-qt6 qtkeychain-qt6 libssh mongo-c-driver mariadb-libs postgresql-libs sqlite

cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure   # unit tests and selftests
./build/openyog
```

### Install

```bash
cmake --install build --prefix /usr/local   # binary, desktop entry, icons
```

`packaging/arch/PKGBUILD` builds the latest commit as an Arch package
(`openyog-git`) and runs the test suite in `check()`:

```bash
cd packaging/arch && makepkg -si
```

## Tests

`ctest` runs the unit tests and the selftests that need no server. The
live selftests run against throwaway servers when CMake is pointed at them;
their fixtures drop and recreate tables (MongoDB's, the `openyog_test_mongo*`
databases), so never point them at real data:

```bash
cmake -S . -B build -G Ninja \
  -DOPENYOG_TEST_MYSQL=127.0.0.1:3306:root:secret:openyog_test \
  -DOPENYOG_TEST_PG=127.0.0.1:5432:postgres:secret:openyog_test \
  -DOPENYOG_TEST_MONGO=127.0.0.1:27017:root:secret \
  -DOPENYOG_TEST_SSH=127.0.0.1:22:tunnel:secret   # an SSH server that allows forwarding
```

CI (`.github/workflows/ci.yml`) runs the build and every test against
throwaway MariaDB, PostgreSQL and MongoDB containers and an SSH server, a
`clang-format` check, and the tests again on a build with AddressSanitizer,
UndefinedBehaviorSanitizer and LeakSanitizer, which fails on any report.

## Windows (cross-compiled from Linux)

The Windows build is cross-compiled with mingw-w64 against MSYS2's UCRT64
packages (Qt, QScintilla, QtKeychain, the database clients, libssh,
libmongoc, NSIS), at the versions `tools/msys2-ucrt64.lock` pins. It needs
`mingw-w64-gcc`, CMake, Ninja, Python 3, wine, and a host Qt of the same
version as the pinned one (for moc, rcc and uic):

```bash
sudo pacman -S --needed mingw-w64-gcc wine
tools/windows-cross-build.sh --zip --installer
```

This produces, in `build-mingw/`:

- `deploy/`: `openyog.exe`, every DLL it loads and the Qt plugins it needs;
- `openyog-<version>-windows-x64.zip`: that folder, to unpack and run;
- `openyog-<version>-windows-x64-setup.exe`: the installer, built from
  `tools/openyog.nsi` with NSIS under wine.

The script then runs selftests under wine in a prefix of its own (saved
passwords in Windows Credential Manager, SQL parsing, icons), installs the
setup silently, runs the installed program and uninstalls it again, which
must leave nothing behind. Given `OPENYOG_TEST_SSH`, `OPENYOG_TEST_PG` and
`OPENYOG_TEST_MONGO` (see the script's header) it also runs a query through
an SSH tunnel and the MongoDB selftest. On Windows, SSH host keys are read
from `%USERPROFILE%\.ssh\known_hosts`.

Keeping the Windows libraries current:

```bash
# add a package (and the dependencies it needs) without moving the locked ones
tools/fetch-msys2-sysroot.py --add <name>
# move everything to the repository's current versions, Qt pinned to the host's
tools/fetch-msys2-sysroot.py --update --pin qt6-base=<version>-<pkgrel>
```

## Releases

Pushing a `v*` tag runs CI on it and then publishes a GitHub Release with the
Windows installer and zip and the Arch package; the release text comes from
`docs/release-notes/<tag>.md`.

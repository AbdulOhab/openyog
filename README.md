# OpenYog

Open source, cross-platform (Linux + Windows) desktop client for **MySQL,
MariaDB, PostgreSQL, and SQLite**: a **GPL-3.0 fork of the SQLyog Community
Edition source** (13.3.1 GA), with SQLyog's original Windows-only UI layer
replaced by a **Qt 6** front end on top of SQLyog's portable `wy*` core.

> **Not affiliated with, endorsed by, or connected to Webyog or Idera, Inc.**
> "SQLyog" and the SQLyog logo are trademarks of their respective owners and are
> **not** used in this project's branding or user-facing strings. This is an
> independent community fork of the GPL-licensed source code, distributed under
> a different name.

## Status

Early but usable for day-to-day work against a local server. Working now:

- **Backends**: MySQL/MariaDB, PostgreSQL, and SQLite, all through one shared
  `IDbConnection` abstraction, picked per connection. MongoDB support is not
  started.
- **Connections**: SQLyog-style *Connect to MySQL Host* dialog, extended with
  a driver picker for MySQL/MariaDB, PostgreSQL, and SQLite (saved
  connections, clone / rename / delete, Test Connection), SSL client
  certificates, and SSH tunnels (password or private key, host keys checked
  against `~/.ssh/known_hosts`). Saved passwords live in the OS keychain;
  the open connections and their query tabs come back on the next start.
  The HTTP tunnel and Advanced tabs are still placeholders.
- **Object browser**: connection → databases → Tables / Views / Stored Procs /
  Functions / Triggers / Events (lazy-loaded); tables expand to columns.
  Postgres connections can browse and switch between every database on the
  server, not just the one you connected to.
- **Query editor**: line-number gutter, SQL highlighting, multiple *Query N*
  tabs, threaded execution (the UI never blocks), multi-statement runs that
  open multiple result grids, and a History tab.
- **Data grid**: fully staged editing. Edited cells turn amber, new rows
  green, rows marked for deletion red; one **Apply** commits them in a single
  transaction, **Revert** drops them.
- **Schema**: Create Table (F4), Alter Table (F6), Manage Indexes (F7),
  Foreign Keys (F10), Rename / Drop / Truncate / Duplicate Table, Create /
  Drop / Copy Database, all dialect-aware across the three backends.
- **Schema Designer** (File ▸ New Schema Designer, Ctrl+Alt+D, or a
  database's right-click menu): the database's tables and foreign keys as a
  diagram, laid out automatically. Drag tables around, zoom, find or hide a
  table, and export the diagram as an image; double-click a table to alter
  it. It only reads the schema: changes go through the usual dialogs.
- **Query Builder** (File ▸ New Query Builder, Ctrl+K, or a database's
  right-click menu): build a SELECT by pointing. Add tables from a list (they
  join along their foreign keys on their own, or drag a column onto another
  table's to join by hand; right-click a join for LEFT / RIGHT), tick
  columns, then give them aliases, aggregates, sorting and conditions in the
  grid. The SQL follows every change and goes to a query editor or runs
  straight away.
- **Schema Sync** (Powertools ▸ Schema Synchronization Tool, Ctrl+Q): compare
  two databases of the same kind, on the same connection or two different
  ones, and get the script that makes the target's tables, columns, keys and
  foreign keys match the source's. Tick what to bring over; dropping a table
  or a column starts unticked. The script goes to the target's query editor
  or a file: nothing is run for you. SQLite changes it can't make in place
  (altering a column) are listed instead of scripted.
- **Export**: Backup database as a SQL dump; export a result set as CSV.
- **Themes**: light (SQLyog "Flat" palette) and dark.

## Build (Linux)

Requires a C++17 compiler, CMake ≥ 3.21, Ninja, Qt 6, QScintilla (Qt 6
build), MariaDB client headers, libpq, and SQLite3. QtKeychain is optional:
with it, saved connection passwords go to the OS keychain (GNOME Keyring,
KWallet …); without it they stay base64-encoded in `connections.ini`.
libssh is optional too: it adds SSH tunnels (Linux only for now). On
Arch / CachyOS:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-tools qt6-svg qscintilla-qt6 qtkeychain-qt6 libssh mariadb postgresql-libs sqlite

cmake -S . -B build-cmake -G Ninja
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure   # unit tests + selftests

./build-cmake/openyog
```

CI (`.github/workflows/ci.yml`) runs the same build, `ctest` (including
the live MySQL/PostgreSQL selftests against throwaway MariaDB and PostgreSQL
containers), and a `clang-format` check on every push and pull request; it
runs the tests a second time on a build with AddressSanitizer,
UndefinedBehaviorSanitizer and LeakSanitizer, which fails on any report. To
run the live selftests locally, point CMake at throwaway databases — their
fixtures drop and recreate tables:

```bash
cmake -S . -B build-cmake -G Ninja \
  -DOPENYOG_TEST_MYSQL=127.0.0.1:3306:root:secret:openyog_test \
  -DOPENYOG_TEST_PG=127.0.0.1:5432:postgres:secret:openyog_test
```

## Install (Linux)

```bash
cmake --install build-cmake --prefix /usr/local   # binary, desktop entry, icons
```

On Arch / Manjaro, `packaging/arch/PKGBUILD` builds the latest commit as a
package (`openyog-git`), running the test suite in `check()`:

```bash
cd packaging/arch && makepkg -si
```

## Build (Windows, cross-compiled from Linux)

Cross-compiled with mingw-w64 against MSYS2's UCRT64 packages (Qt,
QScintilla, QtKeychain and the database clients, at the versions
`tools/msys2-ucrt64.lock` pins). Needs `mingw-w64-gcc`, cmake, ninja,
python3 and a host Qt of the same version as the pinned one (it supplies
moc/rcc/uic):

```bash
sudo pacman -S --needed mingw-w64-gcc wine   # wine: optional, runs the selftests
tools/windows-cross-build.sh --zip
```

Produces `build-mingw/deploy/` (the exe + every DLL it loads + Qt
plugins) and `build-mingw/openyog-win-deploy.zip`: copy the zip's whole
contents to a Windows machine, not just `openyog.exe`. With wine installed
the script then runs selftests on the result in a wine prefix of its own,
among them `--keychaintest` (saved passwords go to Windows Credential
Manager). Tested under wine only so far, not on real Windows hardware.
SSH tunnels aren't in the Windows build yet.

When the host Qt moves to a new version, pin the same one for Windows:
`tools/fetch-msys2-sysroot.py --update --pin qt6-base=<version>-<pkgrel>`.

## Licensing

- This project: **GPL-3.0-or-later** (upstream is GPL-2.0-or-later; GPL-3 was
  chosen for compatibility with the bundled OpenSSL 3 / Apache-2.0).
- Upstream copyright and GPL notices are preserved.
- The proprietary `htmlayout` component that shipped with SQLyog is **not**
  included and is never linked; panes that used it are being rebuilt with
  native Qt widgets.
- Full license text: [LICENSE.md](LICENSE.md).

## Credits

Built on the SQLyog Community Edition source released by Webyog under the
GPL. The `wy*` portable core, SQL maker, and connection layer are reused
largely unchanged; the `qt/` tree, and the PostgreSQL/SQLite backends, are
new. The application itself is built on Qt 6, MariaDB Connector/C, libpq,
and SQLite.

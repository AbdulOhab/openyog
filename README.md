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
  connections, clone / rename / delete, Test Connection); HTTP / SSH / SSL /
  Advanced are still placeholders.
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
- **Export**: Backup database as a SQL dump; export a result set as CSV.
- **Themes**: light (SQLyog "Flat" palette) and dark.

## Build (Linux)

Requires a C++17 compiler, CMake ≥ 3.21, Ninja, Qt 6, MariaDB client headers,
libpq, and SQLite3. On Arch / CachyOS:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-tools qt6-svg mariadb postgresql-libs sqlite

cmake -S . -B build-cmake -G Ninja
cmake --build build-cmake

./build-cmake/openyog
```

A Windows build via mingw-w64 is planned; the core already compiles portably.

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

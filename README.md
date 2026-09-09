# OpenYog

A cross-platform (Linux + Windows) MySQL / MariaDB GUI client — a **GPL-3.0
fork of the SQLyog Community Edition source** (13.3.1 GA) with the Windows-only
UI layer replaced by a **Qt 6** front end on top of SQLyog's portable `wy*`
core.

> **Not affiliated with, endorsed by, or connected to Webyog or Idera, Inc.**
> "SQLyog" and the SQLyog logo are trademarks of their respective owners and are
> **not** used in this project's branding or user-facing strings. This is an
> independent community fork of the GPL-licensed source code, distributed under
> a different name.

## Status

Early but usable for day-to-day work against a local server. Working now:

- **Connections** — SQLyog-style *Connect to MySQL Host* dialog (saved
  connections, clone / rename / delete, Test Connection); MySQL tab live,
  HTTP / SSH / SSL / Advanced are placeholders.
- **Object browser** — connection → databases → Tables / Views / Stored Procs /
  Functions / Triggers / Events (lazy-loaded); tables expand to columns.
- **Query editor** — line-number gutter, SQL highlighting, multiple *Query N*
  tabs, threaded execution (UI never blocks), multi-statement → multiple result
  grids, History tab.
- **Data grid** — fully staged editing: edited cells (amber), new rows (green),
  rows marked for deletion (red); one **Apply** commits them in a single
  transaction, **Revert** drops them.
- **Schema** — Create Table (F4), Alter Table (F6), Manage Indexes (F7),
  Foreign Keys (F10), Rename / Drop / Truncate / Duplicate Table, Create /
  Drop / Copy Database.
- **Export** — Backup database as a SQL dump; export a result set as CSV.
- **Themes** — light (SQLyog "Flat" palette) and dark.

See `FEATURES.md` for the full parity tracker and `plan.md` for the roadmap.

## Build (Linux)

Requires a C++17 compiler, CMake ≥ 3.21, Ninja, Qt 6, and MariaDB client
headers. On Arch / CachyOS:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-tools qt6-svg mariadb

cmake -S . -B build-cmake -G Ninja
cmake --build build-cmake

./build-cmake/openyog
```

A Windows build via mingw-w64 is planned; the core already compiles portably.

## Licensing

- This project: **GPL-3.0-or-later** (upstream is GPL-2.0-or-later; GPL-3 was
  chosen for compatibility with the bundled OpenSSL 3 / Apache-2.0).
- Upstream copyright and GPL notices are preserved. Modifications are recorded
  in `WORKLOG.md`.
- The proprietary `htmlayout` component that shipped with SQLyog is **not**
  included and is never linked; panes that used it are being rebuilt with
  native Qt widgets.

## Credits

Built on the SQLyog Community Edition source released by Webyog under the GPL.
The `wy*` portable core, SQL maker, and connection layer are reused largely
unchanged; the `qt/` tree is new.

#!/usr/bin/env python3
"""fetch-msys2-sysroot.py — the Windows libraries tools/windows-cross-build.sh
links against, from MSYS2's UCRT64 repository: Qt, QScintilla, QtKeychain,
the MariaDB/PostgreSQL/SQLite clients and everything they depend on, unpacked
into one tree that the cross toolchain (tools/mingw-w64.cmake) uses as its
sysroot.

    tools/fetch-msys2-sysroot.py DIR
        unpack the packages tools/msys2-ucrt64.lock lists into DIR/ucrt64,
        each checked against the SHA-256 the lock records (downloads are
        kept in DIR/cache for the next run)
    tools/fetch-msys2-sysroot.py --update [--pin NAME=VERSION ...]
        rewrite the lock from the repository as it is now: the packages
        below plus their dependencies, at the versions it lists, except for
        each --pin (e.g. --pin qt6-base=6.11.2-2)

The repository is rolling, hence the lock: the Windows Qt has to be the
version of the host Qt that supplies moc/rcc/uic, so qt6-base usually needs a
pin. A pinned package is older than the repository's index, which only lists
current versions: its SHA-256 is the one it downloaded with, over HTTPS.
MSYS2's packages are UCRT-based, like Arch's mingw-w64-gcc, and built by the
same GCC line, so that compiler's binaries can load them."""
import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

REPO = "https://repo.msys2.org/mingw/ucrt64/"
PREFIX = "mingw-w64-ucrt-x86_64-"
LOCK = Path(__file__).with_name("msys2-ucrt64.lock")
WANTED = ["qt6-base", "qscintilla-qt6", "qtkeychain-qt6", "libmariadbclient",
          "postgresql", "sqlite3"]
# dependencies of a package's scripts or bindings, not of any DLL loaded here:
# glib2's code generators (python), sqlite3's Tcl extension
SKIP = ["python", "python-packaging", "tcl"]


def fetch(url, dest):
    with urllib.request.urlopen(url, timeout=120) as r, open(dest, "wb") as f:
        shutil.copyfileobj(r, f)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def strip_version(dep):
    for op in (">", "<", "="):
        dep = dep.split(op)[0]
    return dep


def read_fields(text):
    """pacman's desc format: %KEY% lines, then values until a blank line"""
    fields, key = {}, None
    for line in text.splitlines():
        if line.startswith("%") and line.endswith("%"):
            key = line.strip("%")
            fields[key] = []
        elif line and key:
            fields[key].append(line)
    return fields


def parse_db(db_file):
    """name -> {version, filename, sha256, depends}; and provided name -> name"""
    pkgs, provides = {}, {}
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["tar", "-xf", db_file, "-C", tmp], check=True)
        for desc in Path(tmp).glob("*/desc"):
            f = read_fields(desc.read_text())
            name = f["NAME"][0]
            pkgs[name] = {"version": f["VERSION"][0], "filename": f["FILENAME"][0],
                          "sha256": f["SHA256SUM"][0],
                          "depends": [strip_version(d) for d in f.get("DEPENDS", [])]}
            for p in f.get("PROVIDES", []):
                provides.setdefault(strip_version(p), name)
    return pkgs, provides


def pinned_package(name, version, cache):
    """a pin: the file at that version, its hash and its own dependency list"""
    filename = f"{name}-{version}-any.pkg.tar.zst"
    dest = cache / filename
    if not dest.exists():
        print(f"fetch {filename} (pinned)", flush=True)
        fetch(REPO + filename, dest)
    info = subprocess.run(["tar", "-xOf", dest, ".PKGINFO"], check=True,
                          capture_output=True, text=True).stdout
    depends = [strip_version(l.split("=", 1)[1].strip())
               for l in info.splitlines() if l.startswith("depend =")]
    return {"version": version, "filename": filename, "sha256": sha256(dest),
            "depends": depends}


def update(pins, cache):
    db = cache / "ucrt64.db"
    fetch(REPO + "ucrt64.db", db)
    pkgs, provides = parse_db(db)
    for name, version in pins.items():
        pkgs[PREFIX + name] = pinned_package(PREFIX + name, version, cache)
    todo, seen = [PREFIX + w for w in WANTED], []
    while todo:
        want = todo.pop()
        name = want if want in pkgs else provides.get(want)
        if name is None:
            sys.exit(f"not in the repository: {want}")
        if name in seen:
            continue
        seen.append(name)
        # only UCRT64 packages: plain MSYS2 ones (winpty, ...) are tools for
        # the MSYS2 shell, not libraries anything here links
        todo += [d for d in pkgs[name]["depends"]
                 if d.startswith(PREFIX) and d[len(PREFIX):] not in SKIP]
    pin_note = ", ".join(f"{n}={v}" for n, v in pins.items()) or "none"
    lines = ["# MSYS2 UCRT64 packages for the Windows cross build, one file and its",
             "# SHA-256 per line. Written by tools/fetch-msys2-sysroot.py --update;",
             f"# pins: {pin_note}"]
    lines += [f"{pkgs[n]['filename']} {pkgs[n]['sha256']}" for n in sorted(seen)]
    LOCK.write_text("\n".join(lines) + "\n")
    print(f"{LOCK}: {len(seen)} packages")


def unpack(out, cache):
    entries = [l.split() for l in LOCK.read_text().splitlines()
               if l.strip() and not l.startswith("#")]
    root = out / "ucrt64"
    if root.exists():
        shutil.rmtree(root)
    for filename, digest in entries:
        f = cache / filename
        if not f.exists() or sha256(f) != digest:
            print(f"fetch {filename}", flush=True)
            fetch(REPO + filename, f)
        got = sha256(f)
        if got != digest:
            f.unlink()
            sys.exit(f"{filename}: SHA-256 {got}, the lock records {digest}")
        # files live under ucrt64/; pacman's own metadata stays out
        subprocess.run(["tar", "-xf", f, "-C", out, "--no-same-owner",
                        "--exclude=.PKGINFO", "--exclude=.BUILDINFO", "--exclude=.MTREE",
                        "--exclude=.INSTALL"], check=True)
    print(f"{len(entries)} packages in {root}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dir", nargs="?", help="where to unpack (DIR/ucrt64, DIR/cache)")
    ap.add_argument("--update", action="store_true", help="rewrite the lock file")
    ap.add_argument("--pin", action="append", default=[], metavar="NAME=VERSION")
    args = ap.parse_args()
    if args.update:
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(args.dir).resolve() / "cache" if args.dir else Path(tmp)
            cache.mkdir(parents=True, exist_ok=True)
            update(dict(p.split("=", 1) for p in args.pin), cache)
        return
    if not args.dir:
        ap.error("DIR is required")
    out = Path(args.dir).resolve()
    cache = out / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    unpack(out, cache)


if __name__ == "__main__":
    main()

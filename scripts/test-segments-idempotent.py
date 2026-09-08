#!/usr/bin/env python3
"""tbc-segments --write must be a no-op once the metadata already says it.

A backfill that is not idempotent rewrites the whole metadata file every time
it is run. On a 475,000-field capture that is gigabytes of churn to store rows
the file already holds, and over NFS it is hours of it.

The interesting case is a decode whose decoder wrote no events of its own, so
tbc-segments reconstructs them: that is the path that used to report "changed"
unconditionally. None of the shipped fixtures produce an event, so this injects
an RF gap to make one.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path


def run(binary: Path, db: Path, tbc: Path) -> str:
    result = subprocess.run(
        [str(binary), str(db), "--tbc", str(tbc),
         "--write", "--write-segments", "--json", "-"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit(f"tbc-segments exited {result.returncode}")
    return result.stderr


def inject_gap(db: Path) -> None:
    """Push the second half of the capture forward in the RF file."""
    with sqlite3.connect(db) as conn:
        rows = conn.execute(
            "SELECT field_id, file_loc FROM field_record ORDER BY field_id"
        ).fetchall()
        if len(rows) < 4 or rows[0][1] is None or rows[1][1] is None:
            raise SystemExit("fixture has no usable file_loc values")
        step = rows[1][1] - rows[0][1]
        split = rows[len(rows) // 2][0]
        conn.executemany(
            "UPDATE field_record SET file_loc = ? WHERE field_id = ?",
            [(loc + 10 * step, fid) for fid, loc in rows if fid >= split],
        )


def event_count(db: Path) -> int:
    with sqlite3.connect(db) as conn:
        return conn.execute("SELECT COUNT(*) FROM decoder_event").fetchone()[0]


def restamp_commit(db: Path) -> None:
    """Make the stored rows look like they came from a different build."""
    with sqlite3.connect(db) as conn:
        rows = conn.execute("SELECT event_id, detail_json FROM decoder_event").fetchall()
        for event_id, detail in rows:
            obj = json.loads(detail)
            obj["commit"] = "0000000-from-another-build"
            conn.execute("UPDATE decoder_event SET detail_json = ? WHERE event_id = ?",
                         (json.dumps(obj), event_id))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("db", type=Path)
    parser.add_argument("tbc", type=Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        db, tbc = work / "t.tbc.db", work / "t.tbc"
        shutil.copy(args.db, db)
        shutil.copy(args.tbc, tbc)
        inject_gap(db)

        first = run(args.binary, db, tbc)
        if "Metadata written" not in first:
            raise SystemExit("first run stored nothing; the fixture proves nothing")
        if event_count(db) == 0:
            raise SystemExit("first run reconstructed no decoder events")

        # Second run: everything it would store is already stored
        before = db.read_bytes()
        second = run(args.binary, db, tbc)
        if "Nothing to write" not in second:
            sys.stderr.write(second)
            raise SystemExit("second run rewrote the metadata; --write is not idempotent")
        if db.read_bytes() != before:
            raise SystemExit("second run changed the database despite reporting no change")

        # Third run: the stored rows carry another build's commit string, which
        # is provenance, not content, and must not force a rewrite
        restamp_commit(db)
        before = db.read_bytes()
        third = run(args.binary, db, tbc)
        if "Nothing to write" not in third:
            sys.stderr.write(third)
            raise SystemExit("a differing build commit forced a rewrite")
        if db.read_bytes() != before:
            raise SystemExit("a differing build commit changed the database")

    print("tbc-segments --write is idempotent")
    return 0


if __name__ == "__main__":
    sys.exit(main())

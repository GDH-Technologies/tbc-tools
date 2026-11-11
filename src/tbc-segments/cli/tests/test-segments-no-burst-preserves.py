#!/usr/bin/env python3
"""--no-burst must not erase a burst amplitude the metadata already holds.

tbc-segments stores a walk's metrics by replacing each field's picture_metrics
row wholesale, so a metric the walk did not measure used to be written back as
NULL. With --no-burst that silently destroyed a good burst_amp_ire column, and
recovering it costs another full walk with the chroma read -- exactly the I/O
--no-burst exists to avoid.

Not measuring a metric is not the same as erasing it: this asserts a --no-burst
walk leaves the stored burst untouched while still updating everything it did
measure.
"""

from __future__ import annotations

import argparse
import math
import shutil
import sqlite3
import subprocess
import sys
import tempfile
from contextlib import closing
from pathlib import Path


def run(binary: Path, db: Path, tbc: Path, *extra: str) -> str:
    result = subprocess.run(
        [str(binary), str(db), "--tbc", str(tbc), "--force-walk", "--write",
         "--json", "-", *extra],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit(f"tbc-segments exited {result.returncode}")
    return result.stderr


def metrics(db: Path) -> dict[int, tuple]:
    """field_id -> (burst, luma, field_diff, blanking, sync_tip, noise)."""
    with closing(sqlite3.connect(db)) as conn, conn:
        rows = conn.execute(
            "SELECT field_id, burst_amp_ire, luma_mean_ire, field_diff_ire, "
            "blanking_dev_ire, sync_tip_dev_ire, noise_ire FROM picture_metrics "
            "ORDER BY field_id"
        ).fetchall()
    return {row[0]: row[1:] for row in rows}


def finite(value) -> bool:
    return value is not None and math.isfinite(value)


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

        # A full walk first: the shipped fixtures predate the schema and hold no
        # picture_metrics table at all, so there is nothing to preserve until one
        # run has measured the burst.
        run(args.binary, db, tbc)
        before = metrics(db)
        measured = [f for f, m in before.items() if finite(m[0])]
        if len(measured) < len(before) // 2 or not measured:
            raise SystemExit(
                f"the full walk measured burst for only {len(measured)} of "
                f"{len(before)} fields; the fixture proves nothing")

        # Now the same walk with --no-burst. Everything it measures is rewritten;
        # the burst it did not measure must survive.
        stderr = run(args.binary, db, tbc, "--no-burst")
        after = metrics(db)

        if set(after) != set(before):
            raise SystemExit("--no-burst changed which fields have metrics rows")

        lost = [f for f in measured if not finite(after[f][0])]
        if lost:
            raise SystemExit(
                f"--no-burst erased the stored burst amplitude for {len(lost)} "
                f"of {len(measured)} fields (first: field {lost[0]})")

        changed = [f for f in measured if after[f][0] != before[f][0]]
        if changed:
            raise SystemExit(
                f"--no-burst altered the stored burst amplitude for {len(changed)} "
                f"fields (first: field {changed[0]}, "
                f"{before[changed[0]][0]} -> {after[changed[0]][0]})")

        # The run must genuinely have written, not quietly skipped the metrics
        for field, values in after.items():
            if not any(finite(v) for v in values[1:]):
                raise SystemExit(f"field {field} lost every non-burst metric")

        if "kept the stored burst amplitude" not in stderr:
            sys.stderr.write(stderr)
            raise SystemExit("--no-burst preserved the burst without saying so")

    print(f"--no-burst preserved the stored burst amplitude for "
          f"{len(measured)} fields")
    return 0


if __name__ == "__main__":
    sys.exit(main())

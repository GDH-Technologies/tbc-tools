#!/usr/bin/env python3
"""Extract a shot-boundary detection fixture from a .tbc.db.

Reads the per-field picture metrics around hand-labelled points of a decode
and writes them as a small JSON fixture for testsegments' shot boundary test. Only
scalar metrics are copied (no picture, no audio), in short windows.

    extract-shot-boundary-fixture.py --db capture.tbc.db --labels labels.txt \
        --out test-data/segments/<name>.json --description "..."

labels.txt holds one point per line: a label and a timecode (HH:MM:SS.ss from
field 0, at the decode's field rate), e.g.

    shot_boundary  00:05:13.58
    look_alike     01:23:38.51

"shot_boundary" points must be found (Glossary: shot boundary,
docs/Tools/tbc-segments-glossary.md#shot-boundary); "look_alike" points
(flashes, cuts in a picture filmed off a screen) must not be. The database is
opened read-only.
"""

import argparse
import json
import sqlite3
import sys

METRICS = ("luma_mean_ire", "field_diff_ire", "noise_ire", "blanking_dev_ire", "sync_tip_dev_ire")
KEYS = ("lumaMeanIre", "fieldDiffIre", "noiseIre", "blankingDevIre", "syncTipDevIre")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", required=True)
    parser.add_argument("--labels", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--description", default="")
    parser.add_argument("--radius", type=int, default=40, help="fields either side of each point (40)")
    args = parser.parse_args()

    con = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    system = con.execute("SELECT system FROM capture LIMIT 1").fetchone()[0]
    field_rate = 50.0 if system in ("PAL", "SECAM") else 60000.0 / 1001.0
    total = con.execute("SELECT COUNT(*) FROM picture_metrics").fetchone()[0]

    windows = []
    with open(args.labels, encoding="utf-8") as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            label, timecode = line.split()
            hours, minutes, seconds = timecode.split(":")
            field = int(round((int(hours) * 3600 + int(minutes) * 60 + float(seconds)) * field_rate))
            start = max(0, field - args.radius)
            end = min(total, field + args.radius + 1)
            rows = con.execute(
                f"SELECT {', '.join(METRICS)} FROM picture_metrics WHERE field_id >= ? AND field_id < ? ORDER BY field_id",
                (start, end)).fetchall()
            if len(rows) != end - start:
                sys.exit(f"missing picture metrics around {timecode}")
            metrics = {key: [None if row[i] is None else round(row[i], 3) for row in rows] for i, key in enumerate(KEYS)}
            windows.append({"label": label, "field": field - start, "fields": end - start, "metrics": metrics})

    fixture = {
        "description": args.description,
        "system": system,
        "radius": args.radius,
        "toleranceFields": 2,
        "windows": windows,
    }
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(fixture, handle, separators=(",", ":"))
        handle.write("\n")
    print(f"{len(windows)} windows written to {args.out}")


if __name__ == "__main__":
    main()

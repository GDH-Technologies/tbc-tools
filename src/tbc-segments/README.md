# tbc-segments

**Recording-boundary analysis of TBC metadata and fields**

## Overview

tbc-segments finds where a recording was started, stopped, or interrupted on a
tape or disc capture. It reads the decode metadata (`.tbc.db`, or the legacy
`.tbc.json`) through the tbc library and reports:

- **gapless sections**: runs of fields whose RF file offsets advance by the
  nominal field length (the same rule tbc-audio-align uses to segment audio);
- **events**: `gap` (a jump in the RF offset), `sync_loss` (a run of low
  `syncConf`), `parity_break` (a flipped or repeated field parity),
  `skipped_field` (the decoder duplicated or dropped a field), `dropout_storm`
  (a run of fields whose active area is mostly dropouts).

With `--tbc <luma.tbc>` it also walks the raw fields and measures, per field,
the active luma mean, the same-parity field difference (n vs n−2), the
back-porch noise, the blanking and sync-tip level errors, and the colour-burst
amplitude (from `<luma>_chroma.tbc` when present, else from the luma TBC). Those
add `scene_change`, `noise`, `blank_video` and `no_burst` events, and `--per-field`
dumps the arrays for downstream classifiers.

The output is JSON (schema 1). Fields are 0-based and half-open, seconds count
from field 0 of the file (`range.startField` lets a consumer rebase a
`--start/--length` window). It needs no GPU and no ffmpeg; the field walk is
I/O-bound (about 478 KB per NTSC field).

## Usage

```
tbc-segments <input.tbc.db|input.tbc.json> [options]

  --json <file|->             report destination (default: stdout)
  --start <frame>             first frame, 1-based (as tbc-export-metadata)
  --length <frames>           number of frames
  --rf-sample-rate-hz <Hz>    RF capture rate for the nominal field length;
                              unset: self-calibrated from the median field delta
  --gap-tolerance <f>         deviation from nominal that starts a section (0.333)
  --sync-conf-threshold <n>   syncConf below this is sync loss (50)
  --min-run-fields <n>        minimum run for sync_loss / dropout_storm /
                              field-data runs (2)
  --dropout-storm-threshold <f>  active-area dropout coverage (0.25)
  --tbc <luma.tbc>            also walk the raw fields
  --chroma-tbc <file>         burst source (default <luma>_chroma.tbc if present)
  -t, --threads <n>           worker threads for the walk (logical CPUs)
  --scene-threshold-ire <x>   same-parity difference marking a scene change (12)
  --noise-threshold-ire <x>   back-porch noise marking snow (6)
  --blank-luma-ire <x>        active luma at or below which a quiet field is blank (5)
  --per-field                 include per-field arrays
  --summary                   human summary on stderr
  -d, --debug / -q, --quiet   standard logging options
```

Exit status is 0 on success and 1 on any failure (unreadable metadata, no
fields, an invalid `--start/--length`, a TBC whose field count disagrees with
the metadata, an unwritable output).

### Why the RF rate matters

`fileLoc` is an offset into the **RF capture**, but `videoParameters.sampleRate`
is the **TBC output** rate (4×fsc), so the nominal samples-per-field cannot be
derived from the metadata alone. Pass the capture rate (`--rf-sample-rate-hz
40000000` for a 40 MSps CXADC capture) when you know it; otherwise the tool takes
the median of the positive field-to-field deltas and reports
`timing.nominalSource: "median-delta"` and the implied RF rate.

Legacy JSON written by a 32-bit `fileLoc` writer wraps negative on long
captures; the tool undoes that (`timing.fileLocRolloverFixups`).

## Output

```json
{"schemaVersion": 1,
 "tool": {"name": "tbc-segments", "branch": "...", "commit": "..."},
 "input": {"path": "...", "kind": "sqlite|json"},
 "video": {"system": "NTSC", "tapeFormat": "VHS", "fieldRate": 59.94, "secondsPerField": 0.01668,
           "fieldWidth": 910, "fieldHeight": 263, "numberOfFields": 123456, "numberOfFrames": 61728,
           "frameOffset": 0, "isFirstFieldFirst": true},
 "timing": {"nominalSamplesPerField": 667333.3, "nominalSource": "median-delta|rf-sample-rate-hz|diskLoc|none",
            "impliedRfSampleRateHz": 4.0e7, "gapDetection": "fileLoc|diskLoc|unavailable", "fileLocRolloverFixups": 0},
 "range": {"startFrame": 1, "lengthFrames": 61728, "startField": 0, "endFieldExclusive": 123456, "secondsOrigin": "field0"},
 "thresholds": {...},
 "fieldData": {"enabled": false},
 "sections": [{"startField": 0, "endFieldExclusive": 5000, "startSeconds": 0.0, "endSeconds": 83.4}],
 "events": [{"kind": "gap", "startField": 5000, "endFieldExclusive": 5001, "startSeconds": 83.4, "endSeconds": 83.42,
             "severity": 0.9, "detail": {"seamAfterField": 4999, "deltaSamples": 3336667, "deltaFields": 5.0, "missingFields": 4}}],
 "counts": {"gap": 1, "sync_loss": 0, "parity_break": 0, "skipped_field": 0, "dropout_storm": 0,
            "scene_change": 0, "noise": 0, "blank_video": 0, "no_burst": 0},
 "perField": {...},        // with --per-field
 "fieldMetrics": {...}}    // with --per-field --tbc
```

`--scene-threshold-ire` and `--noise-threshold-ire` are floors: the value applied
is the larger of the option and 2.5× the file's own median field difference /
back-porch noise, because real tape (an EP VHS decode especially) raises both
baselines. The applied values are reported under `fieldData.thresholds` as
`sceneThresholdIreEffective` and `noiseThresholdIreEffective`. A scene-change
candidate whose field, or whose n−2 partner, is inside a noise run is dropped:
snow makes every field difference enormous and the `noise` run already says so.

`gap` events place `startField` on the first field **after** the seam, so a
downstream boundary belongs between `seamAfterField` and `startField`. A
`noise` or `no_burst` run that coincides with a `gap` or `sync_loss` raises that
event's severity (`detail.fieldDataCoincident`).

## Tests

`testsegments` (ctest) synthesises metadata in memory (gap, parity and skipped
fields, sync-loss runs, dropout storms, a 32-bit rollover round-tripped through
JSON, `--start/--length` clipping) and a small synthetic luma + chroma TBC
(scene change, snow, blank picture, missing burst, single- vs multi-threaded
equality, field-count mismatch refused). `tbc-segments-smoke` runs the shipped
`test-data/ntsc/ve-snw-cut` fixture through both tiers.

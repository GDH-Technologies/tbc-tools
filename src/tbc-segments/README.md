# tbc-segments

**Recording-boundary analysis of TBC metadata and fields**

## Overview

tbc-segments finds where a recording was started, stopped, or interrupted on a
tape or disc capture. It reads the decode metadata (`.tbc.db`, or the legacy
`.tbc.json`; a `.tbc.json` with a `.tbc.db` sibling opens the database) through
the tbc library and reports:

- **gapless sections**: runs of fields whose RF file offsets advance by the
  nominal field length (the same rule tbc-audio-align uses to segment audio);
- **events**: `gap` (a jump in the RF offset), `sync_loss` (a run of low
  `syncConf`), `parity_break` (a flipped or repeated field parity),
  `skipped_field` (the decoder duplicated or dropped a field), `dropout_storm`
  (a run of fields whose active area is mostly dropouts), plus whatever the
  decoder itself recorded at a seam (`no_sync_pulses`, `no_field_start`,
  `duplicate_field`, `dropped_field`, `resume_seam`, `redo`) when the decode
  stored decoder events;
- **segments**: the recording segments derived from the sections and events
  (or the segments stored in the metadata, when there are any), each with the
  `--start/--length` frames an export of it covers.

Per-field picture metrics (active luma mean, same-parity field difference n vs
n−2, back-porch noise, blanking and sync-tip level errors, colour-burst
amplitude) add `scene_change`, `noise`, `blank_video` and `no_burst` events.
The decoder stores them with the metadata (vhs-decode schema 2 and later); for
a decode that has none, `--tbc <luma.tbc>` walks the raw fields to measure
them (burst from the chroma TBC beside the luma one when there is one,
else from the luma TBC itself).
`--per-field` dumps the arrays for downstream classifiers.

The output is JSON (schema 1). Fields are 0-based and half-open, seconds count
from field 0 of the file (`range.startField` lets a consumer rebase a
`--start/--length` window). It needs no GPU and no ffmpeg; the field walk is
I/O-bound (about 478 KB per NTSC field), which is why the results are stored.

The analysis itself lives in the tbc library (`src/library/tbc/segments.h`);
this command is a thin front end so ld-analyse and tbc-export-metadata derive
exactly the same segments.

## Usage

```
tbc-segments <input.tbc.db|input.tbc.json> [options]

  --json <file|->             report destination (default: stdout)
  --start <frame>             first frame, 1-based (as tbc-export-metadata)
  --length <frames>           number of frames
  --rf-sample-rate-hz <Hz>    RF capture rate for the nominal field length;
                              overrides the rate stored in the metadata (with
                              neither: self-calibrated from the median field delta)
  --sensitivity <preset>      low | normal | high threshold preset (normal);
                              the options below override it individually
  --gap-tolerance <f>         deviation from nominal that starts a section (1/3)
  --sync-conf-threshold <n>   sync loss below this percentage of the file's
                              median syncConf (50): vhs-decode writes 45 for a
                              healthy VHS field, ld-decode 100
  --min-run-fields <n>        minimum run for sync_loss / dropout_storm /
                              field-data runs (2)
  --dropout-storm-threshold <f>  active-area dropout coverage (0.25)
  --tbc <luma.tbc>            walk the raw fields when the metadata holds no metrics
  --chroma-tbc <file>         burst source (default: the chroma sibling, see below)
  --no-burst                  do not measure burst amplitude, and do not read
                              the chroma TBC for it (halves the walk's I/O; a
                              stored burst amplitude is kept, not cleared)
  -t, --threads <n>           worker threads for the walk (logical CPUs)
  --scene-threshold-ire <x>   same-parity difference marking a scene change (12)
  --noise-threshold-ire <x>   back-porch noise marking snow (6)
  --blank-luma-ire <x>        active luma at or below which a quiet field is blank (5)
  --min-clip-fields <n>       shorter sections are 'unknown', never a clip (10)
  --min-non-clip-run-fields <n>  a noise/blank run this long splits a section (50)
  --force-walk                walk even when metrics are stored
  --verify-stored             walk and compare with the stored metrics; exit 1
                              above 0.05 IRE of disagreement
  --write                     store walked metrics, the given RF rate (when none
                              is stored) and reconstructed decoder events in the
                              metadata, SQLite first
  --write-segments            with --write: store the derived segments when none
                              are stored
  --force                     with --write-segments: replace stored derived
                              segments (user segments are always kept)
  --per-field                 include per-field arrays
  --summary                   human summary on stderr
  -d, --debug / -q, --quiet   standard logging options
```

Exit status is 0 on success and 1 on any failure (unreadable metadata, no
fields, an invalid `--start/--length`, a TBC holding fewer fields than the
metadata describes, stored metrics failing `--verify-stored`, an unwritable
output). A TBC holding *more* fields than the metadata is walked up to the
metadata's count: the decoders flush metadata in batches, so a decode that was
stopped early leaves a described prefix and an undescribed tail.

### Finding the chroma TBC

`--chroma-tbc` is normally unnecessary: the chroma beside the luma is found the
same way ld-analyse finds it. A `.tbcy` luma pairs with `.tbcc` and a `.ytbc`
with `.ctbc`; a plain `.tbc` luma tries `<stem>_chroma.tbc`, then
`chroma_<stem>.tbc`, then `<stem>.ctbc`, then `<stem>.tbcc`.

Name it explicitly only when the pair does not follow one of those. If no
chroma is found, burst is measured from the luma TBC — which is right for a
composite decode (CVBS, LaserDisc) and wrong for a colour-under one (VHS,
Video8, S-Video), where the luma carries no burst and the resulting numbers
mean nothing. `--no-burst` is the way to skip the measurement deliberately.

### Skipping the burst (`--no-burst`)

Burst amplitude is the only thing the chroma TBC is read for, and it is
measured over the few samples of each line the colour burst occupies — about
4% of a line. The walk still reads the whole file, so on a decode with a
chroma sibling the burst costs half of all the I/O the walk does. On a
475,000-field Video8 capture over a 1 GbE NFS mount that is 227.6 GB of the
458.3 GB read, and about 34 minutes of the 68.

`--no-burst` skips the measurement and never opens the chroma TBC. The other
metrics are unaffected. What is lost is the `no_burst` event, which flags
fields whose burst has collapsed.

A burst amplitude already in the metadata is **kept**, not cleared: not
measuring a metric is not the same as erasing it, and a `--write` that dropped
the column would cost a full chroma walk to recover. The run says so once, on
stderr, naming the number of fields it kept. The consequence is that after such
a run the burst column may be older than the other five metrics — if that
matters, re-walk without `--no-burst`.

Note that a decode backfilled this way counts as complete: a field is
considered measured if *any* of its metrics is finite, so a later run will not
walk it again to fill the burst in. Use `--force-walk` (without `--no-burst`)
if you want it after all.

### Stored metrics, events and segments

Since vhs-decode's metadata schema 2 the decoder stores the picture metrics of
every field it writes, the events it saw at a seam, and the RF sample rate its
`fileLoc` values count in. tbc-segments uses all three when present: no walk,
no guessed rate, and the decoder's seam events confirm the gaps derived from
`fileLoc` (`detail.decoderConfirmed`). A decode from before that gets the same
treatment once, with `--tbc … --write`: the walked metrics are stored (a
`.tbc.db` is created for a JSON-only decode and the JSON rewritten as a
projection), reconstructed `gap` / `skipped_field` / `duplicate_field` events
are stored with `source: "tbc-segments"` (never when the decoder wrote its
own), and `--write-segments` stores the derived segments. Segments stored in
the metadata, by this tool or by ld-analyse's Segments viewer, are what the
report shows; the derivation runs only when none are stored.

### Why the RF rate matters

`fileLoc` is an offset into the **RF capture**, but `videoParameters.sampleRate`
is the **TBC output** rate (4×fsc), so the nominal samples-per-field cannot be
derived from the metadata alone. Decoders that store
`videoParameters.rfSourceSampleRateHz` settle it; otherwise pass the capture
rate (`--rf-sample-rate-hz 40000000` for a 40 MSps CXADC capture) or the tool
takes the median of the positive field-to-field deltas and reports
`timing.nominalSource: "median-delta"` and the implied RF rate.

Legacy JSON written by a 32-bit `fileLoc` writer wraps negative on long
captures; the tool undoes that exactly as tbc-audio-align does
(`timing.fileLocRolloverFixups`).

## Output

```json
{"schemaVersion": 1,
 "tool": {"name": "tbc-segments", "branch": "...", "commit": "..."},
 "input": {"path": "...", "kind": "sqlite|json"},
 "video": {"system": "NTSC", "tapeFormat": "VHS", "fieldRate": 59.94, "secondsPerField": 0.01668,
           "fieldWidth": 910, "fieldHeight": 263, "numberOfFields": 123456, "numberOfFrames": 61728,
           "frameOffset": 0, "isFirstFieldFirst": true},
 "timing": {"nominalSamplesPerField": 667333.3,
            "nominalSource": "rf-sample-rate-hz|metadata-rf-sample-rate-hz|median-delta|diskLoc|none",
            "impliedRfSampleRateHz": 4.0e7, "gapDetection": "fileLoc|diskLoc|unavailable", "fileLocRolloverFixups": 0},
 "range": {"startFrame": 1, "lengthFrames": 61728, "startField": 0, "endFieldExclusive": 123456, "secondsOrigin": "field0"},
 "thresholds": {...},
 "fieldData": {"enabled": true, "source": "stored|walked|none", ...},
 "sections": [{"startField": 0, "endFieldExclusive": 5000, "startSeconds": 0.0, "endSeconds": 83.4}],
 "events": [{"kind": "gap", "startField": 5000, "endFieldExclusive": 5001, "startSeconds": 83.4, "endSeconds": 83.42,
             "severity": 0.9, "detail": {"seamAfterField": 4999, "deltaSamples": 3336667, "deltaFields": 5.0,
                                        "missingFields": 4, "decoderConfirmed": true}}],
 "counts": {"gap": 1, "sync_loss": 0, "parity_break": 0, "skipped_field": 0, "dropout_storm": 0,
            "scene_change": 0, "noise": 0, "blank_video": 0, "no_burst": 0, "decoderEvents": 1},
 "segments": [{"id": 1, "startField": 0, "endFieldExclusive": 5000, "startSeconds": 0.0, "endSeconds": 83.4,
               "kind": "clip", "source": "derived", "enabled": true, "title": "", "comment": "",
               "createdBy": "tbc-segments", "updatedAt": "2026-09-06T12:00:00Z",
               "startFrame": 1, "lengthFrames": 2500}],
 "segmentsSource": "stored|derived|none",
 "perField": {...},        // with --per-field
 "fieldMetrics": {...}}    // with --per-field, when metrics are stored or walked
```

`--sync-conf-threshold` is a percentage of the file's median `syncConf`, because the
decoders disagree on what a healthy field scores (vhs-decode 45, ld-decode 100)
while both force 10 or 0 on a fault; the applied value is reported as
`thresholds.syncConfThresholdEffective`.

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
event's severity (`detail.fieldDataCoincident`); a seam-class decoder event
within a field of a `gap` marks it `decoderConfirmed`.

### Segments and frames

Every gapless section becomes one segment, split further at noise or blank
runs of at least `--min-non-clip-run-fields`. Each piece is classified `noise`
(mostly noise or no burst), `blank` (mostly black), `unknown` (shorter than
`--min-clip-fields`, or mostly sync loss) or `clip`; only clips are enabled.
`startFrame` / `lengthFrames` are the 1-based frame range an export of the
segment covers (`tbc-video-export --start/--length`), under the mixed-frame
rule: a frame whose second field starts a segment belongs to the segment that
owns its first field, so contiguous segments tile the frame range exactly once
and a segment holding only a second field (no frame of its own) reports 0 / 0.

## Tests

`testsegments` under `src/library/tbc/testsegments` (ctest) synthesises
metadata in memory: gaps, parity and skipped fields, sync-loss runs, dropout
storms, a 32-bit rollover round-tripped through JSON, `--start/--length`
clipping, the RF rate taken from the metadata, decoder-event merging, stored
picture metrics driving the field-data events, segment derivation and
classification, the presets, frame containment and segment tiling under the
mixed-frame rule, and an exact equivalence of the sections with
tbc-audio-align's `ExtractGapLessSection` + `TbcJsonFixup` over jittered,
gapped, backward-stepping and 32-bit-wrapped `fileLoc` series in PAL and NTSC.
`testsegmentswalk` here covers the field walk over a small synthetic luma +
chroma TBC (scene change, snow, blank picture, missing burst, single- vs
multi-threaded equality, a short TBC refused, a TBC with a trailing surplus
walked to the metadata's count). `tbc-segments-smoke` runs the shipped
`test-data/ntsc/ve-snw-cut` fixture through both tiers.

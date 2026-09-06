# tbc-segments

tbc-segments finds recording boundaries: where a recording was started,
stopped, or interrupted on a captured tape or disc. It reads the decode metadata
(`.tbc.db`, or the legacy `.tbc.json`; a `.tbc.json` with a `.tbc.db` sibling
opens the database) and emits a JSON report of gapless sections, boundary
events and the recording segments derived from them. The rules live in the TBC
library (`src/library/tbc/segments.h`), so ld-analyse and tbc-export-metadata
derive exactly the same segments.

## What it detects

From the metadata alone:

| Event | Source |
| --- | --- |
| `gap` | The RF file offset between two fields deviates from the nominal field length by more than a third (tbc-audio-align's gapless-section rule). |
| `sync_loss` | A run of fields whose `syncConf` is below `--sync-conf-threshold`, a percentage of the file's median `syncConf` (vhs-decode writes 45 for every healthy field, ld-decode 100). |
| `parity_break` | `decodeFaults` bit 1, or two consecutive fields of the same parity. |
| `skipped_field` | `decodeFaults` bit 4 (the decoder duplicated or dropped a field to keep cadence). |
| `dropout_storm` | A run of fields whose active area is mostly dropouts. |
| decoder events | What the decoder itself recorded at a seam (`no_sync_pulses`, `no_field_start`, `duplicate_field`, `dropped_field`, `resume_seam`, `redo`), when the decode stored them. A `gap` the decoder also saw is marked `decoderConfirmed`. |

From per-field picture metrics (stored with the metadata by the decoder or a
backfill, else measured by walking `--tbc <luma.tbc>`):

| Event | Source |
| --- | --- |
| `scene_change` | Same-parity field difference (field n against n−2) above the threshold. |
| `noise` | Back-porch noise above the threshold: snow, unrecorded tape. |
| `blank_video` | Quiet fields whose active luma sits at black. |
| `no_burst` | Colour-burst amplitude far below the file's median (from `<luma>_chroma.tbc` when present). |

## Segments

Every gapless section becomes one segment, split further at noise or blank
runs of at least `--min-non-clip-run-fields`; each piece is classified `clip`,
`blank`, `noise` or `unknown` (shorter than `--min-clip-fields`, or mostly sync
loss) and only clips are enabled for export. The report gives each segment its
0-based field range, its seconds, and the 1-based `startFrame` /
`lengthFrames` an export of it covers (`tbc-video-export --start/--length`),
under the mixed-frame rule: a frame whose second field starts a segment
belongs to the segment that owns its first field, so contiguous segments tile
the frame range exactly once.

Segments stored in the metadata (by ld-analyse's Segments viewer or a
previous `--write-segments`) are reported as they are; the derivation runs only
when none are stored.

## Storing results

`--write` stores walked picture metrics, the RF rate given with
`--rf-sample-rate-hz` (when the decoder stored none) and reconstructed decoder
events (`gap`, `skipped_field`, `duplicate_field`, `source: tbc-segments`,
only when the decoder stored no events of its own) back into the metadata,
SQLite first: a JSON-only decode gets a `.tbc.db` and the JSON is rewritten as
a projection. `--write-segments` adds the derived segments when none are
stored (`--force` replaces derived ones; user segments are always kept). With
metrics stored, later runs never touch the TBC again; `--verify-stored` walks
it once more and fails above 0.05 IRE of disagreement.

## Usage

```
tbc-segments capture.tbc.db --json capture.segments.json
tbc-segments capture.tbc.db --tbc capture.tbc --write --write-segments --summary --json -
tbc-segments capture.tbc.db --sensitivity high --start 1201 --length 600 --json - | jq .segments
tbc-segments capture.tbc.db --tbc capture.tbc --verify-stored --json /dev/null
```

`--sensitivity low|normal|high` picks a threshold preset; any individual
option overrides it. See the tool's README for every option and the JSON
schema.

Field numbers in the report are 0-based and half-open; seconds count from field
0 of the file. Exit status is 0 on success, 1 on any failure.

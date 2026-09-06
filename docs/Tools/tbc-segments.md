# tbc-segments

tbc-segments finds recording boundaries: where a recording was started,
stopped, or interrupted on a captured tape or disc. It reads the decode metadata
(`.tbc.db`, or the legacy `.tbc.json`) and optionally walks the raw TBC fields,
and emits a JSON report of gapless sections and boundary events for editors and
automated segmentation.

## What it detects

From the metadata alone:

| Event | Source |
| --- | --- |
| `gap` | The RF file offset between two fields deviates from the nominal field length (tbc-audio-align's gapless-section rule). |
| `sync_loss` | A run of fields whose `syncConf` is below the threshold. |
| `parity_break` | `decodeFaults` bit 1, or two consecutive fields of the same parity. |
| `skipped_field` | `decodeFaults` bit 4 (the decoder duplicated or dropped a field to keep cadence). |
| `dropout_storm` | A run of fields whose active area is mostly dropouts. |

With `--tbc <luma.tbc>`, per-field measurements of the raw samples add:

| Event | Source |
| --- | --- |
| `scene_change` | Same-parity field difference (field n against n−2) above the threshold. |
| `noise` | Back-porch noise above the threshold: snow, unrecorded tape. |
| `blank_video` | Quiet fields whose active luma sits at black. |
| `no_burst` | Colour-burst amplitude far below the file's median (from `<luma>_chroma.tbc` when present). |

`--per-field` includes the per-field arrays (`syncConf`, `decodeFaults`,
dropout coverage, and the field metrics) so a downstream classifier can fuse
them with other evidence.

## Usage

```
tbc-segments capture.tbc.db --json capture.segments.json --rf-sample-rate-hz 40000000
tbc-segments capture.tbc.db --tbc capture.tbc --per-field --json - | jq .counts
tbc-segments capture.tbc.json --start 1201 --length 600 --summary --json -
```

See the tool's README for every option, the JSON schema, and why the RF sample
rate matters (the metadata's `sampleRate` is the TBC output rate, not the
capture rate, so the tool self-calibrates when the rate is not given).

Field numbers in the report are 0-based and half-open; seconds count from field
0 of the file. Exit status is 0 on success, 1 on any failure.

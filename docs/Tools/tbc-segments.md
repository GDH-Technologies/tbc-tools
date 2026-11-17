# tbc-segments

tbc-segments finds where recorded picture starts and ends on a captured tape
or disc, and where one camera run gives way to the next. It reads the decode
metadata (`.tbc.db`, or the legacy `.tbc.json`; a `.tbc.json` with a `.tbc.db`
sibling opens the database) and emits a JSON report of gapless sections,
boundary events, the [segments](tbc-segments-glossary.md#segment) derived from them, and the
[shot boundaries](tbc-segments-glossary.md#shot-boundary), [scenes](tbc-segments-glossary.md#scene) and
[shots](tbc-segments-glossary.md#shot) inside the content spans. Every term is defined once, in the
[segment glossary](tbc-segments-glossary.md). The rules live in the
tbc-segments module (`src/tbc-segments/core`), which tbc-analyse and
tbc-export-metadata link, so every tool derives exactly the same segments.

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
| [`field_diff_spike`](tbc-segments-glossary.md#field-diff-spike) | Same-parity field difference (field n against n−2) above `--field-diff-spike-ire`: raw evidence (shot boundaries, flashes, fast motion), not a decision. |
| `noise` | Back-porch noise above the threshold: snow, unrecorded tape. |
| `blank_video` | Quiet fields with no picture: active luma at black and the levels collapsed (blanking well below, or the sync tip well above, nominal), or luma below black. A dark recorded scene keeps its levels and is not blank. |
| `no_burst` | Colour-burst amplitude far below the file's median (from `<luma>_chroma.tbc` when present). |

## Segments

Every gapless section becomes one segment, ended early at a noise or blank run
of at least `--min-non-content-run-fields` (a gap shorter than
`--min-content-span-fields` inside a run is part of it: snow flickers). Each
piece is classified [`content_span`](tbc-segments-glossary.md#content-span), [`blank`](tbc-segments-glossary.md#blank),
[`noise`](tbc-segments-glossary.md#noise) or [`unknown`](tbc-segments-glossary.md#unknown) (shorter than
`--min-content-span-fields`, or mostly sync loss), and only content spans are
[enabled](tbc-segments-glossary.md#enabled) for export. Adjacent non-content pieces merge into one,
across gaps too, and a sliver between two content spans joins the following
one, so an unrecorded stretch is one segment however often its snow flickers
or the decoder resyncs. Two content pieces that meet across an
[RF gap](tbc-segments-glossary.md#rf-gap) with nothing between them are one content span, and the
gap is a shot boundary. The report gives each segment its 0-based field range,
its seconds, and the 1-based `startFrame` / `lengthFrames` an export of it
covers (`tbc-video-export --start/--length`), under the mixed-frame rule: a
frame whose second field starts a segment belongs to the segment that owns its
first field, so contiguous segments tile the frame range exactly once.

Segments stored in the metadata (by tbc-analyse's Segments viewer or a
previous `--write-segments`) are reported as they are; the derivation runs only
when none are stored.

## Shot boundaries, scenes and shots

A camcorder that stops and starts recording leaves no gap or snow on the tape,
so the rules above cannot end a content span there: it is a
[shot boundary](tbc-segments-glossary.md#shot-boundary), where one [shot](tbc-segments-glossary.md#shot) ends and the
next begins. The picture still changes between two fields: the same-parity
field difference spikes on two adjacent fields, matched, with quiet fields
either side. tbc-segments detects each such shot boundary inside the content
spans (evidence `picture`), and every RF gap inside one (evidence `rf_gap`). A
photo flash (one bright field, spikes two fields apart) and the exposure
settling after it are ruled out, and so is a cut on a screen in shot (a ramp
over two or three fields). Each carries a [confidence](tbc-segments-glossary.md#confidence) and a
reason.

A shot boundary never ends a segment. A person gives it a
[verdict](tbc-segments-glossary.md#verdict) (confirmed or rejected) and can mark it as a
[scene change](tbc-segments-glossary.md#scene-change), in tbc-analyse's timeline menu or the Shot
Boundaries tab of its Segments viewer. [Scenes](tbc-segments-glossary.md#scene) and shots are
worked out from them, never stored, each with its own title and
[enabled](tbc-segments-glossary.md#enabled) flag, and appear in the report as `scenes` and `shots`.
To end a segment at a frame by hand, [cut](tbc-segments-glossary.md#cut) it in the Segments viewer.

Exports choose a granularity: tbc-analyse's Export tab
[exports by](tbc-segments-glossary.md#export-by) content span, scene or shot, with
[chapters by](tbc-segments-glossary.md#chapters-by) none, scene or shot inside each file.
`tbc-export-metadata --ffmetadata-chapters-by segment|scene|shot` (and
tbc-video-export `--export-metadata-chapters-by`) pick the chapters of one
export. Shot boundaries live in the `.tbc.db`, in this module's own
`shot_boundary` table, and appear in the report as `shotBoundaries`,
`shot_boundary` events and `counts.shot_boundary`. See the
[module README](../../src/tbc-segments/README.md) for the rules, their options
and the temporary aliases kept for older callers.

## Storing results

`--write` stores walked picture metrics, the RF rate given with
`--rf-sample-rate-hz` (when the decoder stored none) and reconstructed decoder
events (`gap`, `skipped_field`, `duplicate_field`, `source: tbc-segments`,
only when the decoder stored no events of its own) back into the metadata,
SQLite first: a JSON-only decode gets a `.tbc.db` and the JSON is rewritten as
a projection. `--write-segments` adds the derived segments and shot boundaries
when none are stored (`--force` replaces derived segments and detected shot
boundaries; user segments, and user or rejected shot boundaries, are always
kept). With metrics stored, later runs never touch the TBC again;
`--verify-stored` walks it once more and fails above 0.05 IRE of disagreement.

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

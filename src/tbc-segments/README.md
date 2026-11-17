# tbc-segments

**Segment analysis of TBC metadata and fields**

## Overview

tbc-segments finds where recorded picture starts and ends on a tape or disc
capture, and where one camera run gives way to the next. Every term below is
defined once, in the [segment glossary](../../docs/Tools/tbc-segments-glossary.md).
It reads the decode metadata (`.tbc.db`, or the legacy
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
- **[segments](../../docs/Tools/tbc-segments-glossary.md#segment)**: the
  content spans, noise, blank and unknown stretches derived from the sections
  and events (or the segments stored in the metadata, when there are any), each
  with the `--start/--length` frames an export of it covers;
- **[shot boundaries](../../docs/Tools/tbc-segments-glossary.md#shot-boundary)**
  inside the content spans, and the **scenes** and **shots** worked out from
  them.

Per-field picture metrics (active luma mean, same-parity field difference n vs
n−2, back-porch noise, blanking and sync-tip level errors, colour-burst
amplitude) add
[`field_diff_spike`](../../docs/Tools/tbc-segments-glossary.md#field-diff-spike),
`noise`, `blank_video` and `no_burst` events.
The decoder stores them with the metadata (vhs-decode schema 2 and later); for
a decode that has none, `--tbc <luma.tbc>` walks the raw fields to measure
them (burst from the chroma TBC beside the luma one when there is one,
else from the luma TBC itself).
`--per-field` dumps the arrays for downstream classifiers.

The output is JSON (schema 1). Fields are 0-based and half-open, seconds count
from field 0 of the file (`range.startField` lets a consumer rebase a
`--start/--length` window). It needs no GPU and no ffmpeg; the field walk is
I/O-bound (about 478 KB per NTSC field), which is why the results are stored.

The analysis itself lives in this module's `tbc-segments-core` library
(`core/segments.h`); this command (`cli/`) is a thin front end over it, so
tbc-analyse and tbc-export-metadata derive exactly the same segments.

### Module layout

Every piece of tbc-tools' segment handling lives under `src/tbc-segments`, so
the feature stays apart from the shared tool sources and can move as one:

| Directory | Target | What |
| --- | --- | --- |
| `core/` | `tbc-segments-core` (static, Qt Core) | the rules (`segments.h`), shot boundaries (`shotboundaries.h`) and their store in the `.tbc.db` (`shotboundarystore.h`), scenes and shots worked out from them (`sceneshots.h`), the glossary's terms and tooltips (`glossary_generated.h`), the report (`segmentsreport.h`, used by `tbc-export-metadata --segments-json`), whole-file derivation for a loaded file (`segmentsderive.h`), segments as export chapters (`segmentchapters.h`, used by `--ffmetadata`), the per-segment export plan (`segmentexportplan.h`, used by tbc-analyse's Export tab), the field walk (`fieldmetrics.h`, `fieldwalkpool.h`) |
| `gui/` | `tbc-segments-gui` (static, Qt Widgets) | the Segments viewer with its Shot Boundaries tab (`segmentsviewerdialog.h`), the module's preferences (`segmentpreferences.h`) and `SegmentsController`, which carries tbc-analyse's segment behaviour (viewer, edits, re-derive, set In/Out, timeline ticks and tints, chapter skip, readout, timeline menu); the main window reaches it through `SegmentsController::Hooks` |
| `cli/` | `tbc-segments` | this command and its script tests |
| `glossary/` | ctest `tbc-segments-glossary` | `glossary.json`, the one source of the segment vocabulary, and `generate_glossary.py`, which writes `core/glossary_generated.h` and the [glossary page](../../docs/Tools/tbc-segments-glossary.md) (both committed, so a build needs no Python) and lints the module for retired words |
| `tools/` | — | `extract-shot-boundary-fixture.py`, which builds a labelled fixture from a `.tbc.db` |

The data model (`TbcMetaData::Segment`, `DecoderEvent`, `PictureMetrics` and
their tables) stays in the tbc library with the rest of the metadata.

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
  --field-diff-spike-ire <x>  same-parity difference that is a field diff spike (12)
  --noise-threshold-ire <x>   back-porch noise marking snow (6)
  --blank-luma-ire <x>        active luma at or below which a quiet field is blank (5),
                              when its levels have collapsed too (below):
  --blank-level-dev-ire <x>   blanking this far below, or sync tip this far
                              above, nominal (5); a dark recorded scene keeps
                              its levels and stays picture
  --blank-below-black-ire <x> or active luma below this (-5): no picture at all
  --no-shot-boundaries        do not detect shot boundaries
  --shot-boundary-pair-match <f>    the weaker spike of the pair is at least
                              this fraction of the stronger (0.9)
  --shot-boundary-min-peak-ire <x>  the pair's peak field difference (5)
  --shot-boundary-min-contrast <x>  peak over the local background (1.5)
  --shot-boundary-max-neighbour-ratio <f>  the fields either side of the
                              pair at most this fraction of its peak (0.6)
  --min-content-span-fields <n>     a shorter piece is 'unknown', never a
                              content span (10)
  --min-non-content-run-fields <n>  a noise/blank run at least this long ends
                              a content span (50)
  --force-walk                walk even when metrics are stored
  --verify-stored             walk and compare with the stored metrics; exit 1
                              above 0.05 IRE of disagreement
  --write                     store walked metrics, the given RF rate (when none
                              is stored) and reconstructed decoder events in the
                              metadata, SQLite first
  --write-segments            with --write: store the derived segments when none
                              are stored, and the shot boundaries
  --force                     with --write-segments: replace stored derived
                              segments and detected shot boundaries (user
                              segments, user and rejected boundaries are kept)
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
same way tbc-analyse finds it. A `.tbcy` luma pairs with `.tbcc` and a `.ytbc`
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
the metadata, by this tool or by tbc-analyse's Segments viewer, are what the
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
 "tool": {"name": "tbc-segments", "build": "nix|git|unknown", "source": "src-…|tree-…|unknown"},
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
            "field_diff_spike": 0, "noise": 0, "blank_video": 0, "no_burst": 0, "shot_boundary": 0,
            "decoderEvents": 1},
 "segments": [{"id": 1, "startField": 0, "endFieldExclusive": 5000, "startSeconds": 0.0, "endSeconds": 83.4,
               "kind": "content_span", "source": "derived", "enabled": true, "title": "", "comment": "",
               "createdBy": "tbc-segments", "updatedAt": "2026-09-06T12:00:00Z",
               "startFrame": 1, "lengthFrames": 2500}],
 "segmentsSource": "stored|derived|none",
 "shotBoundaries": [{"id": 1, "field": 2400, "seconds": 40.04, "frame": 1201, "evidence": "picture|rf_gap",
                     "source": "derived|user", "verdict": "detected|confirmed|rejected", "sceneChange": false,
                     "sceneTitle": "", "sceneEnabled": true, "shotEnabled": true, "confidence": 0.92,
                     "reason": "...", "comment": "", "createdBy": "tbc-segments", "updatedAt": "...", "detail": {...}}],
 "shotBoundariesSource": "stored|derived|none",
 "scenes": [{"id": 1, "spanId": 1, "number": 1, "startField": 0, "endFieldExclusive": 5000, "startSeconds": 0.0,
             "endSeconds": 83.4, "shotBoundaryId": -1, "title": "Segment 1", "enabled": true, "startFrame": 1, "lengthFrames": 2500}],
 "shots": [{"id": 1, "spanId": 1, "sceneId": 1, "number": 1, ...}],
 "perField": {...},        // with --per-field
 "fieldMetrics": {...}}    // with --per-field, when metrics are stored or walked
```

`--sync-conf-threshold` is a percentage of the file's median `syncConf`, because the
decoders disagree on what a healthy field scores (vhs-decode 45, ld-decode 100)
while both force 10 or 0 on a fault; the applied value is reported as
`thresholds.syncConfThresholdEffective`.

`--field-diff-spike-ire` and `--noise-threshold-ire` are floors: the value applied
is the larger of the option and 2.5× the file's own median field difference /
back-porch noise, because real tape (an EP VHS decode especially) raises both
baselines. The applied values are reported under `fieldData.thresholds` as
`fieldDiffSpikeIreEffective` and `noiseThresholdIreEffective`. A field diff spike
candidate whose field, or whose n−2 partner, is inside a noise run is dropped:
snow makes every field difference enormous and the `noise` run already says so.

`gap` events place `startField` on the first field **after** the seam, so a
downstream boundary belongs between `seamAfterField` and `startField`. A
`noise` or `no_burst` run that coincides with a `gap` or `sync_loss` raises that
event's severity (`detail.fieldDataCoincident`); a seam-class decoder event
within a field of a `gap` marks it `decoderConfirmed`.

### Segments and frames

Every gapless section becomes one segment, ended early at a noise or blank
run of at least `--min-non-content-run-fields`. Snow dips under the noise
threshold on a field or two, so inside a run a gap shorter than
`--min-content-span-fields` counts as part of it. Each piece is classified
[`noise`](../../docs/Tools/tbc-segments-glossary.md#noise) (mostly noise or no burst),
[`blank`](../../docs/Tools/tbc-segments-glossary.md#blank) (mostly blank: dark, quiet and with the levels
collapsed, see `--blank-level-dev-ire`), [`unknown`](../../docs/Tools/tbc-segments-glossary.md#unknown) (shorter
than `--min-content-span-fields`, or mostly sync loss) or
[`content_span`](../../docs/Tools/tbc-segments-glossary.md#content-span); only content spans are
[enabled](../../docs/Tools/tbc-segments-glossary.md#enabled). Adjacent non-content pieces then merge into one,
across gaps too (the decoder resyncs inside snow), as noise or blank by
whichever covers more of it; a piece shorter than `--min-content-span-fields`
between two content spans (a one-field section left at a resync) joins the
following one; and two content pieces that meet across an RF gap, with no
noise or blank between them, merge into one content span, the gap becoming a
shot boundary (below). One unrecorded stretch is therefore one segment, however
often the snow flickers, and one stretch of recorded picture is one content
span, however often the decoder lost sync inside it. A content span runs from
its [span start](../../docs/Tools/tbc-segments-glossary.md#span-start) to its [span end](../../docs/Tools/tbc-segments-glossary.md#span-end).
A stored segment of the legacy kind `clip` reads as a content span. <!-- glossary-legacy -->

`startFrame` / `lengthFrames` are the 1-based frame range an export of the
segment covers (`tbc-video-export --start/--length`), under the mixed-frame
rule: a frame whose second field starts a segment belongs to the segment that
owns its first field, so contiguous segments tile the frame range exactly once
and a segment holding only a second field (no frame of its own) reports 0 / 0.
Scenes and shots follow the same rule.

### Shot boundaries

A camcorder that stops and starts recording leaves a clean edit: no RF gap,
no snow, no sync loss, so nothing above ends a content span there. It is a
[shot boundary](../../docs/Tools/tbc-segments-glossary.md#shot-boundary): one [shot](../../docs/Tools/tbc-segments-glossary.md#shot) ends and the next
begins with nothing between them. What it leaves is a picture that changes
between two fields. The same-parity field difference (field n against n-2)
then spikes on two adjacent fields, n and n+1, each comparing a field of the
new shot with one of the old, and drops back at n+2. That is a shot boundary
with [evidence](../../docs/Tools/tbc-segments-glossary.md#evidence) `picture` when:

- the pair matches: the weaker spike is at least `--shot-boundary-pair-match`
  (0.9) of the stronger, and the peak is at least
  `--shot-boundary-min-peak-ire` (5);
- it stands out: the peak is at least `--shot-boundary-min-contrast` (1.5)
  times the local background (the larger median of fields n-12..n-2 and
  n+3..n+14);
- its neighbours are quiet: fields n-1 and n+2 sit no higher than halfway from
  the background to the peak, and no higher than
  `--shot-boundary-max-neighbour-ratio` (0.6) of it (motion keeps them high);
- neither field is snow, and no flash falls in the 12 fields before it.

A photo flash lights a single field, so its spikes land on n and n+2 with n+1
quiet, and the pair test rejects it. The camera's exposure can then settle
over the next few fields into a matched pair, which is why a flash
(one field more than 10 IRE above its neighbours) in the window before rules
a pair out. A cut inside a video playing on a screen in shot ramps over
2-3 fields, so its spikes don't match. A
[field diff spike](../../docs/Tools/tbc-segments-glossary.md#field-diff-spike) on its own is raw evidence, not a
shot boundary: flashes and fast motion make them too.

An [RF gap](../../docs/Tools/tbc-segments-glossary.md#rf-gap) strictly inside a content span is a shot boundary too,
with evidence `rf_gap` and a confidence of 0.9 when the decoder recorded the
seam (0.75 when it did not). Boundaries closer than four fields are one: the
stronger stands, and picture evidence wins over an RF gap at the same place.

Shot boundaries are found only inside content spans, away from their edges.
Each carries a [confidence](../../docs/Tools/tbc-segments-glossary.md#confidence) (50% for passing, the rest from
how cleanly it passes) and a reason in words. The picture rule was tuned on a
hand-labelled Video8 tape: `test-data/segments/video8-shot-boundary-labels.json`
holds the field metrics around 30 shot boundaries and 10 look-alikes (flashes
and their aftermath, cuts on a screen), and `testsegments` checks that every
shot boundary is found and none of the rest is.

A shot boundary never ends a segment. Each has a
[verdict](../../docs/Tools/tbc-segments-glossary.md#verdict) (`detected`, then `confirmed` or `rejected` by a
person), a [source](../../docs/Tools/tbc-segments-glossary.md#source) (`derived` or `user`), and per-level flags:
`sceneChange` with `sceneTitle` and `sceneEnabled`, and `shotEnabled`. A
rejected one counts for nothing below. To end a segment at a frame by hand,
[cut](../../docs/Tools/tbc-segments-glossary.md#cut) it in tbc-analyse's Segments viewer.

### Scenes and shots

Scenes and shots are worked out from the segments and shot boundaries, never
stored, and appear in the report as `scenes` and `shots`:

| | Starts at | Enabled | Title when none is given |
| --- | --- | --- | --- |
| [Scene](../../docs/Tools/tbc-segments-glossary.md#scene) | the span start, and every shot boundary marked as a [scene change](../../docs/Tools/tbc-segments-glossary.md#scene-change) | the boundary's `sceneEnabled`; the first scene takes the span's flag | the boundary's `sceneTitle`, else `<span title> · scene n`; the first scene takes the span's title |
| [Shot](../../docs/Tools/tbc-segments-glossary.md#shot) | the scene start, and every other shot boundary | the boundary's `shotEnabled`; the first shot of a span takes the span's flag | `<scene title> · n` |

An untitled span is `Segment <id>`. Each level's [enabled](../../docs/Tools/tbc-segments-glossary.md#enabled) flag
stands on its own: disabling a scene does not disable its shots. A person
marks a scene change in tbc-analyse (the timeline menu, or the Shot Boundaries
tab of the Segments viewer).

Exports choose a granularity. tbc-analyse's Export tab
[exports by](../../docs/Tools/tbc-segments-glossary.md#export-by) content span, scene or shot (one file per enabled
one), with [chapters by](../../docs/Tools/tbc-segments-glossary.md#chapters-by) none, scene or shot inside each file
(only finer than the files). `tbc-export-metadata --ffmetadata-chapters-by
segment|scene|shot` and `tbc-video-export --export-metadata-chapters-by` do the
same for the chapters of one export.

### Where shot boundaries are stored

Shot boundaries live in the `.tbc.db`, in this module's own tables:
`shot_boundary` (field, evidence, source, verdict, scene change, scene title,
scene enabled, shot enabled, confidence, reason, detail, comment, provenance)
and `tbc_segments_meta` (`shot_boundary_schema = 1`). They are not part of the
tbc library's schema or its `user_version`, and the `.tbc.json` never carries
them.
The legacy `segment_mark` table (3.3.1-gdh-1.1) is dropped when shot boundaries are written. <!-- glossary-legacy -->
`--write-segments` stores them with the segments under
the same rule: when none are stored, or with `--force` (detected boundaries
replaced, user and rejected ones kept, ids never reused). A tool that rewrites
the whole database drops them; tbc-analyse's Save does exactly that, and writes
them back straight after. tbc-analyse's export snapshots carry no `.tbc.db`, so
the export writes them beside the snapshot's JSON as `<json>.shot-boundaries.json`.

The report gains `shotBoundaries` (each with field, seconds, frame and the
fields above), `shotBoundariesSource` (`stored`, `derived` or `none`), a
`shot_boundary` event per boundary that is not rejected, `counts.shot_boundary`,
and the `scenes` and `shots` arrays.

**Temporary aliases**, for callers still on 3.3.1-gdh-1.1, to be removed once
they have switched:

- every `field_diff_spike` event is also reported as a `scene_change` event, with `counts.scene_change`; <!-- glossary-legacy -->
- `--scene-threshold-ire` is a hidden alias of `--field-diff-spike-ire`; <!-- glossary-legacy -->
- the export plan accepts the selection `enabled_clips` as `enabled_content_spans`. <!-- glossary-legacy -->

## Tests

`testsegments` (`core/tests`, ctest) synthesises
metadata in memory: gaps, parity and skipped fields, sync-loss runs, dropout
storms, a 32-bit rollover round-tripped through JSON, `--start/--length`
windows, the RF rate taken from the metadata, decoder-event merging, stored
picture metrics driving the field-data events, segment derivation and
classification (a dark scene with normal levels stays picture; snow with
single-field dips and a resync inside it is one noise segment; a one-field
section between two content spans joins the next; an RF gap inside picture is
one content span with an `rf_gap` shot boundary), shot boundaries against the
hand-labelled fixture, shot boundary merging, scenes and shots (ranges, titles,
enabled flags), the export plan by span, scene and shot, the store's round
trip, its sidecar and the legacy table dropped, the report's aliases, the
presets, frame containment and segment tiling under the mixed-frame rule, and
an exact equivalence of the sections with tbc-audio-align's
`ExtractGapLessSection` + `TbcJsonFixup` over jittered, gapped,
backward-stepping and 32-bit-wrapped `fileLoc` series in PAL and NTSC.
`testsegmentswalk` (`core/tests/walk`) covers the field walk over a small
synthetic luma + chroma TBC (a field diff spike, snow, blank picture, missing
burst, single- vs multi-threaded equality, a short TBC refused, a TBC with a
trailing surplus walked to the metadata's count). `tbc-segments-smoke` runs the
shipped `test-data/ntsc/ve-snw-cut` fixture through both tiers, and the scripts
in `cli/tests` check that `--write` is idempotent and that `--no-burst` keeps a
stored burst amplitude; `tbc-segments-legacy-spike-flag` checks the hidden
alias. `testsegmentsviewer` and `testsegmentscontroller` (`gui/tests`,
offscreen) cover the viewer's buttons and the controller's lookups, readout,
chapter-skip targets, timeline ticks and tints, cuts, re-derive (user segments
kept), the timeline menu, and the shot boundaries: detected at load, ticks and
skip stops, a scene change marked from the menu, and the Shot Boundaries tab's
edits. `tbc-segments-glossary` checks that the generated glossary files are
current and that the module uses no retired word.
`tools/extract-shot-boundary-fixture.py` builds a fixture like the shot
boundary one from any labelled `.tbc.db`.

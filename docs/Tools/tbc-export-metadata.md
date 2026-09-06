## tbc-export-metadata
This application reads ld-decode metadata (`.db` or `.json`) and exports information in standard formats that other tools can read. It can also write decode-export JSON (`.export.json`) directly from SQLite metadata. At present, it can export:

- Decode export metadata JSON (`--export-json` / `--output-json`)
- Per-frame signal quality information from the VITS test signals, as CSV
- Per-frame LaserDisc VBI control signals, as CSV
- LaserDisc navigation information, as Audacity labels
- LaserDisc navigation information, as FFMETADATA1 (which FFmpeg can use to add chapter navigation to a video file, including VITC `timecode` when available)
- Recording segments stored in the metadata (tape record start/stop seams found by tbc-segments or edited in ld-analyse), as FFMETADATA1 chapters in place of the LaserDisc navigation chapters, rebased to the `--start`/`--length` window so a per-segment export carries its own chapters
- The recording-segment report (segments, events, per-segment frame ranges) as JSON, from the metadata alone
- FFmpeg readvitc filter style VITC text, as `lavfi.readvitc.*` key lines for every frame
- Closed Captions, as SCC format (which tools like [ttconv](https://github.com/sandflow/ttconv) can read)

Syntax:

tbc-export-metadata \<options> \<input>

```
Options:
  -h, --help                Displays help on commandline options.
  --help-all                Displays help including Qt specific options.
  -v, --version             Displays version information.
  -d, --debug               Show debug
  -q, --quiet               Suppress info and warning messages
  -g, --gui                 Launch metadata export GUI
  --input <file>            Specify input metadata file
  --input-sqlite <file>     Alias for --input
  --export-json             Write decode export metadata JSON (<input>.export.json by default)
  --output-json <file>      Specify decode export metadata JSON output file (implies --export-json)
  --vits-csv <file>         Write VITS information as CSV
  --vbi-csv <file>          Write VBI information as CSV
  --audacity-labels <file>  Write navigation information as Audacity labels
  --ffmetadata <file>       Write navigation information as FFMETADATA1 (includes VITC timecode when available)
  --ffmetadata-no-vitc-timecode  Disable FFmpeg-style VITC timecode output in FFMETADATA
  --start <frame>           FFMETADATA / segments JSON export start frame (1-based)
  --length <frames>         FFMETADATA / segments JSON export frame length
  --ffmetadata-all-segments Write a chapter for every stored recording segment, disabled ones included
                            (default: enabled segments only)
  --ffmetadata-no-segments  Ignore stored recording segments; write LaserDisc navigation chapters only
  --segments-json <file>    Write the recording-segment report (stored or derived segments, events,
                            frame ranges) as JSON; '-' for stdout
  --ffmpeg-vitc <file>      Write FFmpeg readvitc filter style VITC text (lavfi.readvitc.*)
  --closed-captions <file>  Write closed captions as Scenarist SCC V1.0 format

Arguments:
  input                     Specify input metadata file
```

### Recording segments as chapters

When the metadata holds recording segments (`segments` in the JSON, the
`segment` table in the SQLite database; written by `tbc-segments
--write-segments` or ld-analyse's Segments viewer), `--ffmetadata` writes one
`[CHAPTER]` per enabled segment instead of the LaserDisc navigation chapters.
`START`/`END` are the segment's 0-based fields rebased to the export start
field, so the same `--start`/`--length` tbc-video-export runs with produces
chapters that land correctly in that export, including a per-segment export.
The chapter title is the segment's title, or `Segment N` (N is the segment
id); a segment comment becomes the chapter `comment`. The legacy single user
marker is still spliced in. `--ffmetadata-all-segments` includes disabled
segments; `--ffmetadata-no-segments` restores the navigation-only output. A
decode without stored segments is unchanged.

`--segments-json` writes tbc-segments' report (schema 1) from the metadata
alone: the stored segments (derived through the library when none are
stored), the events derived from the field records and any stored picture
metrics, and each segment's 1-based `startFrame`/`lengthFrames` under the
mixed-frame rule, so a caller can export every segment with
`tbc-video-export --start/--length` and know the ranges tile the tape. No TBC
is read. See `docs/Tools/tbc-segments.md`.

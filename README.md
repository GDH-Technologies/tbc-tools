# TBC Tools


This is the complete suite of tools for processing TBC (Time Base Corrected) files from the decode projects

-
[vhs-decode](https://github.com/oyvindln/vhs-decode/wiki) & [cvbs-decode](https://github.com/oyvindln/vhs-decode/wiki/CVBS-Composite-Decode)
- [Tape Decode Rust](https://github.com/harrypm/tape-decode-rust)
- [Decode Dot Net](https://github.com/JunliangRen/vhs-decode-dotnet)

This is an cross-platform feature addition focused continuation based off the original now legacy ld-tools, directly after the cut-off for [decode-orc](https://simoninns.github.io/decode-orc-docs/decode-orc/) development.

These tools are for analyzing decoded analog video sources, handling the 4fsc video data, sound, metadata and final tweaks in framing, chroma-decoding and video levels before converting to YUV digital video files.

Please see the [releases page](https://github.com/harrypm/tbc-tools/releases/) for ready to use self-contained binary's for production usage on Windows, macOS, Linux for x86 and ARM64 platforms.  


# Images


## Main Window & Scopes


<img width="1920" height="" alt="Screenshot from 2026-04-04 19-16-33" src="https://github.com/user-attachments/assets/9458ffcb-6bc5-4801-8716-476921b8506e" />

<img width="1920" height="" alt="Screenshot from 2026-04-04 19-12-14" src="https://github.com/user-attachments/assets/b5d43138-07d3-4eae-9b65-389df2963792" />


## Export Page


<img width="1354" height="" alt="Screenshot from 2026-05-02 07-02-52" src="https://github.com/user-attachments/assets/e6241648-a911-445b-a777-629a691ce7d6" />


## VBI Decoding


<img width="422" height="404" alt="image" src="https://github.com/user-attachments/assets/e9d748b6-d494-43a1-a1d6-2c8a3f1753c3" />


## Chapter Marker Control 


<img width="600" height="" alt="image" src="https://github.com/user-attachments/assets/4c335698-4413-4335-a31f-c7ec889c0f75" />

<img width="600" height="" alt="tbc-analyse_chapter_markers" src="https://github.com/user-attachments/assets/a89a584b-1221-4b0f-bb6f-023eed65ce74" />


## Auto Audio Align


<img width="400" height="" alt="image" src="https://github.com/user-attachments/assets/fc9c757f-8bc3-464f-a72b-4609581cb550" />


## Teletext Viewer

<img width="400" height="" alt="Teletext_Viewer" src="https://github.com/user-attachments/assets/add401fd-ecc6-46d4-8bb8-658a53ea5497" />


## Tool Categories


### Core Processing Tools

- **tbc-analyse**       - GUI tool for TBC file visual analysis & adjustment handler. Supports drag-and-drop loading of `.tbc`, `.ytbc`, `.ctbc`, `.tbcy`, `.tbcc`, `.db`, and `.json` files into the main window.

- **tbc-process-vbi**   - Decode Vertical Blanking Interval data. Closed Captions, VITC - Vertical Interval Time Code, XDS Data and VITS metrics Teletext and more!

- **tbc-dropout-correct**  - Advanced dropout detection and correction

- **tbc-chroma-decoder**   - Color decoder for 4fsc TBC CVBS/S-Video data to YUV conversion (BBC PAL Transform 2D & Transform 3D, MESECAM, SECAM, NTSC 3D, nnTransfrom 3D colour decoding) 


### EFM Decoder Suite

*Replaces deprecated ld-process-efm with staged decoding and stacking capabilities*

- **ac3-decoder**       - Decode AC3 data from demodulated AC3-RF QPSK symbols

- **efm-handler**        - GUI handling tool for stages automatic use.

- **efm-decoder-f2**     - Convert EFM T-values to F2 sections

- **efm-decoder-d24**    - Convert F2 sections to Data24 format

- **efm-decoder-audio**  - Convert EFM Data24 sections to 16-bit stereo PCM audio
- **efm-decoder-data**   - Convert EFM Data24 sections to ECMA-130 binary data

- **efm-stacker-f2**     - Combine multiple F2 captures for improved quality


### Tools

- **ld-discmap**           - TBC and VBI alignment and correction tool

- **ld-disc-stacker**      - Combine multiple TBC captures for improved quality

### Export and Conversion Tools

- **tbc-video-export**        - Direct to video export task handler for .tbc files via CLI with a visual readout. 

- **tbc-export-metadata**     - Export TBC metadata to external formats I.G FFmpeg. 

- **tbc-metadata-converter**  - Convert between JSON & SQLite metadata formats. 


- **lds-converter**        - Convert and bulk FLAC compress legacy 10-bit packed and 16-bit data formats for legacy DdD app captures.


## Getting Started

[MISRC GUI](https://github.com/harrypm/MISRC-GUI) --> Decoder of Choise --> 4fsc TBC + Metadata + Audio --> TBC-Tools --> Video Files. 

**Analyse & Adjust**: Use `tbc-analyse` to assess capture quality and identify issues such as off-set active picture area or chroma phase/gain & video levels corrections.

**Decode VBI**: If any usable DBI data is present, decode it and save that to metadata or automatically prepare it for export with standard files.

**Align**: Using Auto Audio Align, you can easily align your same clock source capture to your metadata and load in the audio for perfect cut.

**Set in/out**: Using the timeline set your exact in/out cut points and any chapter markers (exported into video) for points of interest. 

**Export**: Convert to final formats using the export page to chroma-decode and encode via FFmpeg profile to a main and or proxy video files.


## Important Notes


- **Metadata Formats**: SQLite (`.tbc.db`) is the 2026-present metadata format, JSON metadata (2017-2026) being still supported by the tools and used by many older builds for the decode suite.

- **File Extensions**: TBC files use `.tbc` extension; metadata is commonly `.tbc.json` & `.tbc.db` (SQLite) 

- **Dependencies**: Most tools require FFmpeg and other multimedia libraries.

- **Performance**: Many tools support multi-threading for fast real-time or faster processing.


## Documentation


Each tool directory contains detailed README.md files with:
- Basic usage instructions
- Complete option references
- Usage examples
- Input/output format specifications
- Troubleshooting References

See individual tool directories for specific tool documentation and [the wiki](https://github.com/harrypm/tbc-tools/wiki) for more info.

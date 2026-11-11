# tbc-segments: recording-boundary analysis. Everything segment-related that
# tbc-tools carries lives under this directory, so the module can move as one:
#   core/  tbc-segments-core, the rules (Qt Core): derivation, report, field walk
#   gui/   tbc-segments-gui, the tbc-analyse side (Qt Widgets): Segments viewer
#   cli/   the tbc-segments command
add_subdirectory(core)
add_subdirectory(gui)
add_subdirectory(cli)

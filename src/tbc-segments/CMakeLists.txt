# tbc-segments: segment analysis. Everything segment-related that
# tbc-tools carries lives under this directory, so the module can move as one:
#   core/  tbc-segments-core, the rules (Qt Core): derivation, report, field walk
#   gui/   tbc-segments-gui, the tbc-analyse side (Qt Widgets): Segments viewer
#   cli/   the tbc-segments command
add_subdirectory(core)
# glossary/: glossary.json, the one source of the segment vocabulary, and the
# script that generates core/glossary_generated.h and
# docs/Tools/tbc-segments-glossary.md from it (both committed)
add_subdirectory(gui)
add_subdirectory(cli)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # The generated glossary files match glossary.json, every term is used,
        # and no retired word is left (docs/Tools/tbc-segments-glossary.md)
        add_test(NAME tbc-segments-glossary
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/glossary/generate_glossary.py --check --lint)
    endif()
endif()

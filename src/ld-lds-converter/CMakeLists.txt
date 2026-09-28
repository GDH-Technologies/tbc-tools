set(_ld_lds_converter_flac_target "")

find_package(FLAC 1.5 QUIET CONFIG)
if(TARGET FLAC::FLAC)
    set(_ld_lds_converter_flac_target FLAC::FLAC)
elseif(TARGET FLAC::libFLAC)
    set(_ld_lds_converter_flac_target FLAC::libFLAC)
endif()

if(NOT _ld_lds_converter_flac_target)
    pkg_check_modules(FLAC_PC REQUIRED IMPORTED_TARGET "flac>=1.5.0")
    set(_ld_lds_converter_flac_target PkgConfig::FLAC_PC)
endif()
add_executable(ld-lds-converter
    converterdialog.cpp
    converterdialog.ui
    dataconverter.cpp
    main.cpp
)
target_link_libraries(ld-lds-converter PRIVATE
    Qt::Core Qt::Gui Qt::Widgets
    tbc-library
    ${_ld_lds_converter_flac_target}
)

install(TARGETS ld-lds-converter)

if(BUILD_TESTING)
    add_executable(testconverterdialog
        testconverterdialog.cpp
        converterdialog.cpp
        converterdialog.ui
        dataconverter.cpp
    )
    target_link_libraries(testconverterdialog PRIVATE
        Qt::Core Qt::Gui Qt::Widgets
        tbc-library
        ${_ld_lds_converter_flac_target}
    )
    add_test(NAME testconverterdialog COMMAND testconverterdialog)
    # It builds a QApplication (not on Windows; see testconverterdialog.cpp);
    # no display is needed or wanted.
    set_tests_properties(testconverterdialog PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60)

    add_test(
        NAME ld-lds-converter-help
        COMMAND ld-lds-converter --help
    )
    set_tests_properties(ld-lds-converter-help PROPERTIES
        PASS_REGULAR_EXPRESSION "Output format for unpack mode: flac \\(default\\), s16 or riff;FLAC compression level 0-8 \\(default: 8\\)"
    )

    add_test(
        NAME ld-lds-converter-invalid-format
        COMMAND ld-lds-converter --unpack --format invalid
    )
    set_tests_properties(ld-lds-converter-invalid-format PROPERTIES
        WILL_FAIL TRUE
    )

    add_test(
        NAME ld-lds-converter-invalid-compression-level
        COMMAND ld-lds-converter --compression-level 9
    )
    set_tests_properties(ld-lds-converter-invalid-compression-level PROPERTIES
        WILL_FAIL TRUE
    )
endif()

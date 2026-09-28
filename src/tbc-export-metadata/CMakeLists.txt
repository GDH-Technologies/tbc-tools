add_executable(tbc-export-metadata
    audacity.cpp
    closedcaptions.cpp
    csv.cpp
    exportdropouts.cpp
    exportmetadata.cpp
    ffmetadata.cpp
    jsonio.cpp
    metadataconverter.cpp
    segmentsreport.cpp
    vitcffmetadata.cpp
    metadataexportdialog.cpp
    metadataexportdialog.ui
    ../library/gui/processprogressrunner.cpp
    main.cpp
)
target_link_libraries(tbc-export-metadata PRIVATE Qt::Core Qt::Gui Qt::Widgets tbc-library)

install(TARGETS tbc-export-metadata)

if(BUILD_TESTING)
    add_executable(testmetadataexportdialog
        testmetadataexportdialog.cpp
        metadataexportdialog.cpp
        metadataexportdialog.ui
        ../library/gui/processprogressrunner.cpp
    )
    target_link_libraries(testmetadataexportdialog PRIVATE Qt::Core Qt::Gui Qt::Widgets tbc-library)
    add_test(NAME testmetadataexportdialog COMMAND testmetadataexportdialog)
    # It builds a QApplication (not on Windows; see testmetadataexportdialog.cpp);
    # no display is needed or wanted.
    set_tests_properties(testmetadataexportdialog PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60)
endif()

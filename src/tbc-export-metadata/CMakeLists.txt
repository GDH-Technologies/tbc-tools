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

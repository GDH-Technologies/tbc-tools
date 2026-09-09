add_executable(tbc-metadata-converter
    jsonconverter.cpp
    metadataconversiondialog.cpp
    metadataconversiondialog.ui
    metadataconverterutil.cpp
    ../library/gui/processprogressrunner.cpp
    main.cpp
)

target_link_libraries(tbc-metadata-converter PRIVATE Qt::Core Qt::Gui Qt::Widgets Qt::Sql tbc-library)

install(TARGETS tbc-metadata-converter)

if(BUILD_TESTING)
    # metadataconverterutil.cpp launches the tool through ProcessProgressRunner,
    # so the test needs that translation unit (and the Widgets it draws its
    # progress dialog with) even though it only exercises the path helpers.
    add_executable(testmetadataconverterutil
        testmetadataconverterutil.cpp
        metadataconverterutil.cpp
        ../library/gui/processprogressrunner.cpp
    )
    target_include_directories(testmetadataconverterutil PRIVATE ${CMAKE_SOURCE_DIR}/src/library)
    target_link_libraries(testmetadataconverterutil PRIVATE Qt::Core Qt::Gui Qt::Widgets)
    add_test(NAME testmetadataconverterutil COMMAND testmetadataconverterutil)
endif()

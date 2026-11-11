# The tbc-analyse side of the module: the Segments viewer and the controller
# that carries the main window's segment behaviour.
add_library(tbc-segments-gui STATIC
    segmentpreferences.cpp
    segmentpreferences.h
    segmentscontroller.cpp
    segmentscontroller.h
    segmentsviewerdialog.cpp
    segmentsviewerdialog.h
)
target_include_directories(tbc-segments-gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tbc-segments-gui PUBLIC Qt::Core Qt::Gui Qt::Widgets tbc-segments-core)

if(BUILD_TESTING)
    add_executable(testsegmentsviewer tests/testsegmentsviewer.cpp)
    target_link_libraries(testsegmentsviewer PRIVATE tbc-segments-gui)
    add_test(NAME testsegmentsviewer COMMAND testsegmentsviewer)
    # It builds a QApplication (not on Windows; see testsegmentsviewer.cpp); no
    # display is needed or wanted. It takes well under a second, so a hang fails
    # fast.
    set_tests_properties(testsegmentsviewer PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60)

    add_executable(testsegmentscontroller tests/testsegmentscontroller.cpp)
    target_link_libraries(testsegmentscontroller PRIVATE tbc-segments-gui)
    add_test(NAME testsegmentscontroller COMMAND testsegmentscontroller)
    set_tests_properties(testsegmentscontroller PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60)
endif()

add_executable(testsegmentsviewer
    testsegmentsviewer.cpp
    ../segmentsviewerdialog.cpp
    ../segmentsviewerdialog.h
)

target_include_directories(testsegmentsviewer PRIVATE
    ..
)

target_link_libraries(testsegmentsviewer PRIVATE
    Qt::Core Qt::Gui Qt::Widgets
    tbc-library
)

add_test(NAME testsegmentsviewer COMMAND testsegmentsviewer)
# It builds a QApplication (not on Windows; see testsegmentsviewer.cpp); no
# display is needed or wanted. It takes well under a second, so a hang fails
# fast.
set_tests_properties(testsegmentsviewer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 60)

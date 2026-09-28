add_executable(testtheme
    testtheme.cpp
)

target_include_directories(testtheme PRIVATE
    ..
)

target_link_libraries(testtheme PRIVATE
    Qt::Core Qt::Gui Qt::Widgets
    tbc-library
)

add_test(NAME testtheme COMMAND testtheme)
# It builds a QApplication (not on Windows; see testtheme.cpp); no display is
# needed or wanted. It takes well under a second, so a hang fails fast.
set_tests_properties(testtheme PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 60)

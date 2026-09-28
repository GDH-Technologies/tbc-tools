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
# It builds a QApplication; no display is needed or wanted.
set_tests_properties(testtheme PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

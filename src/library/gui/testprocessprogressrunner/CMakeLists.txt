add_executable(testprocessprogressrunner
    testprocessprogressrunner.cpp
    ../processprogressrunner.cpp
    ../processprogressrunner.h
)

target_include_directories(testprocessprogressrunner PRIVATE
    ..
)

target_link_libraries(testprocessprogressrunner PRIVATE
    Qt::Core Qt::Gui Qt::Widgets
)

add_test(NAME testprocessprogressrunner COMMAND testprocessprogressrunner)
# It builds a QApplication (not on Windows; see testprocessprogressrunner.cpp);
# no display is needed or wanted. The cancel tests wait under a second each,
# so a hang fails fast.
set_tests_properties(testprocessprogressrunner PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 60)

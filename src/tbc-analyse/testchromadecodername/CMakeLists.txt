add_executable(testchromadecodername
    testchromadecodername.cpp
    ../tbcsource.cpp
)

target_include_directories(testchromadecodername PRIVATE
    ..
)

target_link_libraries(testchromadecodername PRIVATE
    Qt::Core Qt::Gui Qt::Widgets
    tbc-library tbc-chroma
)

add_test(NAME testchromadecodername COMMAND testchromadecodername)
set_tests_properties(testchromadecodername PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 60)

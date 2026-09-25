add_executable(testconfiguration
    testconfiguration.cpp
    ../configuration.cpp
    ../configuration.h
)

target_include_directories(testconfiguration PRIVATE
    ..
)

target_link_libraries(testconfiguration PRIVATE
    Qt::Core Qt::Gui Qt::Widgets
    tbc-library
)

add_test(NAME testconfiguration COMMAND testconfiguration)

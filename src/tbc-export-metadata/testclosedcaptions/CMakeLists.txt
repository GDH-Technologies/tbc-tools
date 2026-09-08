add_executable(testclosedcaptions
    testclosedcaptions.cpp
    ../closedcaptions.cpp
)

target_include_directories(testclosedcaptions PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(testclosedcaptions PRIVATE Qt::Core tbc-library)

add_test(NAME testclosedcaptions COMMAND testclosedcaptions)

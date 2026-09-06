add_executable(testclosedcaption
    testclosedcaption.cpp
    ../closedcaption.cpp
)

target_include_directories(testclosedcaption PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(testclosedcaption PRIVATE Qt::Core tbc-library)

add_test(NAME testclosedcaption COMMAND testclosedcaption)

target_compile_definitions(testclosedcaption PRIVATE
    TESTDATA_NTSC_DIR="${CMAKE_SOURCE_DIR}/test-data/ntsc"
)

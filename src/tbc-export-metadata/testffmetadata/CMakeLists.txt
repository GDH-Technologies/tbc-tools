add_executable(testffmetadata
    testffmetadata.cpp
    ../ffmetadata.cpp
)

target_include_directories(testffmetadata PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(testffmetadata PRIVATE Qt::Core tbc-library tbc-segments-core)

add_test(NAME testffmetadata COMMAND testffmetadata)

add_executable(testsegments
    testsegments.cpp
)

target_link_libraries(testsegments PRIVATE Qt::Core tbc-library)

add_test(NAME testsegments COMMAND testsegments)

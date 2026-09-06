add_executable(tbc-segments
    fieldmetrics.cpp
    processingpool.cpp
    segments.cpp
    main.cpp
)

target_link_libraries(tbc-segments PRIVATE Qt::Core tbc-library)

install(TARGETS tbc-segments)

if(BUILD_TESTING)
    add_executable(testsegments
        testsegments/testsegments.cpp
        fieldmetrics.cpp
        processingpool.cpp
        segments.cpp
    )
    target_include_directories(testsegments PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(testsegments PRIVATE Qt::Core tbc-library)
    add_test(NAME testsegments COMMAND testsegments)

    # Smoke: the shipped LaserDisc fixture through both tiers.
    add_test(NAME tbc-segments-smoke
             COMMAND tbc-segments ${CMAKE_SOURCE_DIR}/test-data/ntsc/ve-snw-cut.tbc.db
                     --tbc ${CMAKE_SOURCE_DIR}/test-data/ntsc/ve-snw-cut.tbc
                     --json - --quiet --threads 2)
endif()

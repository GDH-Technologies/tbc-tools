add_executable(tbc-segments
    fieldmetrics.cpp
    processingpool.cpp
    main.cpp
)

target_link_libraries(tbc-segments PRIVATE Qt::Core tbc-library)

install(TARGETS tbc-segments)

if(BUILD_TESTING)
    add_executable(testsegmentswalk
        testsegments/testsegments.cpp
        fieldmetrics.cpp
        processingpool.cpp
    )
    target_include_directories(testsegmentswalk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(testsegmentswalk PRIVATE Qt::Core tbc-library)
    add_test(NAME testsegmentswalk COMMAND testsegmentswalk)

    # Smoke: the shipped LaserDisc fixture through both tiers.
    add_test(NAME tbc-segments-smoke
             COMMAND tbc-segments ${CMAKE_SOURCE_DIR}/test-data/ntsc/ve-snw-cut.tbc.db
                     --tbc ${CMAKE_SOURCE_DIR}/test-data/ntsc/ve-snw-cut.tbc
                     --json - --quiet --threads 2)

    # --write must be a no-op once the metadata already holds what it would
    # store, or a backfill rewrites the whole file every time it is run.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME tbc-segments-idempotent
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_SOURCE_DIR}/scripts/test-segments-idempotent.py
                         $<TARGET_FILE:tbc-segments>
                         ${CMAKE_SOURCE_DIR}/test-data/pal/ggv-mb-1khz.tbc.db
                         ${CMAKE_SOURCE_DIR}/test-data/pal/ggv-mb-1khz.tbc)
    endif()
endif()

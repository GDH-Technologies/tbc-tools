# The one place the segment rules live: tbc-segments, tbc-analyse
# and tbc-export-metadata all derive events and segments through this library.
add_library(tbc-segments-core STATIC
    segments.cpp
    segments.h
    glossary_generated.h
    sceneshots.cpp
    sceneshots.h
    shotboundaries.cpp
    shotboundaries.h
    shotboundarystore.cpp
    shotboundarystore.h
    segmentexportplan.cpp
    segmentexportplan.h
    segmentchapters.cpp
    segmentchapters.h
    segmentsderive.cpp
    segmentsderive.h
    segmentsreport.cpp
    segmentsreport.h
    fieldmetrics.cpp
    fieldmetrics.h
    fieldwalkpool.cpp
    fieldwalkpool.h
)
target_include_directories(tbc-segments-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tbc-segments-core PUBLIC Qt::Core tbc-library PRIVATE Qt::Sql)

if(BUILD_TESTING)
    # Metadata tier: synthesised metadata in memory
    add_executable(testsegments tests/testsegments.cpp)
    target_link_libraries(testsegments PRIVATE tbc-segments-core Qt::Sql)
    target_compile_definitions(testsegments PRIVATE TBC_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test-data")
    add_test(NAME testsegments COMMAND testsegments)

    # Field-data tier: the walk over a small synthetic luma + chroma TBC
    add_executable(testsegmentswalk tests/walk/testsegmentswalk.cpp)
    target_link_libraries(testsegmentswalk PRIVATE tbc-segments-core)
    add_test(NAME testsegmentswalk COMMAND testsegmentswalk)
endif()

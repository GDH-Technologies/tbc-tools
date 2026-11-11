# Regression test for the CUDA runtime plugin release selection (tag-prefix
# filtered /releases listing instead of the repo-wide /releases/latest).
add_executable(testcudapluginmanager
    testcudapluginmanager.cpp
    ../cudapluginmanager.cpp
    ../configuration.cpp
    # The fork's configuration.cpp reads and writes FrameSnapshot options
    ../framesnapshot.cpp
)

target_include_directories(testcudapluginmanager PRIVATE
    ..
)

target_link_libraries(testcudapluginmanager PRIVATE
    Qt::Core Qt::Gui Qt::Widgets Qt::Network
    tbc-library
)

add_test(NAME testcudapluginmanager COMMAND testcudapluginmanager)

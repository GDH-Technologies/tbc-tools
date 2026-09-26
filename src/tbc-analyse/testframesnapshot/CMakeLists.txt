add_executable(testframesnapshot
    testframesnapshot.cpp
    ../framesnapshot.cpp
)

target_include_directories(testframesnapshot PRIVATE
    ..
)

target_compile_definitions(testframesnapshot PRIVATE _USE_MATH_DEFINES)

target_link_libraries(testframesnapshot PRIVATE
    Qt::Core Qt::Gui
    tbc-library
)

add_test(NAME testframesnapshot COMMAND testframesnapshot)

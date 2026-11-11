add_executable(testchromadecodername
    testchromadecodername.cpp
    ../tbcsource.cpp
)

target_include_directories(testchromadecodername PRIVATE
    ..
    # tbc-chroma keeps FFTW's include path private, and its headers include
    # fftw3.h; tbc-analyse adds the path the same way
    ${FFTW_INCLUDE_DIR}
)

target_link_libraries(testchromadecodername PRIVATE
    Qt::Core Qt::Gui Qt::Widgets
    tbc-library tbc-chroma tbc-segments-core
)

add_test(NAME testchromadecodername COMMAND testchromadecodername)
set_tests_properties(testchromadecodername PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 60)

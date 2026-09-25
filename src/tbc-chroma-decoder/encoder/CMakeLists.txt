# For M_PI constant
add_compile_definitions(_USE_MATH_DEFINES)


add_executable(tbc-chroma-encoder
    main.cpp
    encoder.cpp
    ntscencoder.cpp
    palencoder.cpp
)

target_link_libraries(tbc-chroma-encoder PRIVATE Qt::Core tbc-library)

install(TARGETS tbc-chroma-encoder)

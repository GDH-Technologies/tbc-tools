add_executable(tbc-dropout-correct
    correctorpool.cpp
    main.cpp
    dropoutcorrect.cpp
)

target_link_libraries(tbc-dropout-correct PRIVATE Qt::Core tbc-library)

install(TARGETS tbc-dropout-correct)

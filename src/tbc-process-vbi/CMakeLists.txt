add_executable(tbc-process-vbi
    biphasecode.cpp
    closedcaption.cpp
    decoderpool.cpp
    fmcode.cpp
    main.cpp
    processvbidialog.cpp
    processvbidialog.h
    teletextintegration.cpp
    vbilinedecoder.cpp
    videoid.cpp
    vitccode.cpp
    whiteflag.cpp
)

target_link_libraries(tbc-process-vbi PRIVATE
    Qt::Core Qt::Gui Qt::Widgets
    tbc-library
)
set(_teletext_vendor_dir "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vhs-teletext")
target_compile_definitions(tbc-process-vbi PRIVATE
    TELETEXT_VENDOR_DIR="${_teletext_vendor_dir}"
)

add_custom_command(TARGET tbc-process-vbi POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tbc-process-vbi>/vendor/vhs-teletext"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${_teletext_vendor_dir}"
        "$<TARGET_FILE_DIR:tbc-process-vbi>/vendor/vhs-teletext"
)

install(TARGETS tbc-process-vbi)
install(DIRECTORY "${_teletext_vendor_dir}/" DESTINATION ${CMAKE_INSTALL_BINDIR}/vendor/vhs-teletext)

if(BUILD_TESTING)
    add_test(
        NAME tbc-process-vbi-teletext-help
        COMMAND $<TARGET_FILE:tbc-process-vbi> --help
    )
    set_tests_properties(tbc-process-vbi-teletext-help PROPERTIES
        PASS_REGULAR_EXPRESSION "--teletext-html-dir"
    )

    add_test(
        NAME tbc-process-vbi-teletext-runtime
        COMMAND python3
            ${CMAKE_SOURCE_DIR}/scripts/test-teletext-export
            --build ${CMAKE_BINARY_DIR}
            --input-base ${CMAKE_SOURCE_DIR}/test-data/ntsc/issue176
            --work-dir ${CMAKE_BINARY_DIR}/test-artifacts/teletext-runtime
    )
    set_tests_properties(tbc-process-vbi-teletext-runtime PROPERTIES
        SKIP_RETURN_CODE 77
    )
endif()

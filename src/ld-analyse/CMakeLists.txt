# For M_PI constant
add_compile_definitions(_USE_MATH_DEFINES)

set(ld-analyse_SOURCES
    blacksnranalysisdialog.cpp blacksnranalysisdialog.ui
    busydialog.cpp busydialog.ui
    ../audio-align/audioalignmentdialog.cpp ../audio-align/audioalignmentdialog.h ../audio-align/audioalignmentdialog.ui
    ../audio-align/audioalignmentutil.cpp ../audio-align/audioalignmentutil.h
    closedcaptionsdialog.cpp closedcaptionsdialog.ui
    efmhandlerdialog.cpp efmhandlerdialog.h
    exportdialog.cpp exportdialog.ui
    exportarguments.cpp exportarguments.h
    metadataconversiondialog.cpp metadataconversiondialog.ui
    metadatastatusdialog.cpp metadatastatusdialog.ui
    notesviewerdialog.cpp notesviewerdialog.h
    segmentsviewerdialog.cpp segmentsviewerdialog.h
    teletextviewerdialog.cpp teletextviewerdialog.h
    teletextnativeviewwidget.cpp teletextnativeviewwidget.h
    main.cpp
    mainwindow.cpp mainwindow.ui
    timelinemarkerslider.cpp timelinemarkerslider.h
    metadataconverterutil.cpp
    ../library/gui/processprogressrunner.cpp ../library/gui/processprogressrunner.h
    ../tbc-export-metadata/metadataexportdialog.cpp ../tbc-export-metadata/metadataexportdialog.h ../tbc-export-metadata/metadataexportdialog.ui
    oscilloscopedialog.cpp oscilloscopedialog.ui
    rgbscopedialog.cpp rgbscopedialog.h
    yuvrangedialog.cpp yuvrangedialog.h
    vectorscopedialog.cpp vectorscopedialog.ui
    waveformmonitordialog.cpp waveformmonitordialog.h
    waveformmonitorwidget.cpp waveformmonitorwidget.h
    tbc_cvbs_conversion.cpp tbc_cvbs_conversion.h
    amplitude_conversion.h
    cvbs_signal_constants.h
    fieldtimingdialog.cpp fieldtimingdialog.h
    fieldtimingwidget.cpp fieldtimingwidget.h
    aboutdialog.cpp aboutdialog.ui
    updatechecker.cpp updatechecker.h
    cudapluginmanager.cpp cudapluginmanager.h
    pluginmanagerdialog.cpp pluginmanagerdialog.h
    plugincatalog.cpp plugincatalog.h
    genericplugininstaller.cpp genericplugininstaller.h
    metadataeditordialog.cpp metadataeditordialog.h metadataeditordialog.ui
    videoparametersdialog.cpp videoparametersdialog.ui
    chromadecoderconfigdialog.cpp chromadecoderconfigdialog.ui
    tbcsource.cpp
    vbidialog.cpp vbidialog.ui
    vbiprocessingdialog.cpp vbiprocessingdialog.h
    configuration.cpp
    dropoutanalysisdialog.cpp dropoutanalysisdialog.ui
    visibledropoutanalysisdialog.cpp visibledropoutanalysisdialog.ui
    whitesnranalysisdialog.cpp whitesnranalysisdialog.ui
    plotwidget.cpp
)

qt_add_resources(ld-analyse_SOURCES ld-analyse-resources.qrc)

if(WIN32)
set(APP_ICON_RESOURCE_WINDOWS "${CMAKE_CURRENT_SOURCE_DIR}/appicon.rc")

add_executable(ld-analyse MACOSX_BUNDLE
    ${ld-analyse_SOURCES}
	${APP_ICON_RESOURCE_WINDOWS})
elseif(UNIX)
	add_executable(ld-analyse MACOSX_BUNDLE
    ${ld-analyse_SOURCES})
endif()

if(APPLE)
    set(APP_ICON_RESOURCE_MACOS "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/appicon.icns")
    target_sources(ld-analyse PRIVATE ${APP_ICON_RESOURCE_MACOS})
    set_source_files_properties(${APP_ICON_RESOURCE_MACOS} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    set_target_properties(ld-analyse PROPERTIES MACOSX_BUNDLE_ICON_FILE "appicon.icns")
endif()


target_include_directories(ld-analyse PRIVATE
    ${FFTW_INCLUDE_DIR}
)

target_link_libraries(ld-analyse PRIVATE
    Qt::Core Qt::Gui Qt::Widgets Qt::Svg Qt::Network
    tbc-library tbc-chroma
)
stage_windows_onnxruntime_runtime(ld-analyse)
stage_windows_qt_platform_runtime(ld-analyse)
add_executable(tbc-efm-handler
    efmhandler-main.cpp
    efmhandlerdialog.cpp
    efmhandlerdialog.h
    # EfmHandlerDialog optionally remembers its last-used directory through
    # Configuration. The standalone handler never sets one (the pointer stays
    # null and no settings are touched), but the symbols must still link.
    configuration.cpp
    configuration.h
)

target_link_libraries(tbc-efm-handler PRIVATE
    Qt::Core Qt::Gui Qt::Widgets
    tbc-library
)

set(_audio_align_vendor_dir "${CMAKE_SOURCE_DIR}/src/audio-align/vendor/vhs_decode_auto_audio_align")
set(_teletext_vendor_dir "${CMAKE_SOURCE_DIR}/src/ld-process-vbi/vendor/vhs-teletext")
target_compile_definitions(ld-analyse PRIVATE
    AUDIO_ALIGN_VENDOR_DIR="${_audio_align_vendor_dir}"
    TELETEXT_VENDOR_DIR="${_teletext_vendor_dir}"
)
add_custom_command(TARGET ld-analyse POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ld-analyse>/vendor/vhs_decode_auto_audio_align"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_audio_align_vendor_dir}/VhsDecodeAutoAudioAlign.exe"
        "${_audio_align_vendor_dir}/Binah.dll"
        "${_audio_align_vendor_dir}/LICENSE-app.txt"
        "${_audio_align_vendor_dir}/LICENSE-Binah.txt"
        "${_audio_align_vendor_dir}/CHANGELOG.md"
        "${_audio_align_vendor_dir}/build-info.txt"
        "${_audio_align_vendor_dir}/VENDOR_INFO.txt"
        "$<TARGET_FILE_DIR:ld-analyse>/vendor/vhs_decode_auto_audio_align"
)
add_custom_command(TARGET ld-analyse POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ld-analyse>/vendor/vhs-teletext"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${_teletext_vendor_dir}"
        "$<TARGET_FILE_DIR:ld-analyse>/vendor/vhs-teletext"
)
install(DIRECTORY "${_teletext_vendor_dir}/" DESTINATION ${CMAKE_INSTALL_BINDIR}/vendor/vhs-teletext)

if(WIN32)
    target_compile_definitions(ld-analyse PRIVATE
        NOMINMAX
        QUSB_LIBRARY
    )
    set_property(TARGET ld-analyse PROPERTY WIN32_EXECUTABLE true)
endif()

# Binaries install on every platform; only the XDG desktop integration below is
# platform-specific. (The previous if(UNIX)/else() duplicated these two calls in
# both arms, and the else() arm was unreachable - it required neither WIN32 nor
# UNIX.)
install(TARGETS ld-analyse BUNDLE DESTINATION bin)
install(TARGETS tbc-efm-handler)

# Desktop integration is Linux/BSD only. It was previously gated on if(UNIX),
# which is true on macOS too, so a Darwin build installed a .desktop, a MIME
# package and four hicolor PNGs into a bundle prefix where nothing reads them.
if(UNIX AND NOT APPLE)
    set(_dd_src "${CMAKE_CURRENT_SOURCE_DIR}/install")

    # decode-desktop-sync mirrors the assets below into $XDG_DATA_HOME and
    # rebuilds the caches there. That step is not optional on a Nix install:
    # nix never runs the cache builders, so share/mime/ here has only
    # packages/ and the type->app index (mimeinfo.cache) is never written.
    # PROGRAMS rather than FILES - it needs 0755.
    install(PROGRAMS "${_dd_src}/decode-desktop-sync"
        DESTINATION "${CMAKE_INSTALL_BINDIR}")

    # Shared-mime-info packages. tbc-formats.xml supersedes the old
    # application-video-tbc.xml wholesale (it defines application/video-tbc
    # itself, plus the five subtypes); shipping both would double-define the
    # type and make update-mime-database complain for every consumer.
    #
    # orcprj.xml defines application/x-orcprj for decode-orc. It is named for
    # the type rather than reverse-DNS on purpose - see the comment in the file.
    # The matching handler is NOT installed here: it can only live inside
    # decode-orc's own desktop entry, which upstream installs into the same nix
    # profile, so decode-desktop-sync writes the patched copy into
    # $XDG_DATA_HOME/applications instead, where XDG precedence makes it win.
    install(FILES
            "${_dd_src}/tbc-formats.xml"
            "${_dd_src}/rf-captures.xml"
            "${_dd_src}/orcprj.xml"
            "${_dd_src}/closed-captions.xml"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/mime/packages")

    # ld-analyse.desktop ONLY. The decode-orc and vhs-decode entries in
    # install/ are overrides for binaries this package does not build; two of
    # them would collide with their own package's file in a shared prefix.
    install(FILES "${_dd_src}/ld-analyse.desktop"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")

    # Application icons.
    foreach(_dd_size IN ITEMS 16 32 64 128)
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/${_dd_size}-analyse.png"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${_dd_size}x${_dd_size}/apps"
            RENAME ld-analyse.png)
    endforeach()

    # Mimetype icons. PATTERN matches only the last path component, so the
    # directory filter has to be a REGEX over the whole path. The apps/ icons
    # in the same tree belong to vhs-decode and stay asset-only.
    install(DIRECTORY "${_dd_src}/icons/"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons"
        FILES_MATCHING REGEX "/mimetypes/.*\\.(png|svg)$")

    # The sync tool's own asset tree, including the four files that must never
    # appear in share/applications or share/mime/packages. No
    # USE_SOURCE_PERMISSIONS: a git checkout carries no meaningful modes for
    # data files, and CMake's 0644/0755 default is what we want.
    install(DIRECTORY "${_dd_src}/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/decode-desktop"
        PATTERN "decode-desktop-sync" EXCLUDE)

    # Refresh the XDG caches after a plain `cmake --install` to a live prefix -
    # the one path where nothing else ever will. Skipped when DESTDIR is set
    # (the packager owns the caches) and when the prefix is a nix store path
    # (the caches there are never read; decode-desktop-sync refreshes the ones
    # that are). Resolved with find_program at configure time rather than
    # `which` at install time, and CMAKE_INSTALL_PREFIX stays deferred so
    # `cmake --install --prefix=` is honoured.
    option(TBC_REFRESH_XDG_CACHES
        "Refresh mime/desktop/icon caches after a non-staged install" ON)
    find_program(UPDATE_MIME_DATABASE_EXECUTABLE update-mime-database)
    find_program(UPDATE_DESKTOP_DATABASE_EXECUTABLE update-desktop-database)
    find_program(GTK_UPDATE_ICON_CACHE_EXECUTABLE gtk-update-icon-cache)

    if(TBC_REFRESH_XDG_CACHES)
        install(CODE "
set(_dd_share \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}\")
if(NOT \"\$ENV{DESTDIR}\" STREQUAL \"\")
    message(STATUS \"decode-desktop: DESTDIR set - the packager owns the XDG caches\")
elseif(_dd_share MATCHES \"^/nix/store/\")
    message(STATUS \"decode-desktop: nix store prefix - decode-desktop-sync refreshes the caches that are read\")
else()
    if(EXISTS \"${UPDATE_MIME_DATABASE_EXECUTABLE}\")
        execute_process(COMMAND \"${UPDATE_MIME_DATABASE_EXECUTABLE}\" \"\${_dd_share}/mime\"
                        OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(EXISTS \"${UPDATE_DESKTOP_DATABASE_EXECUTABLE}\")
        execute_process(COMMAND \"${UPDATE_DESKTOP_DATABASE_EXECUTABLE}\" \"\${_dd_share}/applications\"
                        OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(EXISTS \"${GTK_UPDATE_ICON_CACHE_EXECUTABLE}\")
        execute_process(COMMAND \"${GTK_UPDATE_ICON_CACHE_EXECUTABLE}\" -f -t -q \"\${_dd_share}/icons/hicolor\"
                        OUTPUT_QUIET ERROR_QUIET)
    endif()
endif()
")
    endif()
endif()

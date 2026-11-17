# Regression test for tbc-analyse quitting mid-export: the export QProcess
# must be stopped, with its signals cut from the dialog, before the dialog's
# UI is deleted (ExportDialog::handleProcessError crashed in ~ExportDialog).
#
# Unix only, the executable included: it signals real `sleep`/`sh` children
# with POSIX kill(), which MSVC does not have.
if(WIN32)
    return()
endif()

add_executable(testprocessshutdown
    testprocessshutdown.cpp
    ../processshutdown.cpp
)

target_include_directories(testprocessshutdown PRIVATE
    ..
)

target_link_libraries(testprocessshutdown PRIVATE
    Qt::Core
)

add_test(NAME testprocessshutdown COMMAND testprocessshutdown)
set_tests_properties(testprocessshutdown PROPERTIES TIMEOUT 60)

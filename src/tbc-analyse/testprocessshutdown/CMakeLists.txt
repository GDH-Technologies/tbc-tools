# Regression test for tbc-analyse quitting mid-export: the export QProcess
# must be stopped, with its signals cut from the dialog, before the dialog's
# UI is deleted (ExportDialog::handleProcessError crashed in ~ExportDialog).
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

# Unix only: it signals real `sleep`/`sh` children with SIGINT.
if(NOT WIN32)
    add_test(NAME testprocessshutdown COMMAND testprocessshutdown)
    set_tests_properties(testprocessshutdown PROPERTIES TIMEOUT 60)
endif()

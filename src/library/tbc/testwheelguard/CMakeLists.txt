add_executable(testwheelguard
    testwheelguard.cpp
)

# The guard is header-only: src/library/tbc/wheelguard.h
target_include_directories(testwheelguard PRIVATE
    ../..
)

target_link_libraries(testwheelguard PRIVATE
    Qt::Core Qt::Gui Qt::Widgets Qt::Test
)

add_test(NAME testwheelguard COMMAND testwheelguard)
# It builds a QApplication and sends real wheel events (not on Windows; see
# testwheelguard.cpp); no display is needed or wanted.
set_tests_properties(testwheelguard PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 60)

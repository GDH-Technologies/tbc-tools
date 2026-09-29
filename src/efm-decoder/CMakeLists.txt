cmake_minimum_required(VERSION 3.16)

# Set the project name
project(efm-tools)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

# The build identity (APP_BUILD, APP_SOURCE_ID) is defined in the parent CMakeLists.txt

# Add subdirectories
add_subdirectory(libs/efm)
add_subdirectory(tools/efm-decoder-f2)
add_subdirectory(tools/efm-decoder-d24)
add_subdirectory(tools/efm-decoder-audio)
add_subdirectory(tools/efm-decoder-data)
add_subdirectory(tools/efm-stacker-f2)
add_subdirectory(tools/vfs-verifier)
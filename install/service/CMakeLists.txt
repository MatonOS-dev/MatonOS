cmake_minimum_required(VERSION 3.22)
project(matonos_installer_core CXX)

add_library(matonos-installer-core STATIC InstallerService.cpp)
target_compile_features(matonos-installer-core PUBLIC cxx_std_17)
target_compile_options(matonos-installer-core PRIVATE -Wall -Wextra -Werror -O2 -fPIC)
target_include_directories(matonos-installer-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

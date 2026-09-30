cmake_minimum_required(VERSION 3.22)
project(matonos_installer_service CXX)

set(INSTALL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../install/service")
add_executable(matonos-installer-service
  "${INSTALL_SRC}/InstallerDaemon.cpp"
  "${INSTALL_SRC}/InstallerService.cpp"
  "${INSTALL_SRC}/InstallerExecutor.cpp")
target_sources(matonos-installer-service PRIVATE "${INSTALL_SRC}/LpMetadata.cpp" "${INSTALL_SRC}/FatFilesystem.cpp")
target_include_directories(matonos-installer-service PRIVATE "${INSTALL_SRC}" "${MATON_IPC_INCLUDE_DIR}")
target_compile_features(matonos-installer-service PRIVATE cxx_std_17)
target_compile_options(matonos-installer-service PRIVATE -Wall -Wextra -Werror -O2 -fPIE)
target_link_options(matonos-installer-service PRIVATE -pie)
target_link_libraries(matonos-installer-service PRIVATE log "${MATON_IPC_LIBRARY}")
install(TARGETS matonos-installer-service RUNTIME DESTINATION .)

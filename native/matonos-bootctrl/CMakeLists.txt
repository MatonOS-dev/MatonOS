cmake_minimum_required(VERSION 3.22)
project(matonos_bootctrl CXX)
if(MATON_STABLE_AIDL_COUNT LESS 1)
  message(FATAL_ERROR "Boot control service requires stable VINTF NDK AIDL")
endif()
file(GLOB_RECURSE AIDL_CPP "${MATON_STABLE_AIDL_GENERATED_DIR}/*.cpp")
set(INSTALL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../install/service")
add_executable(matonos-bootctrl main.cpp BootControl.cpp
  "${INSTALL_SRC}/FatFilesystem.cpp" ${AIDL_CPP})
target_include_directories(matonos-bootctrl PRIVATE
  "${MATON_STABLE_AIDL_GENERATED_DIR}" "${MATON_STABLE_AIDL_INCLUDE_DIR}"
  "${INSTALL_SRC}")
target_compile_features(matonos-bootctrl PRIVATE cxx_std_17)
target_compile_options(matonos-bootctrl PRIVATE -Wall -Wextra -Werror -O2 -fPIE)
target_link_options(matonos-bootctrl PRIVATE -pie)
target_link_libraries(matonos-bootctrl PRIVATE "${MATON_BINDER_NDK_LIB}" log)
install(TARGETS matonos-bootctrl RUNTIME DESTINATION .)

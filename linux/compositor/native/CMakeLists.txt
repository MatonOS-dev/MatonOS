cmake_minimum_required(VERSION 3.22.1)
project(maton_compositor LANGUAGES C CXX)

if(NOT MATON_COMPOSITOR_ROOT)
  message(FATAL_ERROR "MATON_COMPOSITOR_ROOT must point to linux/compositor")
endif()
get_filename_component(MATON_COMPOSITOR_ROOT "${MATON_COMPOSITOR_ROOT}" ABSOLUTE)
get_filename_component(MATON_AOSP_ROOT "${MATON_COMPOSITOR_ROOT}/../../../../../" ABSOLUTE)
set(MATON_COMPOSITOR_BUILD "${MATON_AOSP_ROOT}/out/matonos/compositor")
set(MATON_STAGE "${MATON_COMPOSITOR_BUILD}/stage/x86_64")
set(MATON_WAYLAND_SCANNER "${MATON_COMPOSITOR_BUILD}/host/bin/wayland-scanner")
set(MATON_XDG_XML "${MATON_COMPOSITOR_BUILD}/host/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
set(MATON_WLROOTS_INCLUDE "${MATON_COMPOSITOR_BUILD}/sources/src/wlroots-0.20.2/include")

foreach(required "${MATON_WAYLAND_SCANNER}" "${MATON_XDG_XML}")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "Missing ${required}; build the pinned compositor dependencies first")
  endif()
endforeach()

set(GENERATED "${MATON_COMPOSITOR_BUILD}/generated")
file(MAKE_DIRECTORY "${GENERATED}")
add_custom_command(OUTPUT "${GENERATED}/xdg-shell-client-protocol.h"
  COMMAND "${MATON_WAYLAND_SCANNER}" client-header "${MATON_XDG_XML}" "${GENERATED}/xdg-shell-client-protocol.h"
  DEPENDS "${MATON_XDG_XML}" VERBATIM)
add_custom_command(OUTPUT "${GENERATED}/xdg-shell-protocol.c"
  COMMAND "${MATON_WAYLAND_SCANNER}" private-code "${MATON_XDG_XML}" "${GENERATED}/xdg-shell-protocol.c"
  DEPENDS "${MATON_XDG_XML}" VERBATIM)
add_custom_command(OUTPUT "${GENERATED}/wayland-protocols/xdg-shell-enum.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED}/wayland-protocols"
  COMMAND "${MATON_WAYLAND_SCANNER}" enum-header "${MATON_XDG_XML}" "${GENERATED}/wayland-protocols/xdg-shell-enum.h"
  DEPENDS "${MATON_XDG_XML}" VERBATIM)

foreach(lib wlroots wayland-server wayland-client pixman-1 xkbcommon drm)
  if(lib STREQUAL "wlroots")
    find_library(MATON_${lib}_LIB NAMES wlroots-0.20 wlroots PATHS "${MATON_STAGE}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
  else()
    find_library(MATON_${lib}_LIB NAMES ${lib} PATHS "${MATON_STAGE}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
  endif()
  add_library(maton_${lib} SHARED IMPORTED)
  set_target_properties(maton_${lib} PROPERTIES IMPORTED_LOCATION "${MATON_${lib}_LIB}")
endforeach()

add_library(maton_compositor SHARED
  compositor_core.c jni_shim.cpp android_buffer.c android_output.c test_client.c
  "${GENERATED}/xdg-shell-protocol.c" "${GENERATED}/xdg-shell-client-protocol.h"
  "${GENERATED}/wayland-protocols/xdg-shell-enum.h")
target_compile_features(maton_compositor PRIVATE c_std_11 cxx_std_17)
target_compile_definitions(maton_compositor PRIVATE WLR_USE_UNSTABLE)
target_include_directories(maton_compositor PRIVATE
  "${GENERATED}"
  "${MATON_COMPOSITOR_ROOT}/native"
  "${MATON_WLROOTS_INCLUDE}"
  "${MATON_STAGE}/include"
  "${MATON_STAGE}/include/pixman-1"
  "${MATON_AOSP_ROOT}/external/libdrm/include"
  "${MATON_AOSP_ROOT}/external/libdrm/include/drm"
  "${MATON_AOSP_ROOT}/external/minigbm/cros_gralloc"
  "${MATON_AOSP_ROOT}/system/core/libcutils/include")
target_link_libraries(maton_compositor PRIVATE
  maton_wlroots maton_wayland-server maton_wayland-client maton_pixman-1 maton_xkbcommon maton_drm
  android log EGL GLESv2 dl)

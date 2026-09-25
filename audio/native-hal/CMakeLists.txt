cmake_minimum_required(VERSION 3.22)
project(matonos_audio_hal LANGUAGES C CXX)

foreach(required MATON_AIDL_GENERATED_DIR MATON_AUDIO_STATIC_LIB_DIR MATON_AUDIO_DEPS_INCLUDE_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Set ${required}")
  endif()
endforeach()

set(TP "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(HI "${TP}/hardware_interfaces/hardware_interfaces")
set(CORE "${TP}/system_core/system/core")
set(BASE "${TP}/system_libbase/system/libbase")
set(FMQ "${TP}/system_fmq/system/libfmq")
set(FMT "${TP}/fmtlib/external/fmtlib")
set(LOG "${TP}/system_logging/system/logging/liblog/include")
set(ERROR "${AV}/media/liberror/include")

set(MATON_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/aosp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/aosp"
    "${MATON_AIDL_GENERATED_DIR}/include"
    "${MATON_AUDIO_DEPS_INCLUDE_DIR}")

function(maton_static target output)
  add_library(${target} STATIC ${ARGN})
  set_target_properties(${target} PROPERTIES
      OUTPUT_NAME "${output}"
      ARCHIVE_OUTPUT_DIRECTORY "${MATON_AUDIO_STATIC_LIB_DIR}"
      POSITION_INDEPENDENT_CODE ON)
  target_include_directories(${target} PUBLIC ${MATON_INCLUDES})
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_compile_options(${target} PRIVATE -Wno-unused-parameter -Wno-deprecated-declarations)
endfunction()

# Pinned helper libraries. These are deliberately narrow source subsets of the
# corresponding AOSP libraries; their source revisions are in third_party/.
maton_static(maton_fmt fmt
    "${FMT}/src/format.cc" "${FMT}/src/os.cc")
maton_static(maton_base base
    "${BASE}/chrono_utils.cpp" "${BASE}/cmsg.cpp" "${BASE}/errors_unix.cpp"
    "${BASE}/file.cpp" "${BASE}/hex.cpp" "${BASE}/logging.cpp"
    "${BASE}/mapped_file.cpp" "${BASE}/parsebool.cpp" "${BASE}/parsenetaddress.cpp"
    "${BASE}/posix_strerror_r.cpp" "${BASE}/process.cpp" "${BASE}/properties.cpp"
    "${BASE}/result.cpp" "${BASE}/stringprintf.cpp" "${BASE}/strings.cpp"
    "${BASE}/threads.cpp")
target_link_libraries(maton_base PUBLIC maton_fmt log)

maton_static(maton_utils_binder utils_binder
    "${CORE}/libutils/binder/Errors.cpp" "${CORE}/libutils/binder/RefBase.cpp"
    "${CORE}/libutils/binder/SharedBuffer.cpp" "${CORE}/libutils/binder/String16.cpp"
    "${CORE}/libutils/binder/String8.cpp" "${CORE}/libutils/binder/StrongPointer.cpp"
    "${CORE}/libutils/binder/Unicode.cpp" "${CORE}/libutils/binder/VectorImpl.cpp")
set_source_files_properties("${CORE}/libutils/misc.cpp" PROPERTIES
    COMPILE_DEFINITIONS __ANDROID_RECOVERY__)
maton_static(maton_utils utils
    "${CORE}/libutils/FileMap.cpp" "${CORE}/libutils/JenkinsHash.cpp"
    "${CORE}/libutils/LightRefBase.cpp" "${CORE}/libutils/NativeHandle.cpp"
    "${CORE}/libutils/Printer.cpp" "${CORE}/libutils/StopWatch.cpp"
    "${CORE}/libutils/SystemClock.cpp" "${CORE}/libutils/Threads.cpp"
    "${CORE}/libutils/Timers.cpp" "${CORE}/libutils/Tokenizer.cpp"
    "${CORE}/libutils/misc.cpp")
target_link_libraries(maton_utils PUBLIC maton_utils_binder maton_base maton_cutils log)

maton_static(maton_cutils cutils
    "${CORE}/libcutils/ashmem-dev.cpp"
    "${CORE}/libcutils/native_handle.cpp"
    "${CORE}/libcutils/multiuser.cpp")
target_link_libraries(maton_cutils PUBLIC maton_base log)

maton_static(maton_fmq fmq
    "${FMQ}/EventFlag.cpp" "${FMQ}/FmqInternal.cpp")
target_link_libraries(maton_fmq PUBLIC maton_base maton_cutils maton_utils log)

maton_static(maton_audioaidlcommon audioaidlcommon
    "${HI}/audio/aidl/common/StreamWorker.cpp")
target_link_libraries(maton_audioaidlcommon PUBLIC maton_base maton_utils)

file(GLOB_RECURSE FROZEN_AIDL_CPP CONFIGURE_DEPENDS
     "${MATON_AIDL_GENERATED_DIR}/*.cpp")
set(AOSP_CORE_CPP
    src/aosp/Configuration.cpp
    src/aosp/Module.cpp
    src/aosp/Stream.cpp
    src/aosp/Bluetooth.cpp
    src/aosp/SoundDose.cpp
    src/aosp/stub/DriverStubImpl.cpp
    src/aosp/stub/ModuleStub.cpp
    src/aosp/stub/StreamStub.cpp)
add_executable(matonos-audio-hal
    main.cpp MatonConfig.cpp PipeWireModule.cpp NoTrace.cpp ${AOSP_CORE_CPP} ${FROZEN_AIDL_CPP})
target_compile_features(matonos-audio-hal PRIVATE cxx_std_20)
target_compile_definitions(matonos-audio-hal PRIVATE BACKEND_NDK)
target_compile_options(matonos-audio-hal PRIVATE -Wall -Wextra -Werror
    -Wno-deprecated-declarations -Wno-missing-designated-field-initializers
    -Wno-deprecated-enum-float-conversion -Wno-deprecated-copy-with-user-provided-copy)
target_include_directories(matonos-audio-hal PRIVATE ${MATON_INCLUDES})
target_link_libraries(matonos-audio-hal PRIVATE
    -Wl,--start-group
    maton_audioaidlcommon maton_fmq maton_utils maton_utils_binder maton_cutils maton_base maton_fmt
    -Wl,--end-group -Wl,--no-as-needed binder_ndk -Wl,--as-needed log dl m)
# API-35 NDK import stubs omit several platform Binder NDK service-manager and
# scheduler exports. The platform libbinder_ndk shipped on the device exports
# them; leave those references dynamic, then verify the exact undefined-symbol
# set against the explicit allowlist in build-hal.sh.
target_link_options(matonos-audio-hal PRIVATE -Wl,--unresolved-symbols=ignore-all)
install(TARGETS matonos-audio-hal RUNTIME DESTINATION bin/hw)

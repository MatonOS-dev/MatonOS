if(NOT CMAKE_C_COMPILER_LOADED)
  enable_language(C)
endif()

add_executable(matonos-addons-service
    "${CMAKE_CURRENT_LIST_DIR}/matonos-addons-service.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/package_zip.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/monocypher/monocypher.c")
target_include_directories(matonos-addons-service PRIVATE
    "${MATON_IPC_INCLUDE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/monocypher")
target_compile_features(matonos-addons-service PRIVATE cxx_std_17)
target_compile_options(matonos-addons-service PRIVATE -Wall -Wextra -Werror -O2 -fPIE)
target_link_options(matonos-addons-service PRIVATE -pie)
target_link_libraries(matonos-addons-service PRIVATE log z "${MATON_IPC_LIBRARY}")
install(TARGETS matonos-addons-service RUNTIME DESTINATION .)

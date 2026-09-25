cmake_minimum_required(VERSION 3.22)
project(maton_pipewire_proxy LANGUAGES CXX)

add_executable(maton-pipewire-proxy ../../audio/maton-pipewire-proxy.cpp)
target_include_directories(maton-pipewire-proxy PRIVATE "${MATON_IPC_INCLUDE_DIR}")
target_compile_features(maton-pipewire-proxy PRIVATE cxx_std_20)
target_compile_options(maton-pipewire-proxy PRIVATE -Wall -Wextra -Werror)
target_link_libraries(maton-pipewire-proxy PRIVATE log "${MATON_IPC_LIBRARY}")
install(TARGETS maton-pipewire-proxy RUNTIME DESTINATION bin)

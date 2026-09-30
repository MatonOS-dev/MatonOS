# Fixed out-of-Soong native registry hook; implementation remains owned by
# addons/ and this target-only include avoids duplicating source/build logic.
cmake_minimum_required(VERSION 3.22)
project(matonos_addons_service CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../addons/native/matonos-addons-service/targets.cmake")

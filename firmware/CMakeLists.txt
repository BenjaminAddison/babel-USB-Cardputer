cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(babel-cardputer)
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_SOURCE_DIR}/include" APPEND)

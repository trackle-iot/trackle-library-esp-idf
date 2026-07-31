cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Wrapper sources trip GCC 15 -Werror on enum/aliasing; keep DUT build usable.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=strict-aliasing" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=discarded-qualifiers" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=deprecated-enum-enum-conversion" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=deprecated" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-error=deprecated-enum-enum-conversion" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-error=deprecated" APPEND)

project(trackle-esp-idf-dut)

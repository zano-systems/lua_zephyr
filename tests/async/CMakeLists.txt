# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../../")

include(${ZEPHYR_EXTRA_MODULES}/luaz.cmake)
luaz_define_source_thread(suite.lua)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(async_tests)

luaz_generate_threads()

foreach(t api cancel errors hybrid join readers ring sched)
    luaz_add_file(test_${t}.lua)
endforeach()

target_sources(app PRIVATE src/testlib.c)

# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../../")

include(${ZEPHYR_EXTRA_MODULES}/luaz.cmake)
if(ASYNC_BYTECODE)
    luaz_define_bytecode_thread(src/three_tasks.lua)
else()
    luaz_define_source_thread(src/three_tasks.lua)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(async)

luaz_generate_threads()

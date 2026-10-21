# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../../")

include(${ZEPHYR_EXTRA_MODULES}/luaz.cmake)

if(ASYNC_NAMES_FS)
    # Nothing is mounted: loading fails and reports the path as written.
    luaz_define_async_thread(names FS "dir \"q\"/back\\slash.lua")
else()
    # Same basename twice, and a stem that is not a C identifier.
    luaz_define_async_thread(names SOURCE src/a/foo.lua src/b/foo.lua src/my-file.lua)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(async_names_test)

luaz_generate_threads()

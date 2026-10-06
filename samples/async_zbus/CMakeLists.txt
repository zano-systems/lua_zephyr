# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../../")

include(${ZEPHYR_EXTRA_MODULES}/luaz.cmake)
luaz_define_source_thread(src/listener.lua)
luaz_define_source_thread(src/plain.lua)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(async_zbus)

luaz_generate_threads()

target_sources(app PRIVATE src/publisher.c)

# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../../")

include(${ZEPHYR_EXTRA_MODULES}/luaz.cmake)
luaz_define_source_thread(bench.lua)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(async_bench)

luaz_generate_threads()

target_sources(app PRIVATE src/benchlib.c)

# Per-board cycles/op baseline (scripts/bench_table.py --emit-baseline).
set(BENCH_BOARD "${NORMALIZED_BOARD_TARGET}")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/baselines/${BENCH_BOARD}.lua")
    luaz_add_file(baselines/${BENCH_BOARD}.lua)
    target_compile_definitions(app PRIVATE
        BENCH_BASELINE=${BENCH_BOARD}_lua_script
        "BENCH_BASELINE_HEADER=\"${BENCH_BOARD}_lua_script.h\"")
endif()

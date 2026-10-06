# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../../")

include(${ZEPHYR_EXTRA_MODULES}/luaz.cmake)

set(WORKERS
    src/alpha.lua src/beta.lua src/gamma.lua src/delta.lua
    src/epsilon.lua src/zeta.lua src/eta.lua src/theta.lua
    src/iota.lua src/kappa.lua src/lambda.lua src/mu.lua
    src/nu.lua src/xi.lua src/omicron.lua src/pi.lua)

if(ASYNC_BYTECODE)
    luaz_define_async_thread(workers BYTECODE ${WORKERS})
else()
    luaz_define_async_thread(workers SOURCE ${WORKERS})
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(async_files)

luaz_generate_threads()

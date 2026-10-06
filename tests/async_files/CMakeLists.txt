# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../../")

include(${ZEPHYR_EXTRA_MODULES}/luaz.cmake)

# CONFIG_LUA_ASYNC_MAX_TASKS (40) files, written at configure time.
set(TASKS "")
foreach(i RANGE 39)
    set(_f "${CMAKE_BINARY_DIR}/tasks/t${i}.lua")
    file(WRITE "${_f}"
"ran = (ran or 0) + 1\n\
if ran == 40 then\n\
	require(\"zephyr\").printk(\"all 40 tasks ran\")\n\
end\n")
    list(APPEND TASKS "${_f}")
endforeach()
luaz_define_async_thread(many SOURCE ${TASKS})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(async_files_test)

luaz_generate_threads()

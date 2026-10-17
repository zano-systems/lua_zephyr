# luaz.cmake — Lua-Zephyr CMake API.
#
# This file is included TWICE during a build:
#
#   1. Pre-Zephyr  — by the application CMakeLists.txt, before find_package(Zephyr).
#      Only the luaz_define_*_thread() macros and Kconfig generation run here.
#
#   2. Post-Zephyr — by the module CMakeLists.txt, after find_package(Zephyr).
#      The luac host build, luaz_add_*() functions, and luaz_generate_threads()
#      become usable here.
#
# Example:
#   include(${ZEPHYR_EXTRA_MODULES}/luaz.cmake)
#   luaz_define_bytecode_thread(src/producer.lua)
#   luaz_define_bytecode_thread(src/consumer.lua)
#   find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
#   project(my_app)
#   luaz_generate_threads()

# ===========================================================================
# Section 1: Pre-Zephyr — thread definitions and Kconfig generation
# ===========================================================================

set(_LUAZ_KCONFIG_DIR "${CMAKE_BINARY_DIR}/Kconfig")
file(MAKE_DIRECTORY "${_LUAZ_KCONFIG_DIR}")

# _luaz_write_thread_kconfig(NAME [ASYNC])
#
# With ASYNC the thread always runs under the scheduler (no prompt).
macro(_luaz_write_thread_kconfig _name)
    string(TOUPPER "${_name}" _name_upper)
    set(_kconfig_file "${_LUAZ_KCONFIG_DIR}/Kconfig.lua_thread.${_name}")

    if("${ARGN}" STREQUAL "ASYNC")
        set(_async_kconfig
"config ${_name_upper}_LUA_THREAD_ASYNC\n\
    def_bool LUA_ASYNC\n")
    else()
        set(_async_kconfig
"config ${_name_upper}_LUA_THREAD_ASYNC\n\
    bool \"${_name} runs under the async scheduler\"\n\
    depends on LUA_ASYNC\n\
    help\n\
      Run the ${_name} script under the zephyr.async scheduler.\n")
    endif()

    file(WRITE "${_kconfig_file}"
"${_async_kconfig}\
\n\
config ${_name_upper}_LUA_THREAD_STACK_SIZE\n\
    int \"${_name} Lua thread stack size\"\n\
    default LUA_ASYNC_THREAD_STACK_SIZE if ${_name_upper}_LUA_THREAD_ASYNC\n\
    default LUA_THREAD_STACK_SIZE\n\
    help\n\
      Stack size for the ${_name} Lua thread.\n\
\n\
config ${_name_upper}_LUA_ASYNC_MAX_READERS\n\
    int \"${_name} awaited (task, channel) pairs\"\n\
    depends on ${_name_upper}_LUA_THREAD_ASYNC\n\
    range 0 8192\n\
    default LUA_ASYNC_MAX_READERS\n\
    help\n\
      Readers in the ${_name} thread's slab; 0 means min(2 x tasks, tasks x channels).\n\
\n\
config ${_name_upper}_LUA_THREAD_HEAP_SIZE\n\
    int \"${_name} Lua thread heap size\"\n\
    default LUA_THREAD_HEAP_SIZE\n\
    help\n\
      Heap size for the ${_name} Lua thread.\n\
\n\
config ${_name_upper}_LUA_THREAD_PRIORITY\n\
    int \"${_name} Lua thread priority\"\n\
    default LUA_THREAD_PRIORITY\n\
    help\n\
      Priority for the ${_name} Lua thread.\n"
    )
endmacro()

# luaz_define_source_thread(FILE_NAME_PATH)
#
# Define a source-embedded Lua thread. Appends to LUAZ_SOURCE_THREADS
# and generates the per-thread Kconfig fragment.
macro(luaz_define_source_thread _path)
    list(APPEND LUAZ_SOURCE_THREADS "${_path}")
    set(_lua_thread_path "${_path}")
    cmake_path(GET _lua_thread_path FILENAME _lua_fname)
    cmake_path(REMOVE_EXTENSION _lua_fname OUTPUT_VARIABLE _lua_fname)
    _luaz_write_thread_kconfig("${_lua_fname}")
endmacro()

# luaz_define_bytecode_thread(FILE_NAME_PATH)
#
# Define a bytecode Lua thread. Appends to LUAZ_BYTECODE_THREADS
# and generates the per-thread Kconfig fragment.
macro(luaz_define_bytecode_thread _path)
    list(APPEND LUAZ_BYTECODE_THREADS "${_path}")
    set(_lua_thread_path "${_path}")
    cmake_path(GET _lua_thread_path FILENAME _lua_fname)
    cmake_path(REMOVE_EXTENSION _lua_fname OUTPUT_VARIABLE _lua_fname)
    _luaz_write_thread_kconfig("${_lua_fname}")
endmacro()

# luaz_define_fs_thread(SCRIPT_FS_PATH)
#
# Define an FS-backed Lua thread. Appends to LUAZ_FS_THREADS
# and generates the per-thread Kconfig fragment.
macro(luaz_define_fs_thread _path)
    list(APPEND LUAZ_FS_THREADS "${_path}")
    set(_lua_thread_path "${_path}")
    cmake_path(GET _lua_thread_path FILENAME _lua_fname)
    cmake_path(REMOVE_EXTENSION _lua_fname OUTPUT_VARIABLE _lua_fname)
    _luaz_write_thread_kconfig("${_lua_fname}")
endmacro()

# luaz_define_async_thread(NAME SOURCE|BYTECODE|FS FILE...)
#
# Define an async Lua thread NAME whose files all run as tasks in one
# scheduler, in the order given. SOURCE embeds the files, BYTECODE embeds
# them precompiled (CONFIG_LUA_PRECOMPILE), FS loads them from these
# filesystem paths at runtime (CONFIG_LUA_FS). Requires CONFIG_LUA_ASYNC.
macro(luaz_define_async_thread _name _mode)
    if(NOT "${_mode}" MATCHES "^(SOURCE|BYTECODE|FS)$")
        message(FATAL_ERROR
            "luaz_define_async_thread(${_name}): mode must be SOURCE, BYTECODE or FS, "
            "got '${_mode}'")
    endif()
    if("${ARGN}" STREQUAL "")
        message(FATAL_ERROR "luaz_define_async_thread(${_name}): no files given")
    endif()
    list(APPEND LUAZ_ASYNC_THREADS "${_name}")
    set(LUAZ_ASYNC_THREAD_${_name}_MODE "${_mode}")
    set(LUAZ_ASYNC_THREAD_${_name}_FILES ${ARGN})
    _luaz_write_thread_kconfig("${_name}" ASYNC)
endmacro()

# ===========================================================================
# Section 2: Post-Zephyr — host luac build, code generation, thread creation
# ===========================================================================

# Build host luac at configure time (when CONFIG_LUA_PRECOMPILE is enabled).
# This compiles all Lua core sources + luac.c for the host platform, producing
# a native luac binary used to pre-compile .lua scripts to bytecode.
# Skipped during the pre-Zephyr include (CONFIG_LUA_PRECOMPILE is not yet set).
if(CONFIG_LUA_PRECOMPILE)
    set(LUA_MOD_DIR "${CMAKE_CURRENT_LIST_DIR}")
    find_program(HOST_CC NAMES cc gcc REQUIRED)

    set(LUAC_HOST "${CMAKE_CURRENT_BINARY_DIR}/host_tools/luac" CACHE INTERNAL
        "Path to host-built luac binary for Lua bytecode pre-compilation")

    if(NOT EXISTS "${LUAC_HOST}")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/host_tools")
        file(GLOB LUAC_HOST_SRCS "${LUA_MOD_DIR}/lua/l*.c")
        list(REMOVE_ITEM LUAC_HOST_SRCS "${LUA_MOD_DIR}/lua/lua.c" "${LUA_MOD_DIR}/lua/ltests.c")
        list(APPEND LUAC_HOST_SRCS "${LUA_MOD_DIR}/host_tools/luac.c")

        message(STATUS "Building host luac: ${LUAC_HOST}")
        execute_process(
            COMMAND ${HOST_CC}
                ${LUAC_HOST_SRCS}
                -I${LUA_MOD_DIR}/include
                -I${LUA_MOD_DIR}/lua
                -O2 -DLUA_USE_POSIX -DLUA_32BITS -lm
                -o ${LUAC_HOST}
            RESULT_VARIABLE LUAC_BUILD_RESULT
            ERROR_VARIABLE LUAC_BUILD_ERROR
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()
endif()

# Path to the unified Lua generator script used by all luaz_add_* functions.
set(LUA_GENERATE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/scripts/luaz_gen.py"
    CACHE INTERNAL "Path to luaz_gen.py")


# luaz_add_file(FILE_NAME_PATH)
#
# Embed a .lua script as a C const string header.
#
# Runs luaz_gen.py in source mode to produce <name>_lua_script.h.
# The generated header is placed under ${CMAKE_CURRENT_BINARY_DIR}/lua
# and made available via include_directories.
#
# The .lua source file is tracked as a dependency so that modifying it
# triggers regeneration on incremental builds.
#
# Arguments:
#   FILE_NAME_PATH - Path to the .lua file (relative to project source dir).
function(luaz_add_file FILE_NAME_PATH)
    cmake_path(GET FILE_NAME_PATH FILENAME FILE_NAME)
    cmake_path(REMOVE_EXTENSION FILE_NAME OUTPUT_VARIABLE FILE_NAME)

    set(LUA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${FILE_NAME_PATH}")
    set(LUA_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/lua")
    set(LUA_OUTPUT "${LUA_OUTPUT_DIR}/${FILE_NAME}_lua_script.h")
    set(LUA_TEMPLATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/lua_template.h.in")

    add_custom_command(
        OUTPUT "${LUA_OUTPUT}"
        COMMAND ${PYTHON_EXECUTABLE} "${LUA_GENERATE_SCRIPT}"
            --mode source
            --template "${LUA_TEMPLATE}"
            --output "${LUA_OUTPUT}"
            --name "${FILE_NAME}"
            "${LUA_FILE}"
        DEPENDS "${LUA_FILE}" "${LUA_TEMPLATE}"
        COMMENT "Generating ${FILE_NAME}_lua_script.h from ${FILE_NAME}.lua"
    )

    add_custom_target(${FILE_NAME}_lua_header DEPENDS "${LUA_OUTPUT}")
    add_dependencies(app ${FILE_NAME}_lua_header)

    include_directories("${LUA_OUTPUT_DIR}")
endfunction()


# luaz_add_thread(FILE_NAME_PATH)
#
# Generate a complete Lua thread (Zephyr thread + embedded script).
#
# Runs luaz_gen.py in source mode to produce <name>_lua_thread.c
# containing:
#   - A dedicated sys_heap and stack
#   - A weak _lua_setup hook for pre-script initialization
#   - A K_THREAD_DEFINE that runs the embedded Lua script
#
# The generated .c file is automatically added to the `app` target.
# The .lua source file is tracked as a dependency for incremental builds.
#
# Arguments:
#   FILE_NAME_PATH - Path to the .lua file (relative to project source dir).
function(luaz_add_thread FILE_NAME_PATH)
    cmake_path(GET FILE_NAME_PATH FILENAME FILE_NAME)
    cmake_path(REMOVE_EXTENSION FILE_NAME OUTPUT_VARIABLE FILE_NAME)

    set(LUA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${FILE_NAME_PATH}")
    set(LUA_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/lua")
    set(LUA_OUTPUT "${LUA_OUTPUT_DIR}/${FILE_NAME}_lua_thread.c")
    set(LUA_TEMPLATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/lua_thread.c.in")

    add_custom_command(
        OUTPUT "${LUA_OUTPUT}"
        COMMAND ${PYTHON_EXECUTABLE} "${LUA_GENERATE_SCRIPT}"
            --mode source
            --template "${LUA_TEMPLATE}"
            --output "${LUA_OUTPUT}"
            --name "${FILE_NAME}"
            "${LUA_FILE}"
        DEPENDS "${LUA_FILE}" "${LUA_TEMPLATE}"
        COMMENT "Generating ${FILE_NAME}_lua_thread.c from ${FILE_NAME}.lua"
    )

    include_directories("${LUA_OUTPUT_DIR}")

    target_sources(app PRIVATE "${LUA_OUTPUT}")
endfunction()


# luaz_add_bytecode_file(FILE_NAME_PATH)
#
# Embed a pre-compiled Lua bytecode as a C uint8_t array header.
#
# Runs luaz_gen.py in bytecode mode (which invokes luac -s) to produce
# <name>_lua_bytecode.h.  The generated header is placed under
# ${CMAKE_CURRENT_BINARY_DIR}/lua and made available via include_directories.
#
# The .lua source file is tracked as a dependency for incremental builds.
#
# Requires CONFIG_LUA_PRECOMPILE=y.
#
# Arguments:
#   FILE_NAME_PATH - Path to the .lua file (relative to project source dir).
function(luaz_add_bytecode_file FILE_NAME_PATH)
    if(NOT CONFIG_LUA_PRECOMPILE)
        message(FATAL_ERROR
            "luaz_add_bytecode_file(${FILE_NAME_PATH}) requires CONFIG_LUA_PRECOMPILE=y. "
            "Enable it in your prj.conf to use bytecode pre-compilation.")
    endif()

    cmake_path(GET FILE_NAME_PATH FILENAME FILE_NAME)
    cmake_path(REMOVE_EXTENSION FILE_NAME OUTPUT_VARIABLE FILE_NAME)

    set(LUA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${FILE_NAME_PATH}")
    set(LUA_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/lua")
    set(LUA_OUTPUT "${LUA_OUTPUT_DIR}/${FILE_NAME}_lua_bytecode.h")
    set(LUA_TEMPLATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/lua_bytecode_template.h.in")

    add_custom_command(
        OUTPUT "${LUA_OUTPUT}"
        COMMAND ${PYTHON_EXECUTABLE} "${LUA_GENERATE_SCRIPT}"
            --mode bytecode
            --template "${LUA_TEMPLATE}"
            --output "${LUA_OUTPUT}"
            --name "${FILE_NAME}"
            --luac "${LUAC_HOST}"
            "${LUA_FILE}"
        DEPENDS "${LUA_FILE}" "${LUA_TEMPLATE}"
        COMMENT "Generating ${FILE_NAME}_lua_bytecode.h from ${FILE_NAME}.lua"
    )

    add_custom_target(${FILE_NAME}_lua_bytecode_header DEPENDS "${LUA_OUTPUT}")
    add_dependencies(app ${FILE_NAME}_lua_bytecode_header)

    include_directories("${LUA_OUTPUT_DIR}")
endfunction()


# luaz_add_bytecode_thread(FILE_NAME_PATH)
#
# Generate a complete Lua thread loading pre-compiled bytecode.
#
# Runs luaz_gen.py in bytecode mode to produce
# <name>_lua_bytecode_thread.c containing:
#   - A dedicated sys_heap and stack
#   - A weak _lua_setup hook for pre-script initialization
#   - A K_THREAD_DEFINE that loads and runs the bytecode via luaL_loadbuffer
#
# The generated .c file is automatically added to the `app` target.
# The .lua source file is tracked as a dependency for incremental builds.
#
# Requires CONFIG_LUA_PRECOMPILE=y.
#
# Arguments:
#   FILE_NAME_PATH - Path to the .lua file (relative to project source dir).
function(luaz_add_bytecode_thread FILE_NAME_PATH)
    if(NOT CONFIG_LUA_PRECOMPILE)
        message(FATAL_ERROR
            "luaz_add_bytecode_thread(${FILE_NAME_PATH}) requires CONFIG_LUA_PRECOMPILE=y. "
            "Enable it in your prj.conf to use bytecode pre-compilation.")
    endif()

    cmake_path(GET FILE_NAME_PATH FILENAME FILE_NAME)
    cmake_path(REMOVE_EXTENSION FILE_NAME OUTPUT_VARIABLE FILE_NAME)

    set(LUA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${FILE_NAME_PATH}")
    set(LUA_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/lua")
    set(LUA_OUTPUT "${LUA_OUTPUT_DIR}/${FILE_NAME}_lua_bytecode_thread.c")
    set(LUA_TEMPLATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/lua_bytecode_thread.c.in")

    add_custom_command(
        OUTPUT "${LUA_OUTPUT}"
        COMMAND ${PYTHON_EXECUTABLE} "${LUA_GENERATE_SCRIPT}"
            --mode bytecode
            --template "${LUA_TEMPLATE}"
            --output "${LUA_OUTPUT}"
            --name "${FILE_NAME}"
            --luac "${LUAC_HOST}"
            "${LUA_FILE}"
        DEPENDS "${LUA_FILE}" "${LUA_TEMPLATE}"
        COMMENT "Generating ${FILE_NAME}_lua_bytecode_thread.c from ${FILE_NAME}.lua"
    )

    include_directories("${LUA_OUTPUT_DIR}")

    target_sources(app PRIVATE "${LUA_OUTPUT}")
endfunction()


# luaz_add_fs_thread(SCRIPT_FS_PATH)
#
# Generate a Lua thread that loads its script from the filesystem at runtime.
#
# Configures lua_fs_thread.c.in with the given FS path, producing a Zephyr
# thread that calls lua_fs_dofile() instead of embedding the script.
# The generated .c file is automatically added to the `app` target.
#
# Requires CONFIG_LUA_FS=y.
#
# Arguments:
#   SCRIPT_FS_PATH - Filesystem path the thread will load at runtime
#                    (e.g. "/lfs/hello_fs.lua" or just "hello_fs.lua").
function(luaz_add_fs_thread SCRIPT_FS_PATH)
    # Extract filename and strip extension, consistent with source/bytecode threads
    cmake_path(GET SCRIPT_FS_PATH FILENAME FILE_NAME)
    cmake_path(REMOVE_EXTENSION FILE_NAME OUTPUT_VARIABLE FILE_NAME)

    set(LUA_FS_PATH "${SCRIPT_FS_PATH}")
    string(TOUPPER "${FILE_NAME}" FILE_NAME_UPPER)

    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/lua_fs_thread.c.in"
    "${CMAKE_CURRENT_BINARY_DIR}/lua/${FILE_NAME}_lua_fs_thread.c")

    include_directories("${CMAKE_BINARY_DIR}/lua")

    target_sources(app PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/lua/${FILE_NAME}_lua_fs_thread.c"
    )
endfunction()


# luaz_add_fs_file(SOURCE_PATH [FS_NAME])
#
# Register a Lua source file for embedding and later writing to the FS.
#
# Embeds the file as a C string header (via luaz_add_file) so that the
# application can write it to the filesystem at boot using lua_fs_write_file().
#
# Arguments:
#   SOURCE_PATH - Path to the .lua file (relative to project source dir).
#   FS_NAME     - Optional: basename on the filesystem (defaults to source basename).
function(luaz_add_fs_file SOURCE_PATH)
    luaz_add_file("${SOURCE_PATH}")
endfunction()


# luaz_add_async_thread(NAME)
#
# Generate the thread defined by luaz_define_async_thread(NAME ...).
# Embedded files get thread-prefixed C symbols (<NAME>_<file>_lua_script or
# _lua_bytecode), so the same file name can be used by several threads.
function(luaz_add_async_thread _name)
    set(NAME "${_name}")
    string(TOUPPER "${_name}" NAME_UPPER)
    set(LUAZ_MODE "${LUAZ_ASYNC_THREAD_${_name}_MODE}")
    set(_files ${LUAZ_ASYNC_THREAD_${_name}_FILES})
    list(LENGTH _files LUAZ_COUNT)

    if(LUAZ_MODE STREQUAL "BYTECODE" AND NOT CONFIG_LUA_PRECOMPILE)
        message(FATAL_ERROR
            "luaz_define_async_thread(${_name} BYTECODE ...) requires CONFIG_LUA_PRECOMPILE=y.")
    endif()
    if(LUAZ_MODE STREQUAL "FS" AND NOT CONFIG_LUA_FS)
        message(FATAL_ERROR
            "luaz_define_async_thread(${_name} FS ...) requires CONFIG_LUA_FS=y.")
    endif()

    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/lua")
    set(LUAZ_INCLUDES "")
    set(LUAZ_LOADS "")
    set(_headers "")

    foreach(_file ${_files})
        cmake_path(ABSOLUTE_PATH _file BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _src)
        cmake_path(GET _file FILENAME _base)
        cmake_path(REMOVE_EXTENSION _base OUTPUT_VARIABLE _stem)
        set(_sym "${_name}_${_stem}")

        if(LUAZ_MODE STREQUAL "FS")
            string(APPEND LUAZ_LOADS "\tn += load(L, \"${_file}\");\n")
            continue()
        endif()

        if(LUAZ_MODE STREQUAL "SOURCE")
            set(_header "${_out_dir}/${_sym}_lua_script.h")
            set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/lua_template.h.in")
            set(_gen_mode source)
            set(_buf "${_sym}_lua_script, sizeof(${_sym}_lua_script) - 1")
        else()
            set(_header "${_out_dir}/${_sym}_lua_bytecode.h")
            set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/lua_bytecode_template.h.in")
            set(_gen_mode bytecode --luac "${LUAC_HOST}")
            set(_buf "(const char *)${_sym}_lua_bytecode, ${_sym}_lua_bytecode_len")
        endif()

        add_custom_command(
            OUTPUT "${_header}"
            COMMAND ${PYTHON_EXECUTABLE} "${LUA_GENERATE_SCRIPT}"
                --mode ${_gen_mode}
                --template "${_template}"
                --output "${_header}"
                --name "${_sym}"
                "${_src}"
            DEPENDS "${_src}" "${_template}"
            COMMENT "Embedding ${_file} in async thread ${_name}"
        )
        list(APPEND _headers "${_header}")
        cmake_path(GET _header FILENAME _header_name)
        string(APPEND LUAZ_INCLUDES "#include \"${_header_name}\"\n")
        string(APPEND LUAZ_LOADS "\tn += load(L, ${_buf}, \"@${_base}\");\n")
    endforeach()

    set(_out "${_out_dir}/${_name}_lua_async_thread.c")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/lua_async_thread.c.in"
        "${_out}" @ONLY)

    include_directories("${_out_dir}")
    target_sources(app PRIVATE "${_out}" ${_headers})
endfunction()

# luaz_generate_threads()
#
# Generate Lua threads from the LUAZ_SOURCE_THREADS, LUAZ_BYTECODE_THREADS,
# LUAZ_FS_THREADS and LUAZ_ASYNC_THREADS list variables. Each entry is processed by the
# corresponding luaz_add_*_thread() function.
function(luaz_generate_threads)
    foreach(_path ${LUAZ_SOURCE_THREADS})
        luaz_add_thread("${_path}")
    endforeach()
    foreach(_path ${LUAZ_BYTECODE_THREADS})
        luaz_add_bytecode_thread("${_path}")
    endforeach()
    foreach(_path ${LUAZ_FS_THREADS})
        luaz_add_fs_thread("${_path}")
    endforeach()
    foreach(_name ${LUAZ_ASYNC_THREADS})
        luaz_add_async_thread("${_name}")
    endforeach()
endfunction()

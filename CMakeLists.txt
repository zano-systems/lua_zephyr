if(CONFIG_LUA)
    set(LUA_DIR "${ZEPHYR_CURRENT_MODULE_DIR}/")
    set(LUA_CORE_DIR "${LUA_DIR}/lua/")
    set(INC_DIR "${LUA_DIR}/include/")
    set(SRC_DIR "${LUA_DIR}/src/")
    set(PARENT_SRC_DIR "${LUA_DIR}/../zephyr")

    zephyr_include_directories("${PARENT_SRC_DIR}/include/"
                             "${INC_DIR}/"
                             "${LUA_CORE_DIR}/"
    )

    zephyr_compile_definitions(LUA_32BITS)

    zephyr_library()

    # Lua core sources from the git submodule
    file(GLOB SRC "${LUA_CORE_DIR}/l*.c")
    # Exclude standalone interpreter and internal test harness from the build
    list(REMOVE_ITEM SRC "${LUA_CORE_DIR}/lua.c" "${LUA_CORE_DIR}/ltests.c")

    if(CONFIG_LUA_PRECOMPILE_ONLY)
        list(REMOVE_ITEM SRC
            "${LUA_CORE_DIR}/lparser.c"
            "${LUA_CORE_DIR}/llex.c"
            "${LUA_CORE_DIR}/lcode.c"
        )
    endif()

    # Reduce Lua's internal heap usage for constrained environments:
    #   MINSTRTABSIZE=64  — shrink the initial string table (default 128)
    #   STRCACHE_N=11     — reduce string cache rows (default 53)
    #   STRCACHE_M=1      — single entry per cache row (default 2)
    if(CONFIG_LUA_EXTRA_OPTIMIZATIONS)
        zephyr_compile_definitions(
            MINSTRTABSIZE=64
            STRCACHE_N=11
            STRCACHE_M=1
        )
    endif()

    zephyr_library_sources("${SRC}"
                           "${SRC_DIR}/luaz_utils.c"
                           "${SRC_DIR}/luaz_zbus.c"
                           "${SRC_DIR}/luaz_msg_descr.c"
                           "${SRC_DIR}/luaz_repl.c"
    )

    zephyr_library_sources_ifdef(CONFIG_LUA_PRECOMPILE_ONLY
        "${SRC_DIR}/luaz_parser_stubs.c")
    zephyr_library_sources_ifdef(CONFIG_LUA_FS "${SRC_DIR}/luaz_fs.c")
    zephyr_library_sources_ifdef(CONFIG_LUA_FS_SHELL
        "${SRC_DIR}/luaz_fs_shell.c")

    # Add the helper functions to be used inside the user CMakeLists.txt.
    include("${LUA_DIR}/luaz.cmake")

    zephyr_library_sources_ifdef(CONFIG_LUA_ASYNC "${SRC_DIR}/luaz_async.c")
endif()

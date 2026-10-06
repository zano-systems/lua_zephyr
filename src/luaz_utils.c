/**
 * @file luaz_utils.c
 * @brief Lua-Zephyr core: allocator, kernel API wrappers, and POSIX stubs.
 *
 * Implements the sys_heap-backed Lua allocator and the `zephyr` Lua library
 * which exposes msleep, printk, and LOG_* to Lua scripts.
 */

#include "lua.h"
#include "luaz_utils.h"

#include <lauxlib.h>
#include <lualib.h>
#ifdef CONFIG_LUA_LIB_ZBUS
#include <luaz_zbus.h>
#endif
#include <sys/times.h>
#include <sys/types.h>
#include <time.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_LUA_FS
#include <luaz_fs.h>
#endif

LOG_MODULE_REGISTER(lua_zephyr, CONFIG_LUA_ZEPHYR_LOG_LEVEL);

/**
 * @brief The custom allocator that Lua will use to dynamically allocate all
relevant data in the context
 *
 * @param ud User data, a reference to the custom heap region the allocator will
use
 * @param ptr A pointer to the object that's going to be allocated/rellocated
 * @param osize The old size of the memory region that ptr points to
 * @param nsize The desired new size of the memory region that ptr points to
 */
void *lua_zephyr_allocator(void *ud, void *ptr, size_t osize, size_t nsize)
{
	void *res = NULL;

	if (nsize == 0) {
		if (ptr != NULL) {
			sys_heap_free(ud, ptr);
		}

	} else {
		res = sys_heap_realloc(ud, ptr, nsize);
		if (res == NULL) {
			printk("Something went wrong with allocation\n");
		}
	}

	return res;
}

/** @brief Print thread memory usage report (heap and stack) as a table. */
void luaz_print_mem_usage(struct sys_heap *hp, size_t heap_size)
{
	ARG_UNUSED(hp);
	ARG_UNUSED(heap_size);

	printk("-- Lua thread memory report:\n");
	printk("       %6s  %8s  %8s  %5s\n", "size", "max used", "unused", "usage");

#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
	{
		struct sys_memory_stats stats;

		sys_heap_runtime_stats_get(hp, &stats);
		printk("heap:  %6zu  %8zu  %8zu  %4u%%\n", heap_size, stats.max_allocated_bytes,
		       stats.free_bytes,
		       (unsigned int)(stats.max_allocated_bytes * 100U / heap_size));
	}
#endif

#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
	{
		size_t unused;
		const struct k_thread *t = k_current_get();

		if (k_thread_stack_space_get(t, &unused) == 0) {
			size_t size = t->stack_info.size;
			size_t used = size - unused;

			printk("stack: %6zu  %8zu  %8zu  %4u%%\n", size, used, unused,
			       (unsigned int)(used * 100U / size));
		}
	}
#endif
}

/** @brief Lua binding for k_msleep. Expects one integer argument (ms). */
static int k_msleep_wrapper(lua_State *L)
{
	int n = lua_gettop(L);
	if (n != 1) {
		return luaL_error(L, "expected 1 arguments, got %d", n);
	}

	int ms = luaL_checkinteger(L, 1);

	k_msleep(ms);

	return 0;
}

/** @brief Lua binding for printk. Expects one string argument. */
static int printk_wrapper(lua_State *L)
{
	int n = lua_gettop(L);
	if (n != 1) {
		return luaL_error(L, "expected 1 arguments, got %d", n);
	}

	const char *message = luaL_checkstring(L, 1);

	printk("%s\n", message);

	return 0;
}

/** @brief Lua binding for LOG_INF. Expects one string argument. */
static int log_inf_wrapper(lua_State *L)
{
	int n = lua_gettop(L);
	if (n != 1) {
		return luaL_error(L, "expected 1 arguments, got %d", n);
	}

	const char *message = luaL_checkstring(L, 1);

	LOG_INF("[%s]: %s", k_thread_name_get(k_current_get()), message);

	return 0;
}

/** @brief Lua binding for LOG_WRN. Expects one string argument. */
static int log_wrn_wrapper(lua_State *L)
{
	int n = lua_gettop(L);
	if (n != 1) {
		return luaL_error(L, "expected 1 arguments, got %d", n);
	}

	const char *message = luaL_checkstring(L, 1);

	LOG_WRN("[%s]: %s", k_thread_name_get(k_current_get()), message);

	return 0;
}

/** @brief Lua binding for LOG_DBG. Expects one string argument. */
static int log_dbg_wrapper(lua_State *L)
{
	int n = lua_gettop(L);
	if (n != 1) {
		return luaL_error(L, "expected 1 arguments, got %d", n);
	}

	const char *message = luaL_checkstring(L, 1);

	LOG_DBG("[%s]: %s", k_thread_name_get(k_current_get()), message);

	return 0;
}

/** @brief Lua binding for LOG_ERR. Expects one string argument. */
static int log_err_wrapper(lua_State *L)
{
	int n = lua_gettop(L);
	if (n != 1) {
		return luaL_error(L, "expected 1 arguments, got %d", n);
	}

	const char *message = luaL_checkstring(L, 1);

	LOG_ERR("[%s]: %s", k_thread_name_get(k_current_get()), message);

	return 0;
}

static const luaL_Reg zephyr_wrappers[] = {
	{"msleep", k_msleep_wrapper},
	{"printk", printk_wrapper},
	{"log_inf", log_inf_wrapper},
	{"log_wrn", log_wrn_wrapper},
	{"log_dbg", log_dbg_wrapper},
	{"log_err", log_err_wrapper},
	{NULL, NULL} /* Sentinel value to mark the end of the array */
};

/** @brief Open the `zephyr` Lua library. Registers kernel wrappers and nests zbus/fs. */
int luaopen_zephyr(lua_State *L)
{
	luaL_newlib(L, zephyr_wrappers);

#ifdef CONFIG_LUA_LIB_ZBUS
	/* Nest zbus as zephyr.zbus */
	luaopen_zbus(L);
	lua_setfield(L, -2, "zbus");
#endif

#ifdef CONFIG_LUA_FS
	/* Nest fs as zephyr.fs */
	luaopen_fs(L);
	lua_setfield(L, -2, "fs");
#endif

	return 1;
}

/**
 * @brief Minimal require() for preload-only environments.
 *
 * Checks registry._LOADED[name] first (cached), then falls back to
 * registry._PRELOAD[name].  No file/C-library searchers — eliminates
 * ~1 KB heap overhead of luaopen_package() per Lua state.
 */
static int luaz_require(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);

	lua_settop(L, 1);

	/* idx 2: _LOADED table */
	luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
	lua_getfield(L, 2, name);
	if (lua_toboolean(L, -1)) {
		return 1;
	}
	lua_pop(L, 1);

	/* idx 3: _PRELOAD table */
	luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
	if (lua_getfield(L, 3, name) == LUA_TNIL) {
		return luaL_error(L,
				  "module '%s' not found:\n\t"
				  "no field package.preload['%s']",
				  name, name);
	}

	/* call loader(name, ":preload:") */
	lua_pushvalue(L, 1);
	lua_pushliteral(L, ":preload:");
	lua_call(L, 2, 1);

	if (!lua_isnil(L, -1)) {
		lua_setfield(L, 2, name); /* _LOADED[name] = result */
	} else {
		lua_pop(L, 1);
		lua_pushboolean(L, 1);
		lua_setfield(L, 2, name); /* _LOADED[name] = true */
	}

	lua_getfield(L, 2, name);
	lua_pushliteral(L, ":preload:");
	return 2;
}

/** @brief Register minimal require() and preload zephyr + standard Lua libs. */
void luaz_openlibs(lua_State *L)
{
	lua_pushcfunction(L, luaz_require);
	lua_setglobal(L, "require");

#ifdef CONFIG_LUA_LIB_BASE
	luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
	lua_pop(L, 1);
#endif

	/* Preload zephyr (includes zbus, fs) and standard Lua libs */
	luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);

	lua_pushcfunction(L, luaopen_zephyr);
	lua_setfield(L, -2, "zephyr");

#ifdef CONFIG_LUA_LIB_STRING
	lua_pushcfunction(L, luaopen_string);
	lua_setfield(L, -2, "string");
#endif
#ifdef CONFIG_LUA_LIB_TABLE
	lua_pushcfunction(L, luaopen_table);
	lua_setfield(L, -2, "table");
#endif
#ifdef CONFIG_LUA_LIB_MATH
	lua_pushcfunction(L, luaopen_math);
	lua_setfield(L, -2, "math");
#endif
#ifdef CONFIG_LUA_LIB_COROUTINE
	lua_pushcfunction(L, luaopen_coroutine);
	lua_setfield(L, -2, "coroutine");
#endif
#ifdef CONFIG_LUA_LIB_UTF8
	lua_pushcfunction(L, luaopen_utf8);
	lua_setfield(L, -2, "utf8");
#endif
#ifdef CONFIG_LUA_LIB_DEBUG
	lua_pushcfunction(L, luaopen_debug);
	lua_setfield(L, -2, "debug");
#endif

	lua_pop(L, 1); /* pop preload table */
}

/** @brief POSIX stub — _times is unused but required by the toolchain. */
clock_t _times(struct tms *tms)
{
	errno = ENOSYS;
	return (clock_t)-1;
}

/** @brief POSIX stub — _unlink is unused but required by the toolchain. */
int _unlink(const char *pathname)
{
	return -1;
}

/** @brief POSIX stub — _link is unused but required by the toolchain. */
int _link(const char *oldpath, const char *newpath)
{
	errno = ENOSYS;
	return -1;
}

/*
 * The base library's loadfile reaches picolibc's fopen, which needs these to
 * link.
 */

#if defined(CONFIG_PICOLIBC) && !defined(CONFIG_POSIX_DEVICE_IO)
__weak int open(const char *path, int flags, ...)
{
	ARG_UNUSED(path);
	ARG_UNUSED(flags);
	errno = ENOSYS;
	return -1;
}

__weak int close(int fd)
{
	ARG_UNUSED(fd);
	errno = ENOSYS;
	return -1;
}

__weak ssize_t read(int fd, void *buf, size_t count)
{
	ARG_UNUSED(fd);
	ARG_UNUSED(buf);
	ARG_UNUSED(count);
	errno = ENOSYS;
	return -1;
}

__weak ssize_t write(int fd, const void *buf, size_t count)
{
	ARG_UNUSED(fd);
	ARG_UNUSED(buf);
	ARG_UNUSED(count);
	errno = ENOSYS;
	return -1;
}
#endif

#if defined(CONFIG_PICOLIBC) && !defined(CONFIG_POSIX_FD_MGMT)
__weak off_t lseek(int fd, off_t offset, int whence)
{
	ARG_UNUSED(fd);
	ARG_UNUSED(offset);
	ARG_UNUSED(whence);
	errno = ENOSYS;
	return -1;
}
#endif

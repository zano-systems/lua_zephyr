/**
 * @file luaz_zbus.c
 * @brief Lua bindings for Zephyr zbus: publish, read, wait, and serialization.
 *
 * Implements channel and observer userdata types with metatables so Lua scripts
 * can call :pub(), :read(), and :wait_msg().  Conversion between C structs and
 * Lua tables is handled via the descriptor system in lua_msg_descr.
 */

#include <lauxlib.h>
#include <lualib.h>
#include <string.h>
#include <sys/times.h>
#include <zephyr/zbus/zbus.h>
#include <luaz_utils.h>
#include <luaz_zbus.h>
#include <luaz_msg_descr.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#ifdef CONFIG_LUA_ASYNC
#include <luaz_async.h>
#endif

/** @brief Metatable name for zbus observer userdata. */
#define ZBUS_OBS_METATABLE "zbus.observer.mt"

/** @brief Largest message size across all registered zbus channels (computed at boot). */
static size_t max_chan_msg_size;

/**
 * @brief Allocate raw memory from the Lua state's heap allocator.
 *
 * @param L     Lua state whose allocator is used.
 * @param size  Number of bytes to allocate.
 * @return Pointer to allocated block, or NULL on failure.
 */
static void *lua_alloc_raw(lua_State *L, size_t size)
{
	void *ud;
	lua_Alloc allocf = lua_getallocf(L, &ud);

	return allocf(ud, NULL, 0, size);
}

/**
 * @brief Free raw memory previously obtained from lua_alloc_raw().
 *
 * @param L     Lua state whose allocator is used.
 * @param ptr   Pointer to the block to free.
 * @param size  Original allocation size (must match the alloc call).
 */
static void lua_free_raw(lua_State *L, void *ptr, size_t size)
{
	void *ud;
	lua_Alloc allocf = lua_getallocf(L, &ud);

	allocf(ud, ptr, size, 0);
}

static bool find_max_msg_size(const struct zbus_channel *chan)
{
	size_t msg_size = zbus_chan_msg_size(chan);

	if (msg_size > max_chan_msg_size) {
		max_chan_msg_size = msg_size;
	}
	return true;
}

static int lua_zbus_init(void)
{
	zbus_iterate_over_channels(find_max_msg_size);
	return 0;
}

SYS_INIT(lua_zbus_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

size_t luaz_zbus_max_msg_size(void)
{
	return max_chan_msg_size;
}

/** @brief Validate and return the zbus channel userdata at stack index @p idx. */
static const struct zbus_channel **check_zbus_channel(lua_State *L, int idx)
{
	void *ud = luaL_checkudata(L, idx, ZBUS_CHAN_METATABLE);
	luaL_argcheck(L, ud != NULL, idx, "`zbus channel' expected");
	return ud;
}

/**
 * @brief Convert a C message struct to a Lua table.
 *
 * Uses the descriptor stored in zbus_chan_user_data if available.
 *
 * @param L        Lua state.
 * @param chan     The zbus channel the message belongs to.
 * @param message  Pointer to the raw message buffer.
 * @return 1 (table or nil pushed onto the Lua stack).
 */
static int msg_struct_to_lua_table(lua_State *L, const struct zbus_channel *chan, void *message)
{
	const struct lua_msg_descr *descr = zbus_chan_user_data(chan);

	if (descr != NULL) {
		lua_msg_descr_to_table(L, descr->fields, descr->field_count, message);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

/**
 * @brief Convert a Lua table to a C message struct.
 *
 * Uses the descriptor stored in zbus_chan_user_data if available.
 *
 * @param L        Lua state.
 * @param chan     The zbus channel the message belongs to.
 * @param message  Pointer to the output message buffer.
 * @return Message size on success, 0 if no descriptor is available.
 */
static size_t lua_table_to_msg_struct(lua_State *L, const struct zbus_channel *chan, void *message)
{
	const struct lua_msg_descr *descr = zbus_chan_user_data(chan);

	if (descr != NULL) {
		lua_msg_descr_from_table(L, descr->fields, descr->field_count, message, 2);
		return descr->msg_size;
	}
	return 0;
}

/** @brief Lua method: channel:pub(table, timeout_ms) -> err. */
static int chan_pub(lua_State *L)
{
	int err = -EINVAL;

	int n = lua_gettop(L);
	if (n != 3) {
		return luaL_error(L, "expected 3 arguments, got %d", n);
	}

	const struct zbus_channel **chan = check_zbus_channel(L, 1);

	luaL_checktype(L, 2, LUA_TTABLE);

	k_timeout_t timeout = luaz_check_timeout(L, 3);

#ifdef CONFIG_LUA_ASYNC
	if (!K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		luaz_async_warn_blocking(L, "channel:pub");
	}
#endif

	size_t msg_size = zbus_chan_msg_size(*chan);
	void *msg = lua_alloc_raw(L, msg_size);

	if (msg == NULL) {
		lua_pushinteger(L, -ENOMEM);
		return 1;
	}

	size_t s = lua_table_to_msg_struct(L, *chan, msg);

	if (s) {
		err = zbus_chan_pub(*chan, msg, timeout);
	}

	lua_free_raw(L, msg, msg_size);

	lua_pushinteger(L, err);

	return 1;
}

/** @brief Lua method: channel:read(timeout_ms) -> err, table. */
static int chan_read(lua_State *L)
{
	int err = -EINVAL;

	int n = lua_gettop(L);
	if (n != 2) {
		return luaL_error(L, "expected 2 arguments, got %d", n);
	}

	const struct zbus_channel **chan = check_zbus_channel(L, 1);
	k_timeout_t timeout = luaz_check_timeout(L, 2);

#ifdef CONFIG_LUA_ASYNC
	if (!K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		luaz_async_warn_blocking(L, "channel:read");
	}
#endif

	size_t msg_size = zbus_chan_msg_size(*chan);
	void *msg = lua_alloc_raw(L, msg_size);

	if (msg == NULL) {
		lua_pushinteger(L, -ENOMEM);
		lua_pushnil(L);
		return 2;
	}

	err = zbus_chan_read(*chan, msg, timeout);

	lua_pushinteger(L, err);

	msg_struct_to_lua_table(L, *chan, msg);

	lua_free_raw(L, msg, msg_size);

	return 2;
}
/** @brief Lua metamethod __eq: compare two channel userdata by pointer. */
static int chan_equals(lua_State *L)
{
	const struct zbus_channel **chan1 = check_zbus_channel(L, 1);
	if (chan1 == NULL) {
		return luaL_error(L, "invalid zbus channel");
	}
	const struct zbus_channel **chan2 = check_zbus_channel(L, 2);
	if (chan2 == NULL) {
		return luaL_error(L, "invalid zbus channel");
	}

	lua_pushboolean(L, *chan1 == *chan2);

	return 1;
}

/** @brief Lua metamethod __tostring: return a human-readable channel string. */
static int chan_tostring(lua_State *L)
{
	const struct zbus_channel **chan = check_zbus_channel(L, 1);

	if (chan == NULL) {
		return luaL_error(L, "invalid zbus channel");
	}

	lua_pushfstring(L, "zbus_channel { ref=%p } ", *chan);
	return 1;
}

static const struct luaL_Reg zbus_chan_metamethods[] = {{"pub", chan_pub},
							{"read", chan_read},
							{"__tostring", chan_tostring},
							{"__eq", chan_equals},
							{NULL, NULL}};

/** @brief Validate and return the zbus observer userdata at stack index @p idx. */
static const struct zbus_observer **check_zbus_observer(lua_State *L, int idx)
{
	void *ud = luaL_checkudata(L, idx, ZBUS_OBS_METATABLE);
	luaL_argcheck(L, ud != NULL, idx, "`zbus observer' expected");
	return ud;
}

/** @brief Lua method: observer:wait_msg(timeout_ms) -> err, channel, table. */
static int sub_wait_msg(lua_State *L)
{
	const struct zbus_channel *chan;

	const struct zbus_observer **obs = check_zbus_observer(L, 1);
	k_timeout_t timeout = luaz_check_timeout(L, 2);

#ifdef CONFIG_LUA_ASYNC
	luaz_async_warn_blocking(L, "observer:wait_msg");
#endif

	void *msg = lua_alloc_raw(L, max_chan_msg_size);

	if (msg == NULL) {
		lua_pushinteger(L, -ENOMEM);
		lua_pushnil(L);
		lua_pushnil(L);
		return 3;
	}

	int err = zbus_sub_wait_msg(*obs, &chan, msg, timeout);

	lua_pushinteger(L, err);

	if (err) {
		lua_pushnil(L);
		lua_pushnil(L);
	} else {
		const struct zbus_channel **lua_chan =
			lua_newuserdata(L, sizeof(const struct zbus_channel *));
		*lua_chan = chan;
		luaL_getmetatable(L, ZBUS_CHAN_METATABLE);
		lua_setmetatable(L, -2);

		msg_struct_to_lua_table(L, chan, msg);
	}

	lua_free_raw(L, msg, max_chan_msg_size);

	return 3;
}

static const struct luaL_Reg zbus_obs_metamethods[] = {{"wait_msg", sub_wait_msg}, {NULL, NULL}};

/** @brief Lua function: zbus.channel_declare(name) -> channel userdata. */
static int zbus_channel_declare(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	const struct zbus_channel *chan = zbus_chan_from_name(name);

	if (chan == NULL) {
		return luaL_error(L, "zbus channel '%s' not found", name);
	}

	const struct zbus_channel **ud = lua_newuserdata(L, sizeof(const struct zbus_channel *));
	*ud = chan;
	luaL_getmetatable(L, ZBUS_CHAN_METATABLE);
	lua_setmetatable(L, -2);

	return 1;
}

/** @brief Context for observer name lookup iteration. */
struct obs_find_ctx {
	const char *name;
	const struct zbus_observer *found;
};

/** @brief Iterator callback: match observer by name. */
static bool find_obs_by_name(const struct zbus_observer *obs, void *user_data)
{
	struct obs_find_ctx *ctx = user_data;

	if (strcmp(zbus_obs_name(obs), ctx->name) == 0) {
		ctx->found = obs;
		return false;
	}
	return true;
}

/** @brief Lua function: zbus.observer_declare(name) -> observer userdata. */
static int zbus_observer_declare(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	struct obs_find_ctx ctx = {.name = name, .found = NULL};

	zbus_iterate_over_observers_with_user_data(find_obs_by_name, &ctx);

	if (ctx.found == NULL) {
		return luaL_error(L, "zbus observer '%s' not found", name);
	}

	const struct zbus_observer **ud = lua_newuserdata(L, sizeof(const struct zbus_observer *));
	*ud = ctx.found;
	luaL_getmetatable(L, ZBUS_OBS_METATABLE);
	lua_setmetatable(L, -2);

	return 1;
}

static const luaL_Reg zbus[] = {
	{"channel_declare", zbus_channel_declare},
	{"observer_declare", zbus_observer_declare},
	{NULL, NULL},
};

/** @brief Open the `zbus` Lua library. Creates channel and observer metatables. */
int luaopen_zbus(lua_State *L)
{
	luaL_newmetatable(L, ZBUS_CHAN_METATABLE);
	/* Duplicate the metatable for setting __index */
	lua_pushvalue(L, -1);
	/* Set metatable.__index = metatable */
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, zbus_chan_metamethods, 0);
	lua_pop(L, 1);

	luaL_newmetatable(L, ZBUS_OBS_METATABLE);
	/* Duplicate the metatable for setting __index */
	lua_pushvalue(L, -1);
	/* Set metatable.__index = metatable */
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, zbus_obs_metamethods, 0);
	lua_pop(L, 1);

	luaL_newlib(L, zbus);

	return 1;
}

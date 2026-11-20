/**
 * @file testlib.c
 * @brief Test channels and the `testlib` Lua module for the async suite.
 */

#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include <luaz_async.h>
#include <luaz_msg_descr.h>
#include <luaz_utils.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/zbus/zbus.h>

#include "test_api_lua_script.h"
#include "test_cancel_lua_script.h"
#include "test_errors_lua_script.h"
#include "test_hybrid_lua_script.h"
#include "test_join_lua_script.h"
#include "test_readers_lua_script.h"
#include "test_ring_lua_script.h"
#include "test_sched_lua_script.h"

struct t_msg {
	char v[16];
	int32_t i;
};

static const struct lua_msg_field_descr t_fields[] = {
	LUA_MSG_FIELD(struct t_msg, v, LUA_MSG_TYPE_STRING_BUF),
	LUA_MSG_FIELD(struct t_msg, i, LUA_MSG_TYPE_INT),
};

#define T_CHAN(_name)                                                                              \
	ZBUS_CHAN_DEFINE(_name, struct t_msg, NULL, LUA_ZBUS_MSG_DESCR(struct t_msg, t_fields),    \
			 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0))

T_CHAN(t_a);
T_CHAN(t_b);
T_CHAN(t_c);
T_CHAN(t_flood);

/* No descriptor: chan:await must refuse it. */
ZBUS_CHAN_DEFINE(t_raw, struct t_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

LUAZ_ASYNC_DEFINE(suite_async, CONFIG_LUA_ASYNC_MAX_READERS);

/* printk capture, so tests can check what the scheduler reports. */
static printk_hook_fn_t console_hook;
static char captured[4096];
static size_t captured_len;

static int capture_char(int c)
{
	if (captured_len < sizeof(captured)) {
		captured[captured_len++] = (char)c;
	}

	return console_hook != NULL ? console_hook(c) : c;
}

/* testlib.run(fn, ...) -> true when the scheduler returned 0 */
static int l_run(lua_State *L)
{
	lua_pushboolean(L, luaz_async_run(L, &suite_async, lua_gettop(L)) == 0);

	return 1;
}

/* testlib.readers() -> readers taken from the suite's slab */
static int l_readers(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)k_mem_slab_num_used_get(&suite_async_readers));

	return 1;
}

static int l_now(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)k_uptime_get());

	return 1;
}

/* Wait for the next millisecond boundary, so timestamps taken with
 * k_uptime_get() don't carry a fraction left over from earlier tests.
 */
static int l_align(lua_State *L)
{
	const int64_t per_ms = CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1000;
	int64_t rem = k_uptime_ticks() % per_ms;

	ARG_UNUSED(L);
	if (rem != 0) {
		k_busy_wait(k_ticks_to_us_ceil32(per_ms - rem));
	}

	return 0;
}

/* Spend simulated time without yielding to the scheduler. */
static int l_busy(lua_State *L)
{
	k_busy_wait((uint32_t)luaL_checkinteger(L, 1) * 1000U);

	return 0;
}

/* testlib.claim(name) / testlib.finish(name): hold a channel's lock to make it busy. */
static const struct zbus_channel *check_chan(lua_State *L)
{
	const struct zbus_channel *chan = zbus_chan_from_name(luaL_checkstring(L, 1));

	luaL_argcheck(L, chan != NULL, 1, "unknown channel");

	return chan;
}

static int l_claim(lua_State *L)
{
	lua_pushinteger(L, zbus_chan_claim(check_chan(L), K_NO_WAIT));

	return 1;
}

static int l_finish(lua_State *L)
{
	lua_pushinteger(L, zbus_chan_finish(check_chan(L)));

	return 1;
}

/* testlib.hold(name, ms): the system workqueue holds the lock for ms, then frees it. */
static const struct zbus_channel *held;

static void claim_work(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)zbus_chan_claim(held, K_FOREVER);
}

static void finish_work(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)zbus_chan_finish(held);
}

static K_WORK_DEFINE(claim_w, claim_work);
static K_WORK_DELAYABLE_DEFINE(finish_w, finish_work);

static int l_hold(lua_State *L)
{
	held = check_chan(L);
	(void)k_work_submit(&claim_w);
	(void)k_work_schedule(&finish_w, K_MSEC(luaL_checkinteger(L, 2)));

	return 0;
}

/*
 * testlib.publish_later(name, ms): the system workqueue publishes {v = "later"}
 * on the channel after ms, waking a scheduler whose tasks are all parked.
 */
static const struct zbus_channel *later_chan;

static void publish_later_work(struct k_work *work)
{
	struct t_msg msg = {.v = "later", .i = 0};

	ARG_UNUSED(work);
	(void)zbus_chan_pub(later_chan, &msg, K_MSEC(100));
}

static K_WORK_DELAYABLE_DEFINE(later_w, publish_later_work);

static int l_publish_later(lua_State *L)
{
	later_chan = check_chan(L);
	(void)k_work_schedule(&later_w, K_MSEC(luaL_checkinteger(L, 2)));

	return 0;
}

/* testlib.watching(name) -> whether the suite's subscriber is on the channel */
static int l_watching(lua_State *L)
{
	const struct zbus_channel *chan = check_chan(L);
	struct zbus_observer_node *nd;
	bool found = false;

	SYS_SLIST_FOR_EACH_CONTAINER(&chan->data->observers, nd, node) {
		found = found || nd->obs == &suite_async_sub;
	}
	lua_pushboolean(L, found);

	return 1;
}

/* testlib.queued() -> whether a message waits in the suite's subscriber */
static int l_queued(lua_State *L)
{
	lua_pushboolean(L, !k_fifo_is_empty(suite_async_sub.message_fifo));

	return 1;
}

#ifdef CONFIG_OBJ_CORE_TIMER
/* Stops a walk that would not end (a cycle from a re-linked timer). */
#define WALK_BOUND 1000

struct timer_count {
	int seen;
	int visits;
};

static int count_timer(struct k_obj_core *obj_core, void *data)
{
	struct timer_count *tc = data;

	if (obj_core == K_OBJ_CORE(&suite_async_timer)) {
		tc->seen++;
	}
	tc->visits++;

	return tc->visits < WALK_BOUND ? 0 : -1;
}
#endif

/* testlib.timer_links() -> times the suite's idle timer is registered, -1 for
 * an unbounded walk, or nil without CONFIG_OBJ_CORE_TIMER
 */
static int l_timer_links(lua_State *L)
{
#ifdef CONFIG_OBJ_CORE_TIMER
	struct k_obj_type *type = k_obj_type_find(K_OBJ_TYPE_TIMER_ID);
	struct timer_count tc = {0};

	if (type == NULL) {
		return luaL_error(L, "no timer object type");
	}
	(void)k_obj_type_walk_unlocked(type, count_timer, &tc);
	lua_pushinteger(L, tc.visits >= WALK_BOUND ? -1 : tc.seen);
#else
	lua_pushnil(L);
#endif

	return 1;
}

/*
 * testlib.alloc_check() -> "ok" or what failed. Drives the state's allocator
 * directly, so no Lua allocation interleaves with the counters it checks.
 */
static int l_alloc_check(lua_State *L)
{
#ifdef CONFIG_LUA_ALLOC_STATS
	void *ud;
	lua_Alloc f = lua_getallocf(L, &ud);
	struct luaz_alloc_stats a;
	struct luaz_alloc_stats b;
	void *p;

	luaz_alloc_stats_reset();
	luaz_alloc_stats_get(&a);
	if (a.live == 0U || a.peak != a.live || a.allocs != 0U || a.bytes != 0U) {
		lua_pushliteral(L, "reset must keep live bytes and set peak to them");
		return 1;
	}
	if (f(ud, NULL, LUA_TSTRING, SIZE_MAX / 2U) != NULL) {
		lua_pushliteral(L, "huge allocation succeeded");
		return 1;
	}
	luaz_alloc_stats_get(&b);
	if (b.fails != a.fails + 1U || b.allocs != a.allocs || b.bytes != a.bytes ||
	    b.live != a.live || b.peak != a.peak) {
		lua_pushliteral(L, "a failed allocation must only count as a failure");
		return 1;
	}
	p = f(ud, NULL, LUA_TSTRING, 64U);
	luaz_alloc_stats_get(&b);
	if (p == NULL || b.allocs != 1U || b.bytes != 64U || b.live != a.live + 64U ||
	    b.peak != a.live + 64U) {
		lua_pushliteral(L, "an allocation must count bytes, live and peak");
		return 1;
	}
	(void)f(ud, p, 64U, 0U);
	luaz_alloc_stats_get(&b);
	if (b.frees != 1U || b.live != a.live || b.peak != a.live + 64U) {
		lua_pushliteral(L, "a free must lower live bytes and keep the peak");
		return 1;
	}
	lua_pushliteral(L, "ok");
#else
	lua_pushnil(L);
#endif

	return 1;
}

/* Returns what printk wrote since the last call, and clears it. */
static int l_captured(lua_State *L)
{
	lua_pushlstring(L, captured, captured_len);
	captured_len = 0;

	return 1;
}

static int l_config(lua_State *L)
{
	lua_createtable(L, 0, 7);
	lua_pushinteger(L, CONFIG_LUA_ASYNC_MAX_TASKS);
	lua_setfield(L, -2, "max_tasks");
	lua_pushinteger(L, CONFIG_LUA_ASYNC_MAX_CHANNELS);
	lua_setfield(L, -2, "max_chans");
	lua_pushinteger(L, CONFIG_LUA_ASYNC_CHAN_QUEUE_DEPTH);
	lua_setfield(L, -2, "depth");
	lua_pushinteger(L, CONFIG_LUA_ASYNC_MAX_DRAIN);
	lua_setfield(L, -2, "max_drain");
	lua_pushinteger(L, LUAZ_ASYNC_READERS(CONFIG_LUA_ASYNC_MAX_READERS));
	lua_setfield(L, -2, "max_readers");
	lua_pushinteger(L, CONFIG_LUA_ASYNC_MAX_LOCK_WAITS);
	lua_setfield(L, -2, "max_locks");
	/* 0: observer nodes come from the heap */
	lua_pushinteger(L, COND_CODE_1(CONFIG_ZBUS_RUNTIME_OBSERVERS_NODE_ALLOC_STATIC,
				       (CONFIG_ZBUS_RUNTIME_OBSERVERS_NODE_POOL_SIZE), (0)));
	lua_setfield(L, -2, "node_pool");
	lua_pushinteger(L, IS_ENABLED(CONFIG_OBJ_CORE_TIMER) ? 1 : 0);
	lua_setfield(L, -2, "obj_core");
	lua_pushinteger(L, IS_ENABLED(CONFIG_LUA_ASYNC_STATS) && IS_ENABLED(CONFIG_LUA_ALLOC_STATS)
				   ? 1
				   : 0);
	lua_setfield(L, -2, "stats");

	return 1;
}

/* testlib.files() -> { {name, source}, ... } */
static int l_files(lua_State *L)
{
	static const struct {
		const char *name;
		const char *src;
	} files[] = {
		{"test_api", test_api_lua_script},       {"test_cancel", test_cancel_lua_script},
		{"test_errors", test_errors_lua_script}, {"test_hybrid", test_hybrid_lua_script},
		{"test_join", test_join_lua_script},     {"test_readers", test_readers_lua_script},
		{"test_ring", test_ring_lua_script},     {"test_sched", test_sched_lua_script},
	};

	lua_createtable(L, ARRAY_SIZE(files), 0);
	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		lua_createtable(L, 2, 0);
		lua_pushstring(L, files[i].name);
		lua_rawseti(L, -2, 1);
		lua_pushstring(L, files[i].src);
		lua_rawseti(L, -2, 2);
		lua_rawseti(L, -2, (lua_Integer)i + 1);
	}

	return 1;
}

static int luaopen_testlib(lua_State *L)
{
	static const luaL_Reg funcs[] = {
		{"run", l_run},
		{"now", l_now},
		{"busy", l_busy},
		{"align", l_align},
		{"captured", l_captured},
		{"config", l_config},
		{"files", l_files},
		{"claim", l_claim},
		{"finish", l_finish},
		{"hold", l_hold},
		{"readers", l_readers},
		{"watching", l_watching},
		{"queued", l_queued},
		{"timer_links", l_timer_links},
		{"publish_later", l_publish_later},
		{"alloc_check", l_alloc_check},
		{NULL, NULL},
	};

	luaL_newlib(L, funcs);

	return 1;
}

int suite_lua_setup(lua_State *L)
{
	console_hook = __printk_get_hook();
	__printk_hook_install(capture_char);

	luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
	lua_pushcfunction(L, luaopen_testlib);
	lua_setfield(L, -2, "testlib");
	lua_pop(L, 1);

	return 0;
}

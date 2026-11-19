/**
 * @file benchlib.c
 * @brief Channels, publisher thread and the `bench` Lua module for the
 *        zephyr.async benchmarks.
 *
 * Each bench.measure() call runs its functions under a fresh scheduler and
 * prints one machine-readable line:
 *
 *   BENCH <name> ops=<n> cyc=<c> cyc/op=<c> wall_us=<us> allocs/op=<a.aa>
 *         bytes/op=<b> peak=<bytes> gc=<mode> rc=<rc>
 *
 * cyc counts the Lua thread's own execution cycles (thread runtime stats), so
 * idle time and the publisher are left out. With CONFIG_LUA_ASYNC_STATS a
 * STATS line follows with the scheduler counters and the phase split.
 */

#include <inttypes.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include <luaz_async.h>
#include <luaz_msg_descr.h>
#include <luaz_utils.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef CONFIG_TIMING_FUNCTIONS
#include <zephyr/timing/timing.h>
#endif

struct bench_msg {
	int32_t seq;
	int32_t a;
	int32_t b;
	int32_t c;
};

static const struct lua_msg_field_descr bench_fields[] = {
	LUA_MSG_FIELD(struct bench_msg, seq, LUA_MSG_TYPE_INT),
	LUA_MSG_FIELD(struct bench_msg, a, LUA_MSG_TYPE_INT),
	LUA_MSG_FIELD(struct bench_msg, b, LUA_MSG_TYPE_INT),
	LUA_MSG_FIELD(struct bench_msg, c, LUA_MSG_TYPE_INT),
};

#define B_CHAN(_name)                                                                              \
	ZBUS_CHAN_DEFINE(_name, struct bench_msg, NULL,                                            \
			 LUA_ZBUS_MSG_DESCR(struct bench_msg, bench_fields), ZBUS_OBSERVERS_EMPTY, \
			 ZBUS_MSG_INIT(0))

B_CHAN(b_c0);
B_CHAN(b_c1);
B_CHAN(b_c2);
B_CHAN(b_c3);

LUAZ_ASYNC_DEFINE(bench_async, CONFIG_LUA_ASYNC_MAX_READERS);

/* ---- clocks ---- */

#ifdef CONFIG_TIMING_FUNCTIONS
static timing_t wall_t0;

static void wall_start(void)
{
	wall_t0 = timing_counter_get();
}

static uint64_t wall_us(void)
{
	timing_t t1 = timing_counter_get();

	return timing_cycles_to_ns(timing_cycles_get(&wall_t0, &t1)) / 1000U;
}

static uint32_t wall_hz(void)
{
	return (uint32_t)timing_freq_get();
}
#else
static uint32_t wall_t0;

static void wall_start(void)
{
	wall_t0 = k_cycle_get_32();
}

static uint64_t wall_us(void)
{
	return k_cyc_to_us_floor64((uint32_t)(k_cycle_get_32() - wall_t0));
}

static uint32_t wall_hz(void)
{
	return (uint32_t)sys_clock_hw_cycles_per_sec();
}
#endif

static uint64_t thread_cycles(void)
{
	k_thread_runtime_stats_t rt;

	(void)k_thread_runtime_stats_get(k_current_get(), &rt);

	return rt.execution_cycles;
}

/* ---- publisher: runs only while the Lua thread is idle (lower priority) ---- */

static const struct zbus_channel *const pub_all[] = {&b_c0, &b_c1, &b_c2, &b_c3};
static const struct zbus_channel *pub_chans[ARRAY_SIZE(pub_all)];
static int pub_nchans;
static int pub_count;
static K_SEM_DEFINE(pub_go, 0, 1);

static void publisher(void *p1, void *p2, void *p3)
{
	struct bench_msg msg = {.a = 1, .b = 2, .c = 3};

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		(void)k_sem_take(&pub_go, K_FOREVER);
		for (int i = 0; i < pub_count; i++) {
			msg.seq = i + 1;
			(void)zbus_chan_pub(pub_chans[i % pub_nchans], &msg, K_FOREVER);
		}
	}
}

K_THREAD_DEFINE(bench_pub, 1024, publisher, NULL, NULL, NULL, CONFIG_BENCH_LUA_THREAD_PRIORITY + 1,
		0, 0);

/* ---- Lua module ---- */

static int failures;

/* bench.run(fn, ...) -> true when the scheduler returned 0 (no measurement) */
static int l_run(lua_State *L)
{
	lua_pushboolean(L, luaz_async_run(L, &bench_async, lua_gettop(L)) == 0);

	return 1;
}

static const char *gc_name = "inc";

/* bench.gc("inc" | "gen"): set the collector mode reported by measure() */
static int l_gc(lua_State *L)
{
	static const char *const modes[] = {"inc", "gen", NULL};
	int m = luaL_checkoption(L, 1, NULL, modes);

	(void)lua_gc(L, m == 0 ? LUA_GCINC : LUA_GCGEN);
	gc_name = modes[m];

	return 0;
}

#ifdef CONFIG_LUA_ASYNC_STATS
static unsigned int pct(uint64_t part, uint64_t total)
{
	return total == 0U ? 0U : (unsigned int)((part * 1000U) / total);
}

static void print_stats(const char *name)
{
	const struct luaz_async_stats *st = bench_async.stats;
	uint64_t busy = st->cyc_total - st->cyc_idle;
	uint64_t sched = busy - st->cyc_resume;

	printk("STATS %s passes=%u resumes=%u wakes=%u idle=%u msgs=%u decodes=%u scans=%u "
	       "busy_cyc=%" PRIu64 " resume=%u.%u%% sched=%u.%u%% drain=%u.%u%% expire=%u.%u%% "
	       "locks=%u.%u%%\n",
	       name, st->passes, st->resumes, st->wakes, st->idle_waits, st->msgs, st->decodes,
	       st->scans, busy, pct(st->cyc_resume, busy) / 10U, pct(st->cyc_resume, busy) % 10U,
	       pct(sched, busy) / 10U, pct(sched, busy) % 10U, pct(st->cyc_drain, busy) / 10U,
	       pct(st->cyc_drain, busy) % 10U, pct(st->cyc_expire, busy) / 10U,
	       pct(st->cyc_expire, busy) % 10U, pct(st->cyc_locks, busy) / 10U,
	       pct(st->cyc_locks, busy) % 10U);
}
#endif

/* bench.measure(name, ops, fn, ...) -> rc; the functions run as the first tasks */
static int l_measure(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	lua_Integer ops = luaL_checkinteger(L, 2);
	int n = lua_gettop(L) - 2;
	struct luaz_alloc_stats a;
	uint64_t cyc;
	uint64_t us;
	uint32_t allocs_x100;
	int rc;

	luaL_argcheck(L, ops > 0, 2, "ops must be > 0");
	luaL_argcheck(L, n >= 1, 3, "function expected");

	(void)lua_gc(L, LUA_GCCOLLECT);
	luaz_alloc_stats_reset();
	luaz_alloc_stats_get(&a);
	uint32_t live0 = a.live;

	wall_start();
	cyc = thread_cycles();
	rc = luaz_async_run(L, &bench_async, n);
	cyc = thread_cycles() - cyc;
	us = wall_us();
	luaz_alloc_stats_get(&a);

	if (CONFIG_BENCH_PROFILE[0] != '\0') {
		lua_pushinteger(L, rc);
		return 1;
	}
	allocs_x100 = (uint32_t)(((uint64_t)a.allocs * 100U) / (uint64_t)ops);
	printk("BENCH %s ops=%d cyc=%" PRIu64 " cyc/op=%" PRIu64 " wall_us=%" PRIu64
	       " allocs/op=%u.%02u bytes/op=%u peak=%u gc=%s rc=%d\n",
	       name, (int)ops, cyc, cyc / (uint64_t)ops, us, allocs_x100 / 100U, allocs_x100 % 100U,
	       (uint32_t)(a.bytes / (uint32_t)ops), a.peak - live0, gc_name, rc);
#ifdef CONFIG_LUA_ASYNC_STATS
	print_stats(name);
#endif
	if (rc != 0) {
		failures++;
	}
	lua_pushinteger(L, rc);

	return 1;
}

/* bench.publish(count, chan_name, ...): publish count messages round-robin */
static int l_publish(lua_State *L)
{
	int count = (int)luaL_checkinteger(L, 1);
	int nchans = lua_gettop(L) - 1;

	luaL_argcheck(L, nchans >= 1 && nchans <= (int)ARRAY_SIZE(pub_all), 2,
		      "1..4 channel names");
	for (int i = 0; i < nchans; i++) {
		const struct zbus_channel *chan = zbus_chan_from_name(luaL_checkstring(L, i + 2));

		luaL_argcheck(L, chan != NULL, i + 2, "unknown channel");
		pub_chans[i] = chan;
	}
	pub_nchans = nchans;
	pub_count = count;
	k_sem_give(&pub_go);

	return 0;
}

/* bench.fail(msg): count a failed check */
static int l_fail(lua_State *L)
{
	printk("BENCH FAIL %s\n", luaL_checkstring(L, 1));
	failures++;

	return 0;
}

/* bench.done(): print the summary line twister waits for */
static int l_done(lua_State *L)
{
	ARG_UNUSED(L);
	printk("bench done: %d failed\n", failures);

	return 0;
}

static int luaopen_bench(lua_State *L)
{
	static const luaL_Reg funcs[] = {
		{"run", l_run},   {"measure", l_measure}, {"publish", l_publish}, {"fail", l_fail},
		{"done", l_done}, {"gc", l_gc},           {NULL, NULL},
	};

	luaL_newlib(L, funcs);
	lua_pushstring(L, CONFIG_BENCH_PROFILE);
	lua_setfield(L, -2, "profile");

	return 1;
}

int bench_lua_setup(lua_State *L)
{
	luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
	lua_pushcfunction(L, luaopen_bench);
	lua_setfield(L, -2, "bench");
	lua_pop(L, 1);

	printk("BENCH_INFO board=%s wall_hz=%u max_tasks=%d depth=%d\n", CONFIG_BOARD_TARGET,
	       wall_hz(), CONFIG_LUA_ASYNC_MAX_TASKS, CONFIG_LUA_ASYNC_CHAN_QUEUE_DEPTH);

	return 0;
}

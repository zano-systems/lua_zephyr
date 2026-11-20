/**
 * @file luaz_async.h
 * @brief Entry point of the zephyr.async scheduler.
 */

#ifndef _LUAZ_ASYNC_H
#define _LUAZ_ASYNC_H

#include <stdint.h>

#include <lua.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/slist.h>
#include <zephyr/zbus/zbus.h>

/** @brief One task's read position on one awaited channel (a mem slab block). */
struct luaz_async_reader {
	sys_snode_t node; /* in the task's reader list */
	uint32_t cursor;  /* last sequence number read */
	uint32_t drops;   /* messages overwritten before this task read them */
	int16_t chan;     /* channel record index */
};

/**
 * @brief Scheduler counters of the last (or current) run (CONFIG_LUA_ASYNC_STATS).
 *
 * Reset when luaz_async_run() starts. Cycle fields use the unit given by
 * @ref cyc_hz. cyc_total covers the whole run; scheduler overhead is
 * cyc_total - cyc_resume - cyc_idle.
 */
struct luaz_async_stats {
	uint32_t passes;     /**< scheduler loop iterations */
	uint32_t resumes;    /**< lua_resume calls */
	uint32_t wakes;      /**< parked tasks made ready */
	uint32_t idle_waits; /**< k_poll sleeps */
	uint32_t msgs;       /**< messages taken from the subscriber */
	uint32_t decodes;    /**< messages converted to Lua tables */
	uint32_t scans;      /**< task slots visited by linear searches */
	uint32_t co_reuses;  /**< spawns that reused a pooled coroutine */
	uint32_t cyc_hz;     /**< cycle counter frequency */
	uint64_t cyc_total;  /**< whole run */
	uint64_t cyc_resume; /**< inside lua_resume (task code + bindings) */
	uint64_t cyc_idle;   /**< sleeping in k_poll */
	uint64_t cyc_drain;  /**< taking and delivering messages */
	uint64_t cyc_expire; /**< timer expiry */
	uint64_t cyc_locks;  /**< lock checks */
};

/** @brief Per-thread resources of the scheduler (see LUAZ_ASYNC_DEFINE). */
struct luaz_async_ctx {
	const struct zbus_observer *sub;
	struct k_mem_slab *readers;
	struct k_timer *timer;      /* wakes the idle scheduler at the earliest deadline */
	struct k_poll_signal *tick; /* raised by timer */
#ifdef CONFIG_LUA_ASYNC_STATS
	struct luaz_async_stats *stats;
#endif
};

#ifdef CONFIG_LUA_ASYNC_STATS
#define Z_LUAZ_ASYNC_STATS_DEFINE(_name) static struct luaz_async_stats _name##_stats
#define Z_LUAZ_ASYNC_STATS_INIT(_name)   .stats = &_name##_stats,
#else
#define Z_LUAZ_ASYNC_STATS_DEFINE(_name) struct luaz_async_stats
#define Z_LUAZ_ASYNC_STATS_INIT(_name)
#endif

/**
 * @brief Reader count for @p _n: 0 means 2 × CONFIG_LUA_ASYNC_MAX_TASKS, capped
 *        at one reader per possible (task, channel) pair.
 */
#define LUAZ_ASYNC_READERS(_n)                                                                     \
	((_n) > 0 ? (_n)                                                                           \
		  : MIN(2 * CONFIG_LUA_ASYNC_MAX_TASKS,                                            \
			CONFIG_LUA_ASYNC_MAX_TASKS * CONFIG_LUA_ASYNC_MAX_CHANNELS))

/**
 * @brief Define the resources of one async thread.
 *
 * Defines the message subscriber `<_name>_sub`, the reader slab
 * `<_name>_readers`, the idle timer `<_name>_timer` with its signal
 * `<_name>_tick`, and the context `_name` to pass to luaz_async_run().
 * With CONFIG_LUA_ASYNC_STATS it also defines `<_name>_stats`.
 *
 * @param _name         Context name.
 * @param _max_readers  (task, channel) pairs awaited at once; 0 for the
 *                      default (see LUAZ_ASYNC_READERS).
 */
/* clang-format off */
#define LUAZ_ASYNC_DEFINE(_name, _max_readers)                                                     \
	ZBUS_MSG_SUBSCRIBER_DEFINE(_name##_sub);                                                   \
	K_MEM_SLAB_DEFINE_STATIC(_name##_readers, sizeof(struct luaz_async_reader),                \
				 LUAZ_ASYNC_READERS(_max_readers), sizeof(void *));                \
	Z_LUAZ_ASYNC_STATS_DEFINE(_name);                                                          \
	K_TIMER_DEFINE(_name##_timer, luaz_async_timer_expired, NULL);                             \
	static struct k_poll_signal _name##_tick = K_POLL_SIGNAL_INITIALIZER(_name##_tick);        \
	static const struct luaz_async_ctx _name = {                                               \
		.sub = &_name##_sub,                                                               \
		.readers = &_name##_readers,                                                       \
		.timer = &_name##_timer,                                                           \
		.tick = &_name##_tick,                                                             \
		Z_LUAZ_ASYNC_STATS_INIT(_name)                                                     \
	}
/* clang-format on */

/**
 * @brief Run the top @p n functions on the stack as the first tasks until no
 *        tasks remain.
 *
 * @param L    Main Lua state of the thread. The functions are popped.
 * @param ctx  Context from LUAZ_ASYNC_DEFINE (static storage).
 * @param n    Number of functions, 1..CONFIG_LUA_ASYNC_MAX_TASKS.
 * @return 0, -ENOMEM (no Lua stack space), -EALREADY (a scheduler already runs
 *         on this Lua state; nothing was changed), -EBUSY (a channel stayed
 *         claimed; its registration is left disabled) or -EFAULT (scheduler fault).
 */
int luaz_async_run(lua_State *L, const struct luaz_async_ctx *ctx, int n);

/**
 * @brief coroutine.running() for async threads.
 *
 * Same results as the standard one; it also marks the calling task as having
 * exposed its coroutine, so the coroutine pool never reuses it.
 *
 * @param L  Calling Lua state.
 * @return 2 (the running coroutine and whether it is the main one).
 */
int luaz_async_coroutine_running(lua_State *L);

/**
 * @brief Expiry handler of the idle timer that LUAZ_ASYNC_DEFINE defines.
 *
 * The timer and its signal are initialized statically, once: re-running
 * k_timer_init() on a live timer would re-link it into the kernel's object
 * lists (CONFIG_OBJ_CORE_TIMER).
 *
 * @param timer  The context's timer; its user data is the context's signal.
 */
void luaz_async_timer_expired(struct k_timer *timer);

/**
 * @brief Warn once per call site when a blocking binding runs under the scheduler.
 *
 * @param L   Calling Lua state.
 * @param fn  Binding name for the warning.
 */
void luaz_async_warn_blocking(lua_State *L, const char *fn);

#endif /* _LUAZ_ASYNC_H */

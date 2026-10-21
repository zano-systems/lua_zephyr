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

/** @brief Per-thread resources of the scheduler (see LUAZ_ASYNC_DEFINE). */
struct luaz_async_ctx {
	const struct zbus_observer *sub;
	struct k_mem_slab *readers;
};

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
 * `<_name>_readers` and the context `_name` to pass to luaz_async_run().
 *
 * @param _name         Context name.
 * @param _max_readers  (task, channel) pairs awaited at once; 0 for the
 *                      default (see LUAZ_ASYNC_READERS).
 */
#define LUAZ_ASYNC_DEFINE(_name, _max_readers)                                                     \
	ZBUS_MSG_SUBSCRIBER_DEFINE(_name##_sub);                                                   \
	K_MEM_SLAB_DEFINE_STATIC(_name##_readers, sizeof(struct luaz_async_reader),                \
				 LUAZ_ASYNC_READERS(_max_readers), sizeof(void *));                \
	static const struct luaz_async_ctx _name = {                                               \
		.sub = &_name##_sub,                                                               \
		.readers = &_name##_readers,                                                       \
	}

/**
 * @brief Run the top @p n functions on the stack as the first tasks until no
 *        tasks remain.
 *
 * @param L    Main Lua state of the thread. The functions are popped.
 * @param ctx  Context from LUAZ_ASYNC_DEFINE (static storage).
 * @param n    Number of functions, 1..CONFIG_LUA_ASYNC_MAX_TASKS.
 * @return 0, -ENOMEM (no Lua stack space), -EBUSY (a channel stayed claimed;
 *         its registration is left disabled) or -EFAULT (scheduler fault).
 */
int luaz_async_run(lua_State *L, const struct luaz_async_ctx *ctx, int n);

/**
 * @brief Warn once per call site when a blocking binding runs under the scheduler.
 *
 * @param L   Calling Lua state.
 * @param fn  Binding name for the warning.
 */
void luaz_async_warn_blocking(lua_State *L, const char *fn);

#endif /* _LUAZ_ASYNC_H */

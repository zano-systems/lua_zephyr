/**
 * @file luaz_async.c
 * @brief zephyr.async: cooperative scheduler on zbus, and its thread entry.
 *
 * A parking call stores its request in the task record and yields no values,
 * so a yield without a request is a stray coroutine.yield. On wake, the reply
 * is pushed onto the task's stack and becomes the parking call's results.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include <luaz_async.h>
#include <luaz_msg_descr.h>
#include <luaz_zbus.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>

#if defined(CONFIG_LUA_ASYNC_STATS) && defined(CONFIG_TIMING_FUNCTIONS)
#include <zephyr/timing/timing.h>
#endif

LOG_MODULE_DECLARE(lua_zephyr, CONFIG_LUA_ZEPHYR_LOG_LEVEL);

#define HANDLE_MT "zephyr.async.handle"
#define MAX_MS    0x7FFFFFFF
#define MAX_TASKS CONFIG_LUA_ASYNC_MAX_TASKS
#define MAX_CHANS CONFIG_LUA_ASYNC_MAX_CHANNELS
#define MAX_LOCKS CONFIG_LUA_ASYNC_MAX_LOCK_WAITS

#define UNWATCH_BUDGET K_MSEC(100)

static const char loop_key;
static const char anchor_key;
static const char rx_key;
static const char warned_key;

enum {
	KIND_YIELD = 1,
	KIND_SLEEP,
	KIND_AWAIT,
	KIND_JOIN,
	KIND_LOCK
};

enum task_state {
	T_FREE = 0,
	T_READY,
	T_RUNNING,
	T_SLEEPING,
	T_WAITING,
	T_JOINING,
	T_LOCKING
};

enum handle_final {
	H_LIVE = 0,
	H_DONE,
	H_FAILED,
	H_CANCELLED
};

struct task {
	lua_State *co;
	int co_ref;
	int handle_ref; /* LUA_NOREF for the initial tasks */
	uint32_t park_seq;
	int32_t deadline;
	int nreply;          /* values on co's stack for the next resume */
	sys_slist_t readers; /* struct luaz_async_reader, one per awaited channel */
	const struct zbus_channel *lock_chan; /* T_LOCKING: the lock it waits for */
	int32_t req_ms;                       /* request: timeout, -1 for forever */
	int16_t target;                       /* T_WAITING: channel; T_JOINING: task; else -1 */
	int16_t req_target;                   /* request: channel or task */
	uint8_t state;
	uint8_t has_deadline;
	uint8_t req; /* KIND_* set by the parking call, 0 for none */
};

struct chan {
	const struct zbus_channel *key; /* NULL: free */
	int ring_ref;                   /* registry ref anchoring ring */
	uint8_t *ring;                  /* depth slots of stride bytes; seq % depth */
	size_t stride;                  /* message size rounded up for alignment */
	uint32_t head;                  /* newest seq */
	uint32_t low;                   /* oldest seq held */
	int depth;
	bool watched;
};

struct loop {
	int current; /* -1 when no task runs */
	int live;
	uint32_t park_seq;
	int rq_head;
	int rq_len;
	int pass_left; /* tasks of the current pass still queued, at the front */
	int ntimed;
	int nlocking; /* tasks in T_LOCKING */
	bool draining;
	struct k_mem_slab *readers;
	struct rx *rx;
#ifdef CONFIG_LUA_ASYNC_STATS
	struct luaz_async_stats *stats;
#endif
	struct task tasks[MAX_TASKS];
	struct chan chans[MAX_CHANS];
	int16_t runq[MAX_TASKS];
	int16_t timed[MAX_TASKS];
};

struct handle {
	int slot;
	uint8_t final;
};

/* Profiling counters (CONFIG_LUA_ASYNC_STATS); no code when disabled. */
#ifdef CONFIG_LUA_ASYNC_STATS
#ifdef CONFIG_TIMING_FUNCTIONS
#define STAT_CLOCK timing_t

static inline timing_t stat_now(void)
{
	return timing_counter_get();
}

static inline uint64_t stat_since(timing_t t0)
{
	timing_t t1 = timing_counter_get();

	return timing_cycles_get(&t0, &t1);
}

static uint32_t stat_hz(void)
{
	return (uint32_t)timing_freq_get();
}
#else
#define STAT_CLOCK uint32_t

static inline uint32_t stat_now(void)
{
	return k_cycle_get_32();
}

static inline uint64_t stat_since(uint32_t t0)
{
	return (uint32_t)(k_cycle_get_32() - t0);
}

static uint32_t stat_hz(void)
{
	return (uint32_t)sys_clock_hw_cycles_per_sec();
}
#endif
#define STAT_ADD(_lp, _f, _n) ((_lp)->stats->_f += (_n))
#define STAT_T0(_v)           STAT_CLOCK _v = stat_now()
#define STAT_CYC(_lp, _f, _v) ((_lp)->stats->_f += stat_since(_v))
#else
#define STAT_ADD(_lp, _f, _n) ((void)(_lp))
#define STAT_T0(_v)           (void)0
#define STAT_CYC(_lp, _f, _v) ((void)(_lp))
#endif
#define STAT_INC(_lp, _f) STAT_ADD(_lp, _f, 1)

/*
 * Per-state record the bindings carry as upvalue 1, so they reach the running
 * scheduler without a registry lookup. lp is NULL between runs, so a task
 * coroutine resumed after its run sees "not running" rather than a stale loop.
 */
struct anchor {
	struct loop *lp;
};

/* Returns the state's anchor, creating it on first use (may raise). */
static struct anchor *anchor_get(lua_State *L)
{
	struct anchor *a;

	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &anchor_key) == LUA_TUSERDATA) {
		a = lua_touserdata(L, -1);
		lua_pop(L, 1);
		return a;
	}
	lua_pop(L, 1);
	a = lua_newuserdatauv(L, sizeof(*a), 0);
	a->lp = NULL;
	lua_rawsetp(L, LUA_REGISTRYINDEX, &anchor_key);

	return a;
}

/* Never raises; NULL when no anchor was created yet. */
static struct anchor *anchor_find(lua_State *L)
{
	struct anchor *a;

	lua_rawgetp(L, LUA_REGISTRYINDEX, &anchor_key);
	a = lua_touserdata(L, -1);
	lua_pop(L, 1);

	return a;
}

/* The running loop as seen from a binding (a C closure with the anchor upvalue). */
static struct loop *up_loop(lua_State *L)
{
	return ((const struct anchor *)lua_touserdata(L, lua_upvalueindex(1)))->lp;
}

static struct loop *get_loop(lua_State *L)
{
	struct loop *lp;

	lua_rawgetp(L, LUA_REGISTRYINDEX, &loop_key);
	lp = lua_touserdata(L, -1);
	lua_pop(L, 1);

	return lp;
}

/* Registry userdata while the scheduler runs; buf holds one message. */
struct rx {
	const struct luaz_async_ctx *ctx;
	size_t buf_size;
	max_align_t buf[];
};

static struct rx *get_rx(lua_State *L)
{
	struct rx *c;

	lua_rawgetp(L, LUA_REGISTRYINDEX, &rx_key);
	c = lua_touserdata(L, -1);
	lua_pop(L, 1);

	return c;
}

static struct rx *check_rx(lua_State *L)
{
	struct rx *c = get_rx(L);

	if (c == NULL) {
		luaL_error(L, "async: no subscriber");
	}

	return c;
}

/*
 * Channel methods carry the channel metatable as upvalue 2: comparing against
 * it avoids luaL_testudata()'s registry lookup by name.
 */
static const struct zbus_channel *to_chan(lua_State *L, int idx)
{
	const struct zbus_channel **ud = lua_touserdata(L, idx);
	bool ok;

	if (ud == NULL || lua_getmetatable(L, idx) == 0) {
		return NULL;
	}
	ok = lua_rawequal(L, -1, lua_upvalueindex(2));
	lua_pop(L, 1);

	return ok ? *ud : NULL;
}

static int32_t uptime(void)
{
	return (int32_t)(uint32_t)k_uptime_get();
}

static int cur_slot(lua_State *L, struct loop *lp)
{
	if (lp == NULL || lp->current < 0) {
		luaL_error(L, "async: not running under the scheduler");
	}

	return lp->current;
}

/* Returns -1 for forever (nil or zephyr.K_FOREVER) when allowed. */
static int32_t check_timeout(lua_State *L, int idx, const char *fname, bool allow_forever)
{
	lua_Integer ms = 0;
	int isint = 0;

	if (allow_forever && lua_isnoneornil(L, idx)) {
		return -1;
	}
	if (lua_type(L, idx) == LUA_TNUMBER) {
		ms = lua_tointegerx(L, idx, &isint);
	}
	if (allow_forever && isint && ms == SYS_FOREVER_MS) {
		return -1;
	}
	if (!isint || ms < 0 || ms > MAX_MS) {
		luaL_error(L, "%s: timeout must be an integer in [0, %d] ms%s", fname, MAX_MS,
			   allow_forever ? " or zephyr.K_FOREVER" : "");
	}

	return (int32_t)ms;
}

static int tostring_value(lua_State *L)
{
	luaL_tolstring(L, 1, NULL);

	return 1;
}

/* Never raises. */
static void report(lua_State *L, const char *prefix, int idx)
{
	const char *name = k_thread_name_get(k_current_get());

	idx = lua_absindex(L, idx);
	if (lua_checkstack(L, 2)) {
		lua_pushcfunction(L, tostring_value);
		lua_pushvalue(L, idx);
		if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
			printk("[%s] %s%s\n", name, prefix, lua_tostring(L, -1));
			lua_pop(L, 1);
			return;
		}
		lua_pop(L, 1);
	}
	printk("[%s] %s(error object is a %s value)\n", name, prefix, luaL_typename(L, idx));
}

static void enqueue(struct loop *lp, int slot)
{
	int i = (lp->rq_head + lp->rq_len) % MAX_TASKS;

	lp->runq[i] = (int16_t)slot;
	lp->rq_len++;
}

static int dequeue(struct loop *lp)
{
	int slot = lp->runq[lp->rq_head];

	lp->rq_head = (lp->rq_head + 1) % MAX_TASKS;
	lp->rq_len--;

	return slot;
}

static void runq_remove(struct loop *lp, int slot)
{
	int k = 0;

	for (int i = 0; i < lp->rq_len; i++) {
		int16_t s = lp->runq[(lp->rq_head + i) % MAX_TASKS];

		if (s != slot) {
			lp->runq[(lp->rq_head + k++) % MAX_TASKS] = s;
		} else if (i < lp->pass_left) {
			lp->pass_left--;
		}
	}
	lp->rq_len = k;
}

/* Deadlines wrap at 32 bits: compare differences. */
static bool before(const struct task *a, const struct task *b)
{
	int32_t d = (int32_t)((uint32_t)a->deadline - (uint32_t)b->deadline);

	if (d != 0) {
		return d < 0;
	}

	return (int32_t)(a->park_seq - b->park_seq) < 0;
}

static void timed_remove(struct loop *lp, int slot)
{
	for (int i = 0; i < lp->ntimed; i++) {
		if (lp->timed[i] == slot) {
			memmove(&lp->timed[i], &lp->timed[i + 1],
				(size_t)(lp->ntimed - i - 1) * sizeof(lp->timed[0]));
			lp->ntimed--;
			return;
		}
	}
}

static void park(lua_State *L, struct loop *lp, int slot, int32_t ms)
{
	struct task *t = &lp->tasks[slot];

	t->park_seq = ++lp->park_seq;
	t->has_deadline = (ms >= 0);
	if (ms >= 0) {
		int i = lp->ntimed;

		t->deadline = (int32_t)((uint32_t)uptime() + (uint32_t)ms);
		while (i > 0 && before(t, &lp->tasks[lp->timed[i - 1]])) {
			lp->timed[i] = lp->timed[i - 1];
			i--;
		}
		lp->timed[i] = (int16_t)slot;
		lp->ntimed++;
	}
}

static void clear_deadline(struct loop *lp, int slot)
{
	if (lp->tasks[slot].has_deadline) {
		timed_remove(lp, slot);
		lp->tasks[slot].has_deadline = 0;
	}
}

/* The top nvals values of L become the parking call's results. */
static void wake(lua_State *L, struct loop *lp, int slot, int nvals)
{
	struct task *t = &lp->tasks[slot];

	STAT_INC(lp, wakes);
	clear_deadline(lp, slot);
	if (t->state == T_LOCKING) {
		lp->nlocking--;
	}
	t->state = T_READY;
	t->target = -1;
	t->lock_chan = NULL;
	if (nvals > 0) {
		luaL_checkstack(t->co, nvals, "async: reply");
		lua_xmove(L, t->co, nvals);
	}
	t->nreply = nvals;
	enqueue(lp, slot);
}

/* Tasks parked in state st on target (a channel or a task), oldest park first. */
static int parked_on(struct loop *lp, uint8_t st, int target, int16_t out[MAX_TASKS])
{
	int n = 0;

	STAT_ADD(lp, scans, MAX_TASKS);
	for (int s = 0; s < MAX_TASKS; s++) {
		const struct task *t = &lp->tasks[s];
		int i;

		if (t->state != st || t->target != target) {
			continue;
		}
		i = n++;
		while (i > 0 && (int32_t)(lp->tasks[out[i - 1]].park_seq - t->park_seq) > 0) {
			out[i] = out[i - 1];
			i--;
		}
		out[i] = (int16_t)s;
	}

	return n;
}

static struct luaz_async_reader *find_reader(struct task *t, int c)
{
	struct luaz_async_reader *rd;

	SYS_SLIST_FOR_EACH_CONTAINER(&t->readers, rd, node) {
		if (rd->chan == c) {
			return rd;
		}
	}

	return NULL;
}

static void free_readers(struct loop *lp, struct task *t)
{
	sys_snode_t *node;

	while ((node = sys_slist_get(&t->readers)) != NULL) {
		k_mem_slab_free(lp->readers, CONTAINER_OF(node, struct luaz_async_reader, node));
	}
}

static int wake_joiners(lua_State *L, struct loop *lp, int slot, int ridx)
{
	int16_t order[MAX_TASKS];
	int n = parked_on(lp, T_JOINING, slot, order);

	ridx = lua_absindex(L, ridx);
	for (int i = 0; i < n; i++) {
		lua_Integer cnt;

		lua_getfield(L, ridx, "n");
		cnt = lua_tointeger(L, -1);
		lua_pop(L, 1);
		luaL_checkstack(L, (int)cnt, "async: join results");
		for (lua_Integer k = 1; k <= cnt; k++) {
			lua_rawgeti(L, ridx, k);
		}
		wake(L, lp, order[i], (int)cnt);
	}

	return n;
}

static int alloc_slot(struct loop *lp)
{
	for (int s = 0; s < MAX_TASKS; s++) {
		if (lp->tasks[s].state == T_FREE) {
			return s;
		}
	}

	return -1;
}

/* Takes the function and nargs arguments on top of L. */
static int new_task(lua_State *L, struct loop *lp, int nargs)
{
	int slot = alloc_slot(lp);
	struct task *t = &lp->tasks[slot];
	lua_State *co;

	memset(t, 0, sizeof(*t));
	co = lua_newthread(L);
	/* Raise before the slot is taken: a new coroutine only guarantees LUA_MINSTACK slots. */
	if (lua_checkstack(co, nargs + 1) == 0) {
		return luaL_error(L, "async.spawn: too many arguments (%d)", nargs);
	}
	t->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	t->co = co;
	lua_xmove(L, co, nargs + 1);
	t->nreply = nargs;
	t->handle_ref = LUA_NOREF;
	sys_slist_init(&t->readers);
	t->target = -1;
	t->state = T_READY;

	lp->live++;
	enqueue(lp, slot);

	return slot;
}

static void detach(struct loop *lp, int slot)
{
	if (lp->tasks[slot].state == T_READY) {
		runq_remove(lp, slot);
	}
	clear_deadline(lp, slot);
}

/* The handle keeps the outcome, so it stays valid after the slot is reused. */
static void retire(lua_State *L, struct loop *lp, int slot, uint8_t final, int ridx)
{
	struct task *t = &lp->tasks[slot];

	ridx = lua_absindex(L, ridx);
	if (t->handle_ref != LUA_NOREF) {
		struct handle *h;

		lua_rawgeti(L, LUA_REGISTRYINDEX, t->handle_ref);
		h = lua_touserdata(L, -1);
		h->final = final;
		lua_pushvalue(L, ridx);
		lua_setiuservalue(L, -2, 1);
		lua_pop(L, 1);
		luaL_unref(L, LUA_REGISTRYINDEX, t->handle_ref);
	}

	luaL_unref(L, LUA_REGISTRYINDEX, t->co_ref);
	free_readers(lp, t);
	if (t->state == T_LOCKING) {
		lp->nlocking--;
	}
	memset(t, 0, sizeof(*t));
	t->state = T_FREE;
	lp->live--;
}

/* Replaces the top n values with {n = n, ...}. */
static void pack(lua_State *L, int n)
{
	int t;

	lua_createtable(L, n, 1);
	lua_insert(L, -n - 1);
	t = lua_absindex(L, -n - 1);
	for (int i = n; i >= 1; i--) {
		lua_rawseti(L, t, i);
	}
	lua_pushinteger(L, n);
	lua_setfield(L, t, "n");
}

static void push_failure(lua_State *L, int idx)
{
	idx = lua_absindex(L, idx);
	lua_pushboolean(L, 0);
	lua_pushvalue(L, idx);
	pack(L, 2);
}

/* Pushes the coroutine's close error, or nil. */
static bool close_task(lua_State *L, struct loop *lp, int slot)
{
	lua_State *co = lp->tasks[slot].co;

	if (lua_closethread(co, L) != LUA_OK) {
		lua_xmove(co, L, 1);
		return true;
	}
	lua_pushnil(L);

	return false;
}

/* Printed only when no joiner is waiting for the error. */
static void task_failed(lua_State *L, struct loop *lp, int slot, int eidx, int tbidx)
{
	int njoiners;

	eidx = lua_absindex(L, eidx);
	tbidx = tbidx != 0 ? lua_absindex(L, tbidx) : eidx;
	push_failure(L, eidx);
	njoiners = wake_joiners(L, lp, slot, -1);
	retire(L, lp, slot, H_FAILED, -1);
	if (njoiners == 0) {
		report(L, "async task error: ", tbidx);
	}
	lua_pop(L, 1);
}

static void run_task(lua_State *L, struct loop *lp, int slot)
{
	struct task *t = &lp->tasks[slot];
	lua_State *co = t->co;
	int nres = 0;
	int kind;
	int st;

	t->state = T_RUNNING;
	lp->current = slot;
	STAT_INC(lp, resumes);
	STAT_T0(t0);
	st = lua_resume(co, L, t->nreply, &nres);
	STAT_CYC(lp, cyc_resume, t0);
	lp->current = -1;
	t->nreply = 0;

	if (st == LUA_OK) {
		int top = lua_gettop(L);

		if (t->handle_ref == LUA_NOREF) {
			/* An initial task: no handle keeps the results, so no one can join. */
			retire(L, lp, slot, H_DONE, 0);
			return;
		}
		luaL_checkstack(L, nres + 4, "async: results");
		lua_pushboolean(L, 1);
		lua_xmove(co, L, nres);
		pack(L, nres + 1);
		wake_joiners(L, lp, slot, -1);
		retire(L, lp, slot, H_DONE, -1);
		lua_settop(L, top);
		return;
	}

	if (st != LUA_YIELD) {
		/* As coroutine.wrap does, a __close error replaces the original. */
		int top = lua_gettop(L);
		const char *msg = lua_tostring(co, -1);

		/* co has no free slots after an error: copy the error through L. */
		lua_xmove(co, L, 1); /* top+1: error */
		lua_pushvalue(L, top + 1);
		lua_xmove(L, co, 1); /* back on co for lua_closethread */
		luaL_traceback(L, co,
			       msg != NULL ? msg
					   : lua_pushfstring(L, "(error object is a %s value)",
							     luaL_typename(L, top + 1)),
			       0);
		if (msg == NULL) {
			lua_remove(L, -2);
		}
		if (lua_closethread(co, L) != LUA_OK) {
			lua_xmove(co, L, 1);
			if (!lua_rawequal(L, top + 1, top + 3)) {
				lua_replace(L, top + 1);
				lua_pushnil(L);
				lua_replace(L, top + 2);
			} else {
				lua_pop(L, 1);
			}
		}
		task_failed(L, lp, slot, top + 1, lua_isnil(L, top + 2) ? 0 : top + 2);
		lua_settop(L, top);
		return;
	}

	/* Yields without a request come from coroutine.yield in a task body. */
	kind = t->req;
	t->req = 0;
	if (kind == 0 || nres != 0) {
		lua_pop(co, nres);
		(void)close_task(L, lp, slot);
		lua_pop(L, 1);
		lua_pushliteral(L, "async: untagged yield reached the scheduler");
		task_failed(L, lp, slot, -1, 0);
		lua_pop(L, 1);
		return;
	}

	switch (kind) {
	case KIND_YIELD:
		t->state = T_READY;
		enqueue(lp, slot);
		break;
	case KIND_SLEEP:
		t->state = T_SLEEPING;
		park(L, lp, slot, t->req_ms);
		break;
	case KIND_AWAIT:
		t->state = T_WAITING;
		t->target = t->req_target;
		park(L, lp, slot, t->req_ms);
		break;
	case KIND_JOIN:
		t->state = T_JOINING;
		t->target = t->req_target;
		park(L, lp, slot, t->req_ms);
		break;
	case KIND_LOCK:
		t->state = T_LOCKING;
		lp->nlocking++;
		park(L, lp, slot, t->req_ms);
		break;
	default:
		luaL_error(L, "async: unknown request");
	}
}

static int chan_find(struct loop *lp, const struct zbus_channel *key)
{
	for (int c = 0; c < MAX_CHANS; c++) {
		if (lp->chans[c].key == key) {
			return c;
		}
	}

	return -1;
}

static uint8_t *ring_slot(const struct chan *ch, uint32_t seq)
{
	return ch->ring + (size_t)(seq % (uint32_t)ch->depth) * ch->stride;
}

/* Pushes a ring userdata of depth slots; raises on overflow or no memory. */
static uint8_t *ring_new(lua_State *L, size_t stride, lua_Integer depth)
{
	if ((uint64_t)depth > (uint64_t)(SIZE_MAX / stride) || depth > INT32_MAX) {
		luaL_error(L, "queue_depth: depth %d is too large", (int)depth);
	}

	return lua_newuserdatauv(L, stride * (size_t)depth, 0);
}

/* -1 when every channel record is taken. */
static int chan_get(lua_State *L, struct loop *lp, const struct zbus_channel *key, int idx)
{
	size_t stride;
	uint8_t *ring;
	int ring_ref;
	int c;

	if (key == NULL) {
		luaL_typeerror(L, idx, "zbus channel");
	}
	c = chan_find(lp, key);
	if (c >= 0) {
		return c;
	}
	c = chan_find(lp, NULL);
	if (c < 0) {
		return -1;
	}

	/* Allocate before taking the record: if this raises, the record stays free. */
	stride = ROUND_UP(MAX(zbus_chan_msg_size(key), 1U), sizeof(max_align_t));
	ring = ring_new(L, stride, CONFIG_LUA_ASYNC_CHAN_QUEUE_DEPTH);
	ring_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	lp->chans[c] = (struct chan){
		.key = key,
		.ring_ref = ring_ref,
		.ring = ring,
		.stride = stride,
		.head = 0,
		.low = 1,
		.depth = CONFIG_LUA_ASYNC_CHAN_QUEUE_DEPTH,
	};

	return c;
}

/* Forgets the messages that no longer fit in depth slots. */
static void ring_trim(struct chan *ch)
{
	uint32_t min = ch->head - (uint32_t)ch->depth + 1;

	if ((int32_t)(min - ch->low) > 0) {
		ch->low = min;
	}
}

/* Moves the newest messages that fit into a ring of depth slots. */
static void ring_resize(lua_State *L, struct chan *ch, lua_Integer depth)
{
	uint8_t *ring = ring_new(L, ch->stride, depth);
	uint32_t from = ch->head - (uint32_t)depth + 1;

	if ((int32_t)(from - ch->low) < 0) {
		from = ch->low;
	}
	for (uint32_t seq = from; (int32_t)(ch->head - seq) >= 0; seq++) {
		memcpy(ring + (size_t)(seq % (uint32_t)depth) * ch->stride, ring_slot(ch, seq),
		       ch->stride);
	}
	luaL_unref(L, LUA_REGISTRYINDEX, ch->ring_ref);
	ch->ring_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	ch->ring = ring;
	ch->depth = (int)depth;
	ch->low = from;
}

static void push_decoded(lua_State *L, struct loop *lp, int c, uint32_t seq)
{
	const struct zbus_channel *chan = lp->chans[c].key;
	const struct lua_msg_descr *descr = zbus_chan_user_data(chan);
	struct rx *cl = lp->rx;
	size_t len = zbus_chan_msg_size(chan);

	if (len > cl->buf_size) {
		luaL_error(L, "async: cannot decode message on '%s'", zbus_chan_name(chan));
	}
	/* Decode a copy: a __gc run by the decoder's allocations may resize the ring. */
	memcpy(cl->buf, ring_slot(&lp->chans[c], seq), len);
	STAT_INC(lp, decodes);
	lua_msg_descr_to_table(L, descr->fields, descr->field_count, cl->buf);
}

/* Stores msg as the channel's newest message and wakes its waiters. */
static void deliver(lua_State *L, struct loop *lp, const struct zbus_channel *key, const void *msg)
{
	int c = chan_find(lp, key);
	struct chan *ch;
	int16_t order[MAX_TASKS];
	int n;

	if (c < 0) {
		return;
	}
	ch = &lp->chans[c];
	ch->head++;
	memcpy(ring_slot(ch, ch->head), msg, zbus_chan_msg_size(key));
	ring_trim(ch);

	/* A waiter got its reader before parking. */
	n = parked_on(lp, T_WAITING, c, order);
	for (int i = 0; i < n; i++) {
		find_reader(&lp->tasks[order[i]], c)->cursor = ch->head;
		push_decoded(L, lp, c, ch->head);
		wake(L, lp, order[i], 1);
	}
}

static void expire(lua_State *L, struct loop *lp)
{
	int32_t now;

	if (lp->ntimed == 0) {
		return;
	}
	now = uptime();

	while (lp->ntimed > 0) {
		int slot = lp->timed[0];
		struct task *t = &lp->tasks[slot];

		if ((int32_t)((uint32_t)t->deadline - (uint32_t)now) > 0) {
			break;
		}
		timed_remove(lp, slot);
		t->has_deadline = 0;
		if (t->state == T_SLEEPING) {
			wake(L, lp, slot, 0);
		} else {
			lua_pushnil(L);
			lua_pushinteger(L, -EAGAIN);
			wake(L, lp, slot, 2);
		}
	}
}

/* Takes the next message into the rx buffer and sets *chan; 0 when none is queued. */
static int poll(struct rx *c, const struct zbus_channel **chan)
{
	*chan = NULL;

	return zbus_sub_wait_msg(c->ctx->sub, chan, c->buf, K_NO_WAIT) == 0 && *chan != NULL;
}

static void check_locks(lua_State *L, struct loop *lp)
{
	if (lp->nlocking == 0) {
		return;
	}
	STAT_ADD(lp, scans, MAX_TASKS);
	for (int s = 0; s < MAX_TASKS; s++) {
		struct task *t = &lp->tasks[s];

		if (t->state == T_LOCKING && k_sem_count_get(&t->lock_chan->data->sem) > 0) {
			wake(L, lp, s, 0);
		}
	}
}

/*
 * Sleeps until a message is queued, a lock a task waits for is free, or the
 * timeout. One event per distinct lock, up to MAX_LOCKS: beyond that the wait
 * is cut to one tick, so check_locks() sees the other locks within a tick.
 */
static void idle_wait(lua_State *L, struct loop *lp, int32_t timeout_ms)
{
	struct k_poll_event ev[1 + MAX_LOCKS];
	k_timeout_t timeout = timeout_ms < 0 ? K_FOREVER : K_MSEC(timeout_ms);
	bool overflow = false;
	int n = 0;

	k_poll_event_init(&ev[n++], K_POLL_TYPE_FIFO_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
			  lp->rx->ctx->sub->message_fifo);
	if (lp->nlocking > 0) {
		STAT_ADD(lp, scans, MAX_TASKS);
	}
	for (int s = 0; s < MAX_TASKS && !overflow && lp->nlocking > 0; s++) {
		struct k_sem *sem;
		int i = 1;

		if (lp->tasks[s].state != T_LOCKING) {
			continue;
		}
		sem = &lp->tasks[s].lock_chan->data->sem;
		while (i < n && ev[i].sem != sem) {
			i++;
		}
		if (i < n) {
			continue; /* another task waits for the same lock */
		}
		if (n == ARRAY_SIZE(ev)) {
			overflow = true;
		} else {
			k_poll_event_init(&ev[n++], K_POLL_TYPE_SEM_AVAILABLE,
					  K_POLL_MODE_NOTIFY_ONLY, sem);
		}
	}
	if (overflow && (timeout_ms < 0 || k_ms_to_ticks_ceil32((uint32_t)timeout_ms) > 1)) {
		timeout = K_TICKS(1);
	}
	(void)k_poll(ev, n, timeout);
}

/* True when more messages may be queued. Not reentered from a __gc that awaits. */
static bool drain(lua_State *L, struct loop *lp)
{
	const struct zbus_channel *key;
	struct rx *c = lp->rx;
	bool hit_limit = true;

	if (lp->draining) {
		return false;
	}
	lp->draining = true;
	for (int i = 0; i < CONFIG_LUA_ASYNC_MAX_DRAIN; i++) {
		/* Lock-free peek: skips the locked get when nothing is queued. */
		if (k_fifo_is_empty(c->ctx->sub->message_fifo) || poll(c, &key) == 0) {
			hit_limit = false;
			break;
		}
		STAT_INC(lp, msgs);
		deliver(L, lp, key, c->buf);
	}
	lp->draining = false;

	return hit_limit;
}

static int loop_body(lua_State *L)
{
	struct loop *lp = get_loop(L);

	while (lp->live > 0) {
		bool hit_limit;

		STAT_INC(lp, passes);
		STAT_T0(t_drain);
		hit_limit = drain(L, lp);
		STAT_CYC(lp, cyc_drain, t_drain);

		STAT_T0(t_expire);
		expire(L, lp);
		STAT_CYC(lp, cyc_expire, t_expire);

		STAT_T0(t_locks);
		check_locks(L, lp);
		STAT_CYC(lp, cyc_locks, t_locks);

		/* Tasks made ready while running wait for the next pass. */
		lp->pass_left = lp->rq_len;
		while (lp->pass_left > 0) {
			lp->pass_left--;
			run_task(L, lp, dequeue(lp));
		}

		if (lp->live > 0 && lp->rq_len == 0 && !hit_limit) {
			int32_t timeout = -1;

			if (lp->ntimed > 0) {
				timeout = (int32_t)((uint32_t)lp->tasks[lp->timed[0]].deadline -
						    (uint32_t)uptime());
				if (timeout < 0) {
					timeout = 0;
				}
			}
			STAT_INC(lp, idle_waits);
			STAT_T0(t_idle);
			idle_wait(L, lp, timeout);
			STAT_CYC(lp, cyc_idle, t_idle);
		}
	}

	return 0;
}

static int fault_handler(lua_State *L)
{
	const char *msg = lua_tostring(L, 1);

	luaL_traceback(L, L, msg != NULL ? msg : "(non-string error)", 1);

	return 1;
}

/* The arguments, all functions, become the first tasks. */
static int l_run(lua_State *L)
{
	struct anchor *anchor;
	struct loop *lp = get_loop(L);
	int n = lua_gettop(L);
	const struct zbus_observer *sub = check_rx(L)->ctx->sub;
	k_timepoint_t end;
	bool busy = false;
	int st;

	if (lp != NULL) {
		return luaL_error(L, "async: scheduler already running");
	}
	if (n < 1 || n > MAX_TASKS) {
		return luaL_error(L, "async: %d initial tasks (1..%d)", n, MAX_TASKS);
	}
	for (int i = 1; i <= n; i++) {
		luaL_checktype(L, i, LUA_TFUNCTION);
	}

	anchor = anchor_get(L);
	lp = lua_newuserdatauv(L, sizeof(*lp), 0);
	memset(lp, 0, sizeof(*lp));
	lp->current = -1;
	lp->rx = check_rx(L);
	lp->readers = lp->rx->ctx->readers;
#ifdef CONFIG_LUA_ASYNC_STATS
	lp->stats = lp->rx->ctx->stats;
	memset(lp->stats, 0, sizeof(*lp->stats));
	lp->stats->cyc_hz = stat_hz();
#endif
	lua_rawsetp(L, LUA_REGISTRYINDEX, &loop_key);
	anchor->lp = lp;

	for (int i = 1; i <= n; i++) {
		lua_pushvalue(L, i);
		new_task(L, lp, 0);
	}

	lua_pushcfunction(L, fault_handler);
	lua_pushcfunction(L, loop_body);
	{
		STAT_T0(t_total);
		st = lua_pcall(L, 0, 0, -2);
		STAT_CYC(lp, cyc_total, t_total);
	}
	lp->current = -1;

	if (st != LUA_OK) {
		/* Engine state can't be trusted after a fault: end the run. */
		report(L, "async: scheduler fault: ", -1);
		for (int s = 0; s < MAX_TASKS; s++) {
			if (lp->tasks[s].state != T_FREE) {
				(void)close_task(L, lp, s);
				lua_pop(L, 1);
				lua_pushinteger(L, -ECANCELED);
				push_failure(L, -1);
				retire(L, lp, s, H_CANCELLED, -1);
				lua_pop(L, 2);
			}
		}
	}

	(void)zbus_obs_set_enable(sub, false);
	end = sys_timepoint_calc(UNWATCH_BUDGET);
	for (int c = 0; c < MAX_CHANS; c++) {
		struct chan *ch = &lp->chans[c];

		if (ch->key != NULL) {
			if (ch->watched) {
				int rc = zbus_chan_rm_obs(ch->key, sub, sys_timepoint_timeout(end));

				if (rc != 0 && rc != -ENODATA) {
					printk("[%s] async: '%s' is still claimed, left watched "
					       "but "
					       "disabled (%d)\n",
					       k_thread_name_get(k_current_get()),
					       zbus_chan_name(ch->key), rc);
					busy = true;
				}
			}
			luaL_unref(L, LUA_REGISTRYINDEX, ch->ring_ref);
			ch->key = NULL;
		}
	}
	lua_pushnil(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &loop_key);
	anchor->lp = NULL;

	lua_pushinteger(L, st != LUA_OK ? -EFAULT : busy ? -EBUSY : 0);
	return 1;
}

/*
 * Records the parking request of the current task. When the task cannot
 * yield, nothing is recorded and the caller's lua_yield raises its usual
 * error, so a request is only ever left behind by a real yield.
 */
static void set_request(lua_State *L, int kind, int target, int32_t ms)
{
	struct loop *lp = up_loop(L);
	struct task *t = &lp->tasks[lp->current];

	/* From a nested coroutine the yield would reach its resumer, not the scheduler. */
	if (L != t->co) {
		luaL_error(L, "async: cannot park inside a coroutine");
	}
	if (!lua_isyieldable(L)) {
		return;
	}
	t->req = (uint8_t)kind;
	t->req_target = (int16_t)target;
	t->req_ms = ms;
}

static int request(lua_State *L, int kind, int target, int32_t ms)
{
	set_request(L, kind, target, ms);

	return lua_yield(L, 0);
}

static int l_spawn(lua_State *L)
{
	struct loop *lp;
	struct handle *h;
	int nargs = lua_gettop(L) - 1;
	int slot;

	luaL_checktype(L, 1, LUA_TFUNCTION);
	lp = up_loop(L);
	if (lp == NULL) {
		return luaL_error(L, "async: not running under the scheduler");
	}
	if (lp->live >= MAX_TASKS) {
		return luaL_error(L,
				  "async.spawn: task limit reached (CONFIG_LUA_ASYNC_MAX_TASKS)");
	}

	slot = new_task(L, lp, nargs);

	h = lua_newuserdatauv(L, sizeof(*h), 1);
	h->slot = slot;
	h->final = H_LIVE;
	luaL_setmetatable(L, HANDLE_MT);
	lua_pushvalue(L, -1);
	lp->tasks[slot].handle_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	return 1;
}

static int l_sleep(lua_State *L)
{
	int32_t ms = check_timeout(L, 1, "async.sleep", false);

	(void)cur_slot(L, up_loop(L));
	if (ms == 0) {
		return request(L, KIND_YIELD, -1, 0);
	}

	return request(L, KIND_SLEEP, -1, ms);
}

static int l_yield(lua_State *L)
{
	(void)cur_slot(L, up_loop(L));

	return request(L, KIND_YIELD, -1, 0);
}

static struct handle *check_handle(lua_State *L)
{
	return luaL_checkudata(L, 1, HANDLE_MT);
}

static int push_results(lua_State *L, int hidx)
{
	lua_Integer n;

	lua_getiuservalue(L, hidx, 1);
	lua_getfield(L, -1, "n");
	n = lua_tointeger(L, -1);
	lua_pop(L, 1);
	luaL_checkstack(L, (int)n, "async: join results");
	for (lua_Integer i = 1; i <= n; i++) {
		lua_rawgeti(L, -(int)i, i);
	}
	lua_remove(L, -(int)n - 1);

	return (int)n;
}

static int l_join(lua_State *L)
{
	struct handle *h = check_handle(L);
	int32_t ms = check_timeout(L, 2, "join", true);
	struct loop *lp = up_loop(L);
	int cur = cur_slot(L, lp);

	if (h->final != H_LIVE) {
		return push_results(L, 1);
	}
	if (h->slot == cur) {
		return luaL_error(L, "async: a task cannot join itself");
	}
	if (ms == 0) {
		lua_pushnil(L);
		lua_pushinteger(L, -EAGAIN);
		return 2;
	}

	return request(L, KIND_JOIN, h->slot, ms);
}

static int l_cancel(lua_State *L)
{
	struct handle *h = check_handle(L);
	struct loop *lp = up_loop(L);
	int slot = h->slot;
	bool failed;

	if (h->final != H_LIVE || lp == NULL) {
		lua_pushboolean(L, 0);
		return 1;
	}
	if (slot == lp->current) {
		return luaL_error(L, "async: a task cannot cancel itself");
	}

	/* Allocate before changing any state; report only once the task is retired. */
	lua_pushinteger(L, -ECANCELED);
	push_failure(L, -1);
	detach(lp, slot);
	failed = close_task(L, lp, slot);
	wake_joiners(L, lp, slot, -2);
	retire(L, lp, slot, H_CANCELLED, -2);
	if (failed) {
		report(L, "async: error while cancelling task: ", -1);
	}
	lua_pop(L, 3);
	lua_pushboolean(L, 1);

	return 1;
}

static int l_status(lua_State *L)
{
	static const char *const finals[] = {NULL, "done", "failed", "cancelled"};
	static const char *const states[] = {"free",    "ready",   "running", "sleeping",
					     "waiting", "joining", "locking"};
	struct handle *h = check_handle(L);
	struct loop *lp = up_loop(L);

	if (h->final != H_LIVE) {
		lua_pushstring(L, finals[h->final]);
	} else if (lp != NULL && lp->current == h->slot) {
		lua_pushliteral(L, "running");
	} else {
		lua_pushstring(L, states[lp->tasks[h->slot].state]);
	}

	return 1;
}

/* Pushes the deadline ms from now, or nil for forever (ms < 0). */
static void push_deadline(lua_State *L, int32_t ms)
{
	if (ms < 0) {
		lua_pushnil(L);
	} else {
		lua_pushinteger(L, (lua_Integer)(int32_t)((uint32_t)uptime() + (uint32_t)ms));
	}
}

/* Ms left until the deadline at idx: -1 for forever, 0 once it has passed. */
static int32_t time_left(lua_State *L, int idx)
{
	int32_t left;

	if (lua_isnil(L, idx)) {
		return -1;
	}
	left = (int32_t)((uint32_t)lua_tointeger(L, idx) - (uint32_t)uptime());

	return left > 0 ? left : 0;
}

static int park_on_lock(lua_State *L, const struct zbus_channel *chan, int32_t left,
			lua_KContext ctx, lua_KFunction k)
{
	set_request(L, KIND_LOCK, -1, left);
	up_loop(L)->tasks[up_loop(L)->current].lock_chan = chan;

	return lua_yieldk(L, 0, ctx, k);
}

static int await_try(lua_State *L);

static int await_k(lua_State *L, int status, lua_KContext ctx)
{
	ARG_UNUSED(status);
	ARG_UNUSED(ctx);

	if (lua_gettop(L) > 2) { /* expire() replied nil, -EAGAIN */
		return 2;
	}

	return await_try(L);
}

/* One attempt of await; the stack is 1 chan, 2 deadline or nil (forever). */
static int await_try(lua_State *L)
{
	struct loop *lp = up_loop(L);
	int slot = cur_slot(L, lp);
	const struct zbus_channel *chan = to_chan(L, 1);
	int32_t left = time_left(L, 2);
	struct luaz_async_reader *rd;
	struct chan *ch;
	int c;

	c = chan_get(L, lp, chan, 1);
	if (c < 0) {
		return luaL_error(L, "await: cannot watch '%s' (%d)", zbus_chan_name(chan),
				  -ENOMEM);
	}
	ch = &lp->chans[c];
	if (!ch->watched) {
		int rc = zbus_chan_add_obs(chan, lp->rx->ctx->sub, K_NO_WAIT);

		if (rc == -EBUSY) {
			if (left == 0) {
				lua_pushnil(L);
				lua_pushinteger(L, -EAGAIN);
				return 2;
			}
			return park_on_lock(L, chan, left, 0, await_k);
		}
		/* -EALREADY: left registered by an earlier run. */
		if (rc != 0 && rc != -EALREADY) {
			return luaL_error(L, "await: cannot watch '%s' (%d)", zbus_chan_name(chan),
					  rc);
		}
		ch->watched = true;
	}

	rd = find_reader(&lp->tasks[slot], c);
	if (rd == NULL) { /* first await: wait for the next message */
		if (k_mem_slab_alloc(lp->readers, (void **)&rd, K_NO_WAIT) != 0) {
			return luaL_error(L, "await: too many readers "
					     "(CONFIG_<NAME>_LUA_ASYNC_MAX_READERS)");
		}
		*rd = (struct luaz_async_reader){.cursor = ch->head, .chan = (int16_t)c};
		sys_slist_append(&lp->tasks[slot].readers, &rd->node);
	}

	for (bool drained = false;; drained = true) {
		if ((int32_t)(ch->head - rd->cursor) > 0) {
			uint32_t nxt = rd->cursor + 1;

			if ((int32_t)(ch->low - nxt) > 0) { /* overwritten while away */
				rd->drops += ch->low - nxt;
				nxt = ch->low;
			}
			rd->cursor = nxt;
			push_decoded(L, lp, c, nxt);
			return 1;
		}
		if (left != 0) {
			return request(L, KIND_AWAIT, c, left);
		}
		if (drained) {
			break;
		}
		(void)drain(L, lp);
	}
	lua_pushnil(L);
	lua_pushinteger(L, -EAGAIN);

	return 2;
}

static int l_await(lua_State *L)
{
	int32_t ms = check_timeout(L, 2, "await", true);
	const struct zbus_channel *chan = to_chan(L, 1);

	(void)cur_slot(L, up_loop(L));
	if (chan == NULL) {
		return luaL_typeerror(L, 1, "zbus channel");
	}
	if (zbus_chan_user_data(chan) == NULL) {
		return luaL_error(L, "await: channel '%s' has no message descriptor",
				  zbus_chan_name(chan));
	}
	lua_settop(L, 1);
	push_deadline(L, ms);

	return await_try(L);
}

static int l_queue_depth(lua_State *L)
{
	lua_Integer n = 0;
	struct loop *lp;
	struct chan *ch;
	int c;

	if (!lua_isnoneornil(L, 2)) {
		int isint = 0;

		if (lua_type(L, 2) == LUA_TNUMBER) {
			n = lua_tointegerx(L, 2, &isint);
		}
		if (!isint || n < 1) {
			return luaL_error(L, "queue_depth: depth must be an integer >= 1");
		}
	}
	lp = up_loop(L);
	if (lp == NULL) {
		return luaL_error(L, "async: not running under the scheduler");
	}
	c = chan_get(L, lp, to_chan(L, 1), 1);
	if (c < 0) {
		return luaL_error(L,
				  "queue_depth: too many channels (CONFIG_LUA_ASYNC_MAX_CHANNELS)");
	}
	ch = &lp->chans[c];
	if (n > 0 && n != ch->depth) {
		ring_resize(L, ch, n);
	}
	lua_pushinteger(L, ch->depth);

	return 1;
}

static int l_dropped(lua_State *L)
{
	struct loop *lp = up_loop(L);
	int slot = cur_slot(L, lp);
	const struct zbus_channel *key = to_chan(L, 1);
	struct luaz_async_reader *rd;
	uint32_t n = 0;
	int c;

	if (key == NULL) {
		return luaL_typeerror(L, 1, "zbus channel");
	}
	c = chan_find(lp, key);
	rd = c >= 0 ? find_reader(&lp->tasks[slot], c) : NULL;
	if (rd != NULL) {
		/* Overwritten but not yet skipped; the next await counts it once. */
		int32_t gap = (int32_t)(lp->chans[c].low - (rd->cursor + 1));

		n = rd->drops;
		if (gap > 0) {
			n += (uint32_t)gap;
		}
	}
	lua_pushinteger(L, (lua_Integer)n);

	return 1;
}

enum {
	OP_PUB = 1,
	OP_WAITED = 2
};

static int lock_op_try(lua_State *L, lua_KContext ctx);

static int lock_op_k(lua_State *L, int status, lua_KContext ctx)
{
	ARG_UNUSED(status);

	if (lua_gettop(L) > 4) { /* expire() replied nil, -EAGAIN */
		return 2;
	}

	return lock_op_try(L, ctx);
}

/*
 * One attempt of pub_async/read_async; the stack is 1 chan, 2 msg or nil,
 * 3 deadline or nil (forever), 4 message buffer. Only a busy lock is waited
 * for: once zbus holds it, a delivery error may follow a partial delivery.
 */
static int lock_op_try(lua_State *L, lua_KContext ctx)
{
	const struct zbus_channel *chan = to_chan(L, 1);
	void *buf = lua_touserdata(L, 4);
	int err;

	err = (ctx & OP_PUB) ? zbus_chan_pub(chan, buf, K_NO_WAIT)
			     : zbus_chan_read(chan, buf, K_NO_WAIT);
	if (err == 0) {
		if (ctx & OP_PUB) {
			lua_pushboolean(L, 1);
		} else {
			const struct lua_msg_descr *descr = zbus_chan_user_data(chan);

			lua_msg_descr_to_table(L, descr->fields, descr->field_count, buf);
		}
		return 1;
	}
	if (err == -EBUSY) {
		int32_t left = time_left(L, 3);

		if (left != 0) {
			return park_on_lock(L, chan, left, ctx | OP_WAITED, lock_op_k);
		}
		if (ctx & OP_WAITED) {
			err = -EAGAIN;
		}
	}
	lua_pushnil(L);
	lua_pushinteger(L, err);

	return 2;
}

static int start_lock_op(lua_State *L, const char *fname, int timeout_idx, lua_KContext ctx)
{
	struct loop *lp = up_loop(L);
	const struct zbus_channel *chan = to_chan(L, 1);
	size_t size;
	int32_t ms;
	void *buf;

	(void)cur_slot(L, lp);
	if (chan == NULL) {
		return luaL_typeerror(L, 1, "zbus channel");
	}
	if (zbus_chan_user_data(chan) == NULL) {
		return luaL_error(L, "%s: channel '%s' has no message descriptor", fname,
				  zbus_chan_name(chan));
	}
	ms = check_timeout(L, timeout_idx, fname, true);

	lua_settop(L, timeout_idx - 1);
	if (lua_gettop(L) == 1) {
		lua_pushnil(L);
	}
	push_deadline(L, ms);
	size = zbus_chan_msg_size(chan);
	buf = lua_newuserdatauv(L, size, 0);
	memset(buf, 0, size);
	if (ctx & OP_PUB) {
		const struct lua_msg_descr *descr = zbus_chan_user_data(chan);

		lua_msg_descr_from_table(L, descr->fields, descr->field_count, buf, 2);
	}

	return lock_op_try(L, ctx);
}

static int l_pub_async(lua_State *L)
{
	luaL_checktype(L, 2, LUA_TTABLE);

	return start_lock_op(L, "pub_async", 3, OP_PUB);
}

static int l_read_async(lua_State *L)
{
	return start_lock_op(L, "read_async", 2, 0);
}

#ifdef CONFIG_LUA_ASYNC_STATS
/* async.stats() -> table of the current run's counters */
static int l_stats(lua_State *L)
{
	struct loop *lp = up_loop(L);
	const struct luaz_async_stats *st;

	(void)cur_slot(L, lp);
	st = lp->stats;

	lua_createtable(L, 0, 14);
#define STAT_FIELD(_f)                                                                             \
	lua_pushinteger(L, (lua_Integer)st->_f);                                                   \
	lua_setfield(L, -2, #_f)
	STAT_FIELD(passes);
	STAT_FIELD(resumes);
	STAT_FIELD(wakes);
	STAT_FIELD(idle_waits);
	STAT_FIELD(msgs);
	STAT_FIELD(decodes);
	STAT_FIELD(scans);
	STAT_FIELD(cyc_hz);
	STAT_FIELD(cyc_total);
	STAT_FIELD(cyc_resume);
	STAT_FIELD(cyc_idle);
	STAT_FIELD(cyc_drain);
	STAT_FIELD(cyc_expire);
	STAT_FIELD(cyc_locks);
#undef STAT_FIELD

	return 1;
}
#endif

static const luaL_Reg handle_methods[] = {
	{"join", l_join},
	{"cancel", l_cancel},
	{"status", l_status},
	{NULL, NULL},
};

static const luaL_Reg chan_methods[] = {
	{"await", l_await},         {"queue_depth", l_queue_depth}, {"dropped", l_dropped},
	{"pub_async", l_pub_async}, {"read_async", l_read_async},   {NULL, NULL},
};

static const luaL_Reg module_funcs[] = {
	{"spawn", l_spawn}, {"sleep", l_sleep}, {"yield", l_yield},
#ifdef CONFIG_LUA_ASYNC_STATS
	{"stats", l_stats},
#endif
	{NULL, NULL},
};

static int luaz_async_open(lua_State *L)
{
	(void)anchor_get(L);

	if (luaL_newmetatable(L, HANDLE_MT)) {
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
		lua_rawgetp(L, LUA_REGISTRYINDEX, &anchor_key);
		luaL_setfuncs(L, handle_methods, 1);
	}
	lua_pop(L, 1);

	if (luaL_newmetatable(L, ZBUS_CHAN_METATABLE)) {
		/* luaopen_zbus() adds the other methods later. */
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
	}
	lua_rawgetp(L, LUA_REGISTRYINDEX, &anchor_key);
	lua_pushvalue(L, -2);
	luaL_setfuncs(L, chan_methods, 2);
	lua_pop(L, 1);

	luaL_newlibtable(L, module_funcs);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &anchor_key);
	luaL_setfuncs(L, module_funcs, 1);

	return 1;
}

void luaz_async_warn_blocking(lua_State *L, const char *fn)
{
	lua_Debug ar;

	if (get_rx(L) == NULL) {
		return;
	}
	if (!lua_getstack(L, 1, &ar) || !lua_getinfo(L, "Sl", &ar)) {
		return;
	}

	lua_rawgetp(L, LUA_REGISTRYINDEX, &warned_key);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &warned_key);
	}

	lua_pushfstring(L, "%s@%s:%d", fn, ar.short_src, ar.currentline);
	lua_pushvalue(L, -1);
	if (lua_rawget(L, -3) != LUA_TNIL) {
		lua_pop(L, 3);
		return;
	}
	lua_pop(L, 1);
	lua_pushboolean(L, 1);
	lua_rawset(L, -3);
	lua_pop(L, 1);

	LOG_WRN("%s blocks every async task (%s:%d)", fn, ar.short_src, ar.currentline);
}

static bool unwatch_now(const struct zbus_channel *chan, void *sub)
{
	(void)zbus_chan_rm_obs(chan, sub, K_NO_WAIT);

	return true;
}

int luaz_async_run(lua_State *L, const struct luaz_async_ctx *ctx, int n)
{
	int fn_idx = lua_gettop(L) - n + 1;
	size_t buf_size = luaz_zbus_max_msg_size();
	const struct zbus_observer *sub = ctx->sub;
	const struct zbus_channel *chan;
	struct loop *outer;
	struct rx *c;
	int ret = 0;

	/* No protected call yet, so this can't raise: rx, preload table, l_run. */
	if (lua_checkstack(L, 4) == 0) {
		lua_settop(L, fn_idx - 1);
		return -ENOMEM;
	}
	outer = get_loop(L);
#if defined(CONFIG_LUA_ASYNC_STATS) && defined(CONFIG_TIMING_FUNCTIONS)
	timing_init();
	timing_start();
#endif

	c = lua_newuserdatauv(L, sizeof(*c) + buf_size, 0);
	c->ctx = ctx;
	c->buf_size = buf_size;
	lua_rawsetp(L, LUA_REGISTRYINDEX, &rx_key);

	/* Registered here, so only async threads can require it. */
	luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
	lua_pushcfunction(L, luaz_async_open);
	lua_setfield(L, -2, "zephyr.async");
	lua_pop(L, 1);

	/* Registrations left by a run that returned -EBUSY. */
	zbus_iterate_over_channels_with_user_data(unwatch_now, (void *)sub);
	(void)zbus_obs_set_enable(sub, true);

	lua_pushcfunction(L, l_run);
	lua_insert(L, fn_idx); /* l_run, f1 ... fn: the call consumes the functions */
	if (lua_pcall(L, n, 1, 0) != LUA_OK) {
		struct loop *lp = get_loop(L);

		printk("[%s] async: %s\n", k_thread_name_get(k_current_get()), lua_tostring(L, -1));
		/* l_run did not get to unwatch its channels or retire every task. */
		(void)zbus_obs_set_enable(sub, false);
		zbus_iterate_over_channels_with_user_data(unwatch_now, (void *)sub);
		if (lp != NULL && lp != outer) {
			for (int s = 0; s < MAX_TASKS; s++) {
				free_readers(lp, &lp->tasks[s]);
			}
			lua_pushnil(L);
			lua_rawsetp(L, LUA_REGISTRYINDEX, &loop_key);
			if (anchor_find(L) != NULL) {
				anchor_find(L)->lp = NULL;
			}
		}
		ret = -EFAULT;
	} else {
		ret = (int)lua_tointeger(L, -1); /* faults are already reported */
	}

	/* Leave the subscriber clean for the next run, even after a fault. */
	while (zbus_sub_wait_msg(sub, &chan, c->buf, K_NO_WAIT) == 0) {
	}
	lua_pushnil(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &rx_key);

	lua_settop(L, fn_idx - 1);
	__ASSERT(outer != NULL || k_mem_slab_num_used_get(ctx->readers) == 0,
		 "async: readers left in the slab");

	return ret;
}

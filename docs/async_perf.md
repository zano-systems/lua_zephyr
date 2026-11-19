# zephyr.async performance

How the cooperative scheduler (`src/luaz_async.c`) is measured, where its time
goes, and what the optimizations on `perf/async-scheduler` bought.

## Tools

| Tool | What it gives | How |
|---|---|---|
| `tests/async_bench` | Lua-thread cycles/op, allocs/op, bytes/op, heap peak per workload, for incremental and generational GC | `just bench-device` (board in `hardware_map.yaml`) or `just bench mps2/an385` |
| `CONFIG_LUA_ASYNC_STATS` | Scheduler counters and per-phase cycle split (resume / drain / expire / locks / idle) | the `lua_zephyr.async_bench.breakdown` scenario; `async.stats()` in Lua |
| `CONFIG_LUA_ALLOC_STATS` | Allocator calls, requested bytes, live/peak bytes | `luaz_alloc_stats_get()` |
| `scripts/bench_table.py` | Markdown table of one run, or before/after deltas of two | `bench_table.py base.log new.log` |
| `scripts/pc_sample.py` | Statistical function-level profile over J-Link (PC + LR samples mapped to ELF symbols) | build `tests/async_bench` with `CONFIG_BENCH_PROFILE="<case>"`, flash, run the script |
| zview | Thread CPU shares, Lua heap use and peak (each Lua heap is a `k_heap` named `<thread>_lua_heap`), stack watermarks, slab peaks, without instrumentation | `zview dump -e zephyr.elf -r jlink -t NRF52833_XXAA --json` |

Cycle counts are only meaningful on hardware: the nRF52833 numbers use the DWT
cycle counter (`CONFIG_TIMING_FUNCTIONS` and
`CONFIG_THREAD_RUNTIME_STATS_USE_TIMING_FUNCTIONS`, from
`tests/async_bench/boards/nrf52833dk_nrf52833.conf`). QEMU and native_sim
still give exact allocation and scan counts. `cyc` is the Lua thread's own
execution time, so idle time and the zbus publisher thread are left out.

## Results (nRF52833, 64 MHz, cycles/op)

Baseline is `origin/feat/async-coroutine-scheduler` (bd871df), with the
instrumentation added and disabled.

| bench | baseline | now | Δ | allocs/op |
|---|---:|---:|---:|---|
| yield/2 | 2694 | 1260 | −53% | 0.00 → 0.00 |
| yield/8 | 2281 | 1224 | −46% | 0.03 → 0.01 |
| yield/32 | 2288 | 1290 | −44% | 0.11 → 0.06 |
| sleep/16 | 5385 | 3402 | −37% | 0.71 → 0.41 |
| await_fanout/1 (per delivery) | 28996 | 12742 | −56% | 5.12 → 2.06 |
| await_fanout/4 | 23919 | 10631 | −56% | 4.30 → 2.03 |
| await_fanout/8 | 22405 | 10616 | −53% | 4.17 → 2.02 |
| await_multi/4 | 31035 | 13565 | −56% | 5.17 → 2.06 |
| pub_async | 9076 | 7926 | −13% | 1.02 → 1.01 |
| read_async | 17267 | 9214 | −47% | 5.02 → 3.01 |
| spawn_join | 23825 | 22801 | −4% | 7.05 → 7.04 |

Incremental GC. Generational GC gives the same cycle counts within a few
percent.

### What each change bought

| Commit | Main effect |
|---|---|
| `perf(msg_descr): presize decoded tables` | Decoding a 4-field message rehashed its table 3 times: read_async −44%, awaits −22 to −31% |
| `perf(async): keep the channel ring in a C buffer` | No Lua string or seq-keyed table per message: awaits −15 to −20%, −1 alloc/delivery |
| `perf(async): reach the scheduler through an upvalue` | No registry hash lookups in bindings: yield −28 to −34%, awaits −10 to −12% |
| `perf(async): skip idle scheduler phases` | No lock scans, uptime reads or FIFO locks when idle: yield/2 −18% |
| `perf(async): resolve channels against an upvalue metatable` | No `luaL_testudata` name lookup, 2 instead of 3 per await: awaits −11 to −14% |
| `perf(async): pass park requests through the task record` | No tag/kind/args on the coroutine stack: yield −19 to −22%, sleep −18% |
| `perf(async): keep one lazily re-armed idle timer` | No kernel timeout added and aborted per idle wait: sleep −5%, timed awaits −1 to −3% |

## Where the time goes now

Phase split (`.breakdown` scenario, share of the Lua thread's busy time):

| bench | in task code (resume) | scheduler | of which drain |
|---|---:|---:|---:|
| yield/8 | 83% | 17% | 1% |
| sleep/16 | 62% | 38% | 1% (expire 12%) |
| await_fanout/1 | 37% | 63% | 54% |
| await_fanout/8 | 37% | 63% | 58% |

- **yield**: most of what's left is Lua's own resume/yield machinery
  (`luaV_execute`, `resume`, `precallC`, `longjmp`), not the scheduler.
- **await** (PC samples, `await_fanout/1`):
  - about 12% in `sys_heap` alloc/free;
  - about 11% in kernel timeouts: the awaits carry a timeout, so every idle
    `k_poll` arms and aborts a timer and reads the RTC;
  - about 6% building the decoded table.
  - The scheduler's own linear scan (`parked_on`) is 1.5%.
- **zview** (steady-state `await_fanout/1`): Lua thread 76% CPU, publisher 24%
  (`zbus_chan_pub` + net_buf), idle 0%. Stack watermark 2.9 KB of 8 KB.
  `bench_lua_heap` showed 43 KB in use and a 46.7 KB peak out of 64 KB.

## Configuration notes

- **Generational GC** (`collectgarbage("generational")`) cuts the heap peak of
  the await workloads about 4× (35 KB → 8–11 KB), at the same cycle cost. It
  suits threads that decode many short-lived messages.
- **`ZBUS_MSG_SUBSCRIBER_BUF_ALLOC_STATIC`** vs. the dynamic default: no
  measurable change on the Lua side (<1%). The per-publish allocation lands on
  the publisher thread.
- **Timed awaits** re-arm the idle timer about once per timeout period. A
  `K_FOREVER` await never arms it.
- `CONFIG_LUA_ASYNC_MAX_TASKS` sizes the remaining linear scans (`parked_on`,
  slot allocation). Keep it near the real task count.

## Remaining opportunities (not done)

- **Per-channel wait lists** in place of `parked_on()`: worth ~1.5% at 32
  task slots, and grows with `MAX_TASKS`.
- **One decoded table shared by all waiters** of a message: fan-out would
  decode once instead of K times. This changes semantics, since a waiter that
  mutates the table would affect the others.

## Reproducing

```sh
just bench-device                      # BENCH tables for the board
python3 scripts/bench_table.py old.log new.log
west build -b nrf52833dk/nrf52833 tests/async_bench -- '-DCONFIG_BENCH_PROFILE="await_fanout/1"'
west flash && python3 scripts/pc_sample.py -e build/zephyr/zephyr.elf -n 3000
```

native_sim functional runs work on macOS through the
`ghcr.io/zephyrproject-rtos/zephyr-build` image (`native_sim/native/64`).
There, `lua_zephyr.async.many` and `lua_zephyr.async_files.max_tasks` run out
of heap on the baseline as well: their heaps are sized for 32-bit.

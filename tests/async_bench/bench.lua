-- zephyr.async benchmarks. Each bench.measure() call runs under a fresh
-- scheduler and prints a BENCH line (see src/benchlib.c). The whole set runs
-- once per collector mode.

string = require("string")
table = require("table")

local bench = require("bench")
local zbus = require("zephyr").zbus

bench.run(function() end) -- installs the zephyr.async preload
local async = require("zephyr.async")

local chans = {
	zbus.channel_declare("b_c0"),
	zbus.channel_declare("b_c1"),
	zbus.channel_declare("b_c2"),
	zbus.channel_declare("b_c3"),
}
local NAMES = { "b_c0", "b_c1", "b_c2", "b_c3" }

-- Every task must finish its work. The scheduler reports a task's error and
-- keeps running, so a workload that raises midway would end early and look
-- fast; wrap each task to record that it got to the end.
local function checked(measure)
	return function(name, ops, ...)
		local fns, done = { ... }, 0
		for i, fn in ipairs({ ... }) do
			fns[i] = function(...)
				fn(...)
				done = done + 1
			end
		end
		local rc, cyc = measure(name, ops, table.unpack(fns))
		return rc, cyc, done == #fns
	end
end

local measure_checked = checked(bench.measure)
bench.measure = function(name, ops, ...)
	local rc, cyc, complete = measure_checked(name, ops, ...)
	if not complete then
		bench.fail(name .. ": a task raised before finishing its work")
	end
	return rc, cyc
end

-- Self-test: a task that raises midway must be caught by the check above.
do
	local _, _, complete = measure_checked("selftest/raise", 2, function()
		async.yield()
		error("selftest: raised on purpose")
	end, function() end)
	if complete then
		bench.fail("selftest: a raising task was not detected")
	end
end

-- CONFIG_BENCH_ONLY runs one case once; CONFIG_BENCH_PROFILE runs one case
-- forever, without output, for PC sampling.
local PROFILE = bench.profile
local ONLY = PROFILE ~= "" and PROFILE or bench.only
if ONLY ~= "" then
	local measure = bench.measure
	bench.measure = function(name, ...)
		if name == ONLY then
			return measure(name, ...)
		end
	end
end

-- Board baseline (baselines/<board>.lua): a case slower than baseline +
-- CONFIG_BENCH_MARGIN_PCT fails the run. Not with stats, nor when profiling.
local BASE = bench.baseline and PROFILE == "" and not bench.stats and load(bench.baseline)()
if BASE then
	local measure, set_gc = bench.measure, bench.gc
	local gc_mode = "inc"
	bench.gc = function(mode)
		gc_mode = mode
		return set_gc(mode)
	end
	bench.measure = function(name, ...)
		local rc, cyc = measure(name, ...)
		local base = BASE[name] and BASE[name][gc_mode]
		if cyc and base then
			local pct = (cyc - base) * 100 // base
			if cyc * 100 > base * (100 + bench.margin) then
				bench.fail(string.format("%s/%s regressed: %d cyc/op, baseline %d (%+d%%)",
					name, gc_mode, cyc, base, pct))
			elseif cyc * 100 < base * (100 - bench.margin) then
				print(string.format("BENCH NOTE %s/%s: %d cyc/op, baseline %d (%+d%%), refresh the baseline",
					name, gc_mode, cyc, base, pct))
			end
		end
		return rc, cyc
	end
end

-- Context switches: ntasks round-robin through async.yield().
local function yield_bench(ntasks, iters)
	local fns = {}
	for i = 1, ntasks do
		fns[i] = function()
			for _ = 1, iters do
				async.yield()
			end
		end
	end
	bench.measure("yield/" .. ntasks, ntasks * iters, table.unpack(fns))
end

-- Timers: ntasks sleeping 1..4 ms; cyc leaves the idle time out.
local function sleep_bench(ntasks, rounds)
	local fns = {}
	for i = 1, ntasks do
		fns[i] = function()
			for _ = 1, rounds do
				async.sleep(1 + (i % 4))
			end
		end
	end
	bench.measure("sleep/" .. ntasks, ntasks * rounds, table.unpack(fns))
end

-- zbus fan-out: k tasks await every message on one channel. ops = deliveries.
-- The publisher thread has a lower priority, so each message is consumed by
-- all awaiters before the next one is published (no drops expected).
local function await_fanout(k, nmsg)
	local lost = 0
	local fns = {}
	for i = 1, k do
		fns[i] = function()
			for n = 1, nmsg do
				local m = chans[1]:await(1000)
				if m == nil or m.seq ~= n then
					lost = lost + 1
					return
				end
			end
		end
	end
	fns[k + 1] = function()
		bench.publish(nmsg, NAMES[1])
	end
	bench.measure("await_fanout/" .. k, k * nmsg, table.unpack(fns))
	if lost > 0 then
		bench.fail("await_fanout/" .. k .. ": " .. lost .. " awaiters lost messages")
	end
end

-- zbus multi-channel: one awaiter per channel, publisher round-robin.
local function await_multi(nchans, nmsg)
	local lost = 0
	local fns = {}
	local per = nmsg // nchans
	for i = 1, nchans do
		fns[i] = function()
			for _ = 1, per do
				if chans[i]:await(1000) == nil then
					lost = lost + 1
					return
				end
			end
		end
	end
	fns[nchans + 1] = function()
		bench.publish(per * nchans, table.unpack(NAMES, 1, nchans))
	end
	bench.measure("await_multi/" .. nchans, per * nchans, table.unpack(fns))
	if lost > 0 then
		bench.fail("await_multi/" .. nchans .. ": " .. lost .. " awaiters lost messages")
	end
end

-- Lock path without contention: pub_async / read_async on an unwatched channel.
-- Every result is checked (one comparison per op), so a fast failure cannot
-- pass for a speedup.
local function lock_bench(n)
	local ch = chans[4]
	local bad = 0
	bench.measure("pub_async", n, function()
		local msg = { seq = 0, a = 1, b = 2, c = 3 }
		for i = 1, n do
			msg.seq = i
			if ch:pub_async(msg) ~= true then
				bad = bad + 1
			end
		end
	end)
	if bad > 0 then
		bench.fail("pub_async: " .. bad .. " publishes failed")
	end
	-- Seeded outside the measured run, so the case does not depend on pub_async
	-- (which CONFIG_BENCH_ONLY / CONFIG_BENCH_PROFILE may skip).
	local SEED = 4242
	if ch:pub({ seq = SEED, a = 1, b = 2, c = 3 }, 100) ~= 0 then
		bench.fail("read_async: could not seed the channel")
	end
	bad = 0
	bench.measure("read_async", n, function()
		for _ = 1, n do
			local m = ch:read_async()
			if m == nil or m.seq ~= SEED then
				bad = bad + 1
			end
		end
	end)
	if bad > 0 then
		bench.fail("read_async: " .. bad .. " reads failed or returned the wrong message")
	end
end

-- Task churn: spawn + join one short task at a time.
local function spawn_bench(n)
	local bad = 0
	bench.measure("spawn_join", n, function()
		local id = function(x) return x end
		for i = 1, n do
			local ok, v = async.spawn(id, i):join()
			if ok ~= true or v ~= i then
				bad = bad + 1
			end
		end
	end)
	if bad > 0 then
		bench.fail("spawn_join: " .. bad .. " joins failed")
	end
end

repeat
	for _, mode in ipairs({ "inc", "gen" }) do
		bench.gc(mode)
		yield_bench(2, 1000)
		yield_bench(8, 250)
		yield_bench(32, 64)
		sleep_bench(16, 10)
		await_fanout(1, 200)
		await_fanout(4, 200)
		await_fanout(8, 200)
		await_multi(4, 400)
		lock_bench(500)
		spawn_bench(200)
	end
until PROFILE == ""

bench.done()

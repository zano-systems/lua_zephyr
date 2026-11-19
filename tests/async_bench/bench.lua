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
local function lock_bench(n)
	local ch = chans[4]
	bench.measure("pub_async", n, function()
		local msg = { seq = 0, a = 1, b = 2, c = 3 }
		for i = 1, n do
			msg.seq = i
			ch:pub_async(msg)
		end
	end)
	bench.measure("read_async", n, function()
		for _ = 1, n do
			ch:read_async()
		end
	end)
end

-- Task churn: spawn + join one short task at a time.
local function spawn_bench(n)
	bench.measure("spawn_join", n, function()
		local id = function(x) return x end
		for i = 1, n do
			async.spawn(id, i):join()
		end
	end)
end

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

bench.done()

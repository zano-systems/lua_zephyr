-- coroutine library untouched, channel limit, handle lifetime, claimed channels.

local z = require("zephyr")
local lib = require("testlib")

return {
	{ "the coroutine library is not patched", function(T)
		local async, F = T.start()
		T.eq(debug.getinfo(coroutine.resume).what, "C", "coroutine.resume untouched")
		local ch = F.channel("c")
		local log = {}
		async._run(function()
			local h = async.spawn(function()
				local m = ch:await(100)
				log[#log + 1] = m and m.v
				return "joined"
			end)
			async.sleep(10)
			F.publish("c", { v = "msg" })
			local r = select(2, h:join()) -- park before indexing `log`
			log[#log + 1] = r
		end)
		T.same(log, { "msg", "joined" })
	end },

	{ "awaiting more than MAX_CHANNELS channels raises ENOMEM", function(T)
		local async, F = T.start({ max_chans = 2 })
		local err
		async._run(function()
			F.channel("a"):await(0)
			F.channel("b"):await(0)
			local ok, e = pcall(function() return F.channel("c"):await(0) end)
			err = e
		end)
		T.matches(err, "cannot watch 't_c' %(%-12%)")
	end },

	{ "watch failure raises with errno", function(T)
		local async, F = T.start({ node_pool = 1 })
		local err
		async._run(function()
			F.channel("a"):await(0) -- takes the only observer node
			local ok, e = pcall(function() return F.channel("b"):await(0) end)
			err = e
		end)
		T.matches(err, "cannot watch 't_b' %(%-%d+%)")
	end },

	{ "observer nodes are released when the scheduler exits", function(T)
		local async, F = T.start({ node_pool = 1 })
		local ok, e
		async._run(function()
			ok, e = pcall(function() return F.channel("b"):await(0) end)
		end)
		T.ok(ok, "watch succeeds in a fresh run: " .. tostring(e))
	end },

	{ "a handle stays valid after its slot is reused", function(T)
		local async = T.start()
		local first_status, first_result, second_result
		async._run(function()
			local h1 = async.spawn(function() return "one" end)
			first_result = select(2, h1:join())
			local h2 = async.spawn(function() async.sleep(5); return "two" end) -- reuses the slot
			first_status = h1:status()
			T.eq(select(2, h1:join()), "one", "old handle still answers")
			T.eq(h1:cancel(), false, "cancel on old handle is a no-op")
			second_result = select(2, h2:join())
		end)
		T.eq(first_result, "one")
		T.eq(first_status, "done")
		T.eq(second_result, "two")
	end },

	{ "handles outlive the run", function(T)
		local async = T.start()
		local h
		async._run(function() h = async.spawn(function() return 7 end) end)
		T.eq(h:status(), "done")
		T.eq(h:cancel(), false)
	end },

	{ "reply values become the parking call's results (no tables needed)", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local n
		async._run(function()
			async.spawn(function()
				n = select("#", ch:await(1000))
			end)
			async.yield()
			F.publish("c", { v = 1 })
		end)
		T.eq(n, 1, "await returns exactly one value on success")
	end },

	{ "registering on a claimed channel parks only the awaiting task", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local first_tick, got
		async._run(function()
			F.hold("c", 30) -- the system workqueue holds the lock for 30 ms
			async.spawn(function()
				async.sleep(5)
				first_tick = F.now
			end)
			local w = async.spawn(function() got = ch:await(1000).v end)
			async.sleep(40)
			F.publish("c", { v = "after" })
			w:join()
		end)
		T.eq(first_tick, 5, "other task ran while the lock was held")
		T.eq(got, "after")
	end },

	{ "registering on a claimed channel gives up at the await timeout", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local r1, r2, at
		async._run(function()
			F.hold("c", 50)
			r1, r2 = ch:await(10)
			at = F.now
			async.sleep(45) -- past the hold
		end)
		T.eq(r1, nil); T.eq(r2, -z.EAGAIN); T.eq(at, 10)
	end },

	{ "teardown waits for a briefly claimed channel and unwatches it", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local ok = async._run(function()
			ch:await(0)
			F.hold("c", 30) -- still held when the run ends
		end)
		T.ok(ok, "run returned 0")
		T.ok(F.now >= 30 and F.now < 100, "teardown waited " .. F.now .. " ms")
		T.eq(lib.watching("t_c"), false)
	end },

	{ "a channel claimed past teardown stays watched but receives nothing", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local ok = async._run(function()
			ch:await(0)
			F.claim("c") -- never released during teardown
		end)
		local waited = F.now
		T.eq(ok, false, "run returned -EBUSY")
		T.ok(waited >= 100 and waited < 150, "teardown gave up after " .. waited .. " ms")
		T.eq(lib.watching("t_c"), true)
		T.matches(F.reports[1], "'t_c' is still claimed, left watched but disabled")

		F.finish("c")
		F.publish("c", { v = "unseen" })
		T.eq(lib.queued(), false, "disabled subscriber queued nothing")

		local got
		ok = async._run(function()
			local h = async.spawn(function() got = ch:await(100).v end)
			async.yield()
			F.publish("c", { v = "next run" })
			h:join()
		end)
		T.ok(ok, "next run returned 0")
		T.eq(got, "next run")
		T.eq(lib.watching("t_c"), false)
	end },
}

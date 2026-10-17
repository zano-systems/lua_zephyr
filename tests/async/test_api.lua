-- Return conventions, argument checks, where calls are allowed.

return {
	{ "embedded sources keep backslash escapes", function(T)
		T.start()
		T.eq(#"a\nb", 3)
		T.eq("\t", string.char(9))
		T.eq("\\", string.char(92))
		T.eq("\"", string.char(34))
	end },

	{ "parking inside a coroutine raises", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local errs = {}
		async._run(function()
			local h = async.spawn(function() async.sleep(50) end)
			for _, park in ipairs({
				function() async.sleep(1) end,
				function() async.yield() end,
				function() ch:await(10) end,
				function() h:join() end,
			}) do
				local _, e = pcall(coroutine.wrap(park))
				errs[#errs + 1] = e
			end
		end)
		T.eq(#errs, 4)
		for i = 1, 4 do
			T.matches(errs[i], "cannot park inside a coroutine", "call " .. i)
		end
	end },

	{ "calls that don't park work inside a coroutine", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local ok, v, dropped
		async._run(function()
			local h = async.spawn(function() return 7 end)
			async.yield()
			ok, v = coroutine.wrap(function() return h:join() end)()
			dropped = coroutine.wrap(function() return ch:dropped() end)()
		end)
		T.eq(ok, true)
		T.eq(v, 7)
		T.eq(dropped, 0)
	end },

	{ "a raw coroutine.yield in a task fails it", function(T)
		local async = T.start()
		local ok, err
		async._run(function()
			local h = async.spawn(function() coroutine.yield(1) end)
			ok, err = h:join()
		end)
		T.eq(ok, false)
		T.matches(err, "untagged yield reached the scheduler")
	end },

	{ "parking calls outside the scheduler raise", function(T)
		local async = T.start()
		T.raises(function() async.sleep(1) end, "not running under the scheduler")
		T.raises(function() async.yield() end, "not running under the scheduler")
		T.raises(function() async.spawn(function() end) end, "not running under the scheduler")
	end },

	{ "zephyr.K_FOREVER and zephyr.K_NO_WAIT as timeouts", function(T)
		local async, F = T.start()
		local z = require("zephyr")
		local ch = F.channel("c")
		local r1, r2, joined, sleep_err
		async._run(function()
			r1, r2 = ch:await(z.K_NO_WAIT)
			local h = async.spawn(function() async.sleep(5); return "v" end)
			joined = select(2, h:join(z.K_FOREVER))
			sleep_err = select(2, pcall(async.sleep, z.K_FOREVER))
		end)
		T.eq(r1, nil); T.eq(r2, -z.EAGAIN)
		T.eq(joined, "v")
		T.matches(sleep_err, "timeout must be")
		T.raises(function() z.msleep(z.K_FOREVER) end, "ms must be >= 0")
		T.raises(function() ch:pub({ v = "x" }, -2) end, "timeout must be zephyr.K_FOREVER")
	end },

	{ "timeouts must be integers in range", function(T)
		local async = T.start()
		local errs = {}
		async._run(function()
			for _, bad in ipairs({ -1, 1.5, "10", 0x7FFFFFFF + 0.0 + 1e3 }) do
				local ok, e = pcall(async.sleep, bad)
				errs[#errs + 1] = (not ok) and e:match("timeout must be") or "accepted"
			end
			local ok, e = pcall(async.sleep, nil)
			errs[#errs + 1] = (not ok) and e:match("timeout must be") or "accepted"
			T.ok(pcall(async.sleep, 2.0), "integral float accepted")
		end)
		T.same(errs, { "timeout must be", "timeout must be", "timeout must be",
			"timeout must be", "timeout must be" })
	end },

	{ "argument errors point at the caller", function(T)
		local async = T.start()
		local err
		async._run(function()
			local ok, e = pcall(function()
				async.sleep(-5) -- this line
			end)
			err = e
		end)
		T.matches(err, "test_api%.lua:%d+:")
	end },

	{ "spawn passes arguments and validates fn", function(T)
		local async = T.start()
		local got
		async._run(function()
			async.spawn(function(a, b, c) got = { a, b, tostring(c) } end, 1, "two", nil)
			T.raises(function() async.spawn(42) end, "function expected, got number")
		end)
		T.same(got, { "1", "two", "nil" })
	end },

	{ "spawn grows the task's stack for many arguments", function(T)
		local async = T.start()
		local n
		local args = {}
		for i = 1, 100 do
			args[i] = i
		end
		async._run(function()
			async.spawn(function(...) n = select("#", ...) end, table.unpack(args))
		end)
		T.eq(n, 100)
	end },

	{ "spawn and cancel never park", function(T)
		local async = T.start()
		local log = {}
		async._run(function()
			local h = async.spawn(function() log[#log + 1] = "child" end)
			log[#log + 1] = "a"
			h:cancel()
			log[#log + 1] = "b"
		end)
		T.same(log, { "a", "b" })
	end },

	{ "status values over a task's life", function(T)
		local async = T.start()
		local seen = {}
		local h_self
		async._run(function()
			local sleeper = async.spawn(function() async.sleep(10) end)
			seen[#seen + 1] = sleeper:status() -- ready (not started)
			async.yield()
			seen[#seen + 1] = sleeper:status() -- sleeping
			h_self = async.spawn(function() seen[#seen + 1] = h_self:status() end)
			async.sleep(20)
			seen[#seen + 1] = sleeper:status() -- done
			local bad = async.spawn(function() error("x") end)
			async.yield()
			seen[#seen + 1] = bad:status()
		end)
		T.same(seen, { "ready", "sleeping", "running", "done", "failed" })
	end },
}

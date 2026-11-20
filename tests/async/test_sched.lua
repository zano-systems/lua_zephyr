-- Passes, fairness, timing, lifetime.

return {
	{ "three workers interleave by deadline", function(T)
		local async, F = T.start()
		local log = {}
		local function worker(name, interval, rounds)
			for i = 1, rounds do
				log[#log + 1] = name .. i .. "@" .. F.now
				async.sleep(interval)
			end
			log[#log + 1] = name .. "-done@" .. F.now
		end
		T.ok(async._run(function()
			async.spawn(worker, "a", 100, 3)
			async.spawn(worker, "b", 150, 3)
			async.spawn(worker, "c", 200, 3)
		end))
		T.same(log, {
			"a1@0", "b1@0", "c1@0",
			"a2@100", "b2@150",
			"c2@200", "a3@200", -- same deadline: c parked first (t=0 vs t=100)
			"b3@300", "a-done@300", -- same deadline: b parked first (t=150 vs t=200)
			"c3@400", "b-done@450", "c-done@600",
		})
	end },

	{ "always-ready tasks run once per pass, FIFO", function(T)
		local async = T.start()
		local log = {}
		local function spinner(name)
			for _ = 1, 3 do
				log[#log + 1] = name
				async.yield()
			end
		end
		async._run(function()
			async.spawn(spinner, "a")
			async.spawn(spinner, "b")
			async.spawn(spinner, "c")
		end)
		T.same(log, { "a", "b", "c", "a", "b", "c", "a", "b", "c" })
	end },

	{ "spawned task never runs inside spawn or in the same pass", function(T)
		local async = T.start()
		local log = {}
		async._run(function()
			async.spawn(function() log[#log + 1] = "child" end)
			log[#log + 1] = "parent-after-spawn"
		end)
		T.same(log, { "parent-after-spawn", "child" })
	end },

	{ "sleep resumes no earlier than ms, even with CPU time spent", function(T)
		local async, F = T.start()
		local woke
		async._run(function()
			local t0 = F.now
			async.spawn(function()
				F.advance(30) -- another task burns 30 ms
			end)
			async.sleep(20)
			woke = F.now - t0
		end)
		T.ok(woke >= 30, "woke after the 30 ms busy task (got " .. woke .. ")")
	end },

	{ "sleep(0) behaves as yield", function(T)
		local async = T.start()
		local log = {}
		async._run(function()
			async.spawn(function() log[#log + 1] = "other" end)
			async.sleep(0)
			log[#log + 1] = "back"
		end)
		T.same(log, { "other", "back" })
	end },

	{ "equal deadlines wake in park order", function(T)
		local async = T.start()
		local log = {}
		async._run(function()
			for _, n in ipairs({ "x", "y", "z" }) do
				async.spawn(function()
					async.sleep(50)
					log[#log + 1] = n
				end)
			end
		end)
		T.same(log, { "x", "y", "z" })
	end },

	{ "flood hitting the drain limit does not starve tasks", function(T)
		local async, F = T.start({ max_drain = 2 })
		local ch = F.channel("flood")
		local ticks, got = 0, 0
		async._run(function()
			async.spawn(function()
				while got < 6 do
					local m = ch:await(1000)
					if m then
						got = got + 1
					end
				end
			end)
			async.spawn(function()
				for _ = 1, 3 do
					ticks = ticks + 1
					async.yield()
				end
			end)
			async.yield() -- let the awaiter watch the channel
			for i = 1, 6 do
				F.publish("flood", { i = i })
			end
		end)
		T.eq(ticks, 3, "spinner ran")
		T.eq(got, 6, "awaiter saw every message")
	end },

	{ "loop runs until all tasks finish, not just task-0", function(T)
		local async, F = T.start()
		local done = false
		local ok = async._run(function()
			async.spawn(function()
				async.sleep(500)
				done = true
			end)
		end)
		T.ok(ok)
		T.ok(done, "worker finished after task-0 returned")
		T.eq(F.now, 500, "virtual time")
	end },

	{ "task limit includes task-0", function(T)
		local async = T.start()
		local err
		async._run(function()
			for _ = 2, T.config.max_tasks do
				async.spawn(function() end)
			end
			local ok, e = pcall(async.spawn, function() end)
			err = (not ok) and e
		end)
		T.matches(err, "task limit reached")
	end },

	{ "the idle timer is registered once across runs", function(T)
		local async, F = T.start({ obj_core = 1 })
		for _ = 1, 5 do
			async._run(function() async.sleep(2) end)
		end
		T.eq(F.timer_links(), 1, "registrations of the idle timer")
	end },

	{ "an earlier deadline arriving while the idle timer is armed wakes first", function(T)
		local async, F = T.start()
		local log = {}
		async._run(function()
			async.spawn(function() async.sleep(50); log[#log + 1] = { "long", F.now } end)
			async.yield() -- the idle timer is armed for 50 ms
			async.spawn(function() async.sleep(10); log[#log + 1] = { "short", F.now } end)
		end)
		T.eq(log[1][1], "short")
		T.ok(log[1][2] >= 10, "short did not wake early")
		T.eq(log[2][1], "long")
		T.ok(log[2][2] >= 50, "long did not wake early")
	end },

	{ "cancelling the earliest sleeper wakes no one early", function(T)
		local async, F = T.start()
		local woke
		async._run(function()
			local early = async.spawn(function() async.sleep(10) end)
			async.spawn(function() async.sleep(30); woke = F.now end)
			async.yield() -- the idle timer is armed for 10 ms
			early:cancel()
		end)
		T.ok(woke ~= nil and woke >= 30, "late sleeper woke at its own deadline")
	end },

	{ "equal deadlines wake together, in park order", function(T)
		local async, F = T.start()
		local log = {}
		async._run(function()
			for _, name in ipairs({ "a", "b", "c" }) do
				async.spawn(function() async.sleep(10); log[#log + 1] = name .. "@" .. F.now end)
			end
		end)
		T.same(log, { "a@10", "b@10", "c@10" })
	end },
}

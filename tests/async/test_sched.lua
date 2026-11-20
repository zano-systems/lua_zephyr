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
		local woke, at = {}, nil
		async._run(function()
			local ch = F.channel("a")
			async.spawn(function() async.sleep(50); woke.long = F.now end)
			F.publish_later("a", 5)
			-- Every task is parked now, so the idle timer is armed for 50 ms; only
			-- the message from the workqueue wakes this task, at 5 ms.
			ch:await()
			at = F.now
			async.spawn(function() async.sleep(10); woke.short = F.now end)
		end)
		T.ok(at >= 5 and at <= 6, "controller woke at the message (" .. tostring(at) .. ")")
		T.ok(woke.short >= at + 10 and woke.short <= at + 11,
			"the earlier deadline re-armed the timer (woke at " .. tostring(woke.short) .. ")")
		T.eq(woke.long, 50, "the later deadline is unchanged")
	end },

	{ "cancelling the earliest sleeper while the idle timer is armed wakes no one early", function(T)
		local async, F = T.start()
		local woke_early, woke_late, cancelled
		async._run(function()
			local ch = F.channel("a")
			local early = async.spawn(function() async.sleep(10); woke_early = F.now end)
			async.spawn(function() async.sleep(30); woke_late = F.now end)
			F.publish_later("a", 2)
			ch:await() -- all parked: the idle timer is armed for 10 ms
			cancelled = early:cancel()
		end)
		T.eq(cancelled, true)
		T.eq(woke_early, nil, "cancelled sleeper never woke")
		T.eq(woke_late, 30, "late sleeper woke at its own deadline")
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

	{ "a task that exposed its coroutine is never reused for another task", function(T)
		local async = T.start()
		local first, second
		async._run(function()
			async.spawn(function() first = coroutine.running() end):join()
			async.spawn(function() second = coroutine.running() end):join()
		end)
		T.ok(first ~= nil and second ~= nil)
		T.ok(first ~= second, "a stored coroutine.running() never aliases a later task")
		T.eq(coroutine.status(first), "dead")
	end },

	{ "a reused coroutine starts clean after an error, a cancel and a <close>", function(T)
		local async = T.start()
		local closed = {}
		local function closer(name)
			return setmetatable({}, { __close = function() closed[name] = (closed[name] or 0) + 1 end })
		end
		local function fresh(...) -- checks it sees only its own arguments
			local c <close> = closer("fresh")
			return select("#", ...), ...
		end
		local results = {}
		async._run(function()
			-- an error, with a <close> pending
			T.eq(async.spawn(function()
				local c <close> = closer("err")
				error("boom", 0)
			end):join(), false)
			results.after_error = table.pack(async.spawn(fresh, "a", nil):join())
			-- a cancel while parked, with a <close> pending
			local h = async.spawn(function()
				local c <close> = closer("cancel")
				async.sleep(100)
			end)
			async.yield()
			T.eq(h:cancel(), true)
			results.after_cancel = table.pack(async.spawn(fresh, 1, 2, 3):join())
			-- a normal finish through a <close>, and a deep stack
			local function deep(n) if n == 0 then return 0 end return 1 + deep(n - 1) end
			T.eq(select(2, async.spawn(function()
				local c <close> = closer("normal")
				return deep(200)
			end):join()), 200)
			results.after_normal = table.pack(async.spawn(fresh):join())
		end)
		local function values(t)
			local out = {}
			for i = 1, t.n do
				out[i] = tostring(t[i])
			end
			return out
		end
		T.same(values(results.after_error), { "true", "2", "a", "nil" }, "after an error")
		T.same(values(results.after_cancel), { "true", "3", "1", "2", "3" }, "after a cancel")
		T.same(values(results.after_normal), { "true", "0" }, "after a normal finish")
		T.eq(closed.err, 1, "the failed task's <close> ran once")
		T.eq(closed.cancel, 1, "the cancelled task's <close> ran once")
		T.eq(closed.normal, 1, "the finished task's <close> ran once")
		T.eq(closed.fresh, 3, "each new task ran only its own <close>")
	end },

	{ "a coroutine saved by a cancelled or failed task's <close> is never reused", function(T)
		local async = T.start()
		local status = {}
		local function saver(name)
			return setmetatable({}, { __close = function() status[name] = coroutine.running() end })
		end
		-- With the saved coroutine reused, a parked task would make it "suspended".
		local function parked_status(name)
			local next_task = async.spawn(function() async.sleep(20) end)
			async.yield() -- next_task is parked now
			local st = coroutine.status(status[name])
			next_task:join()
			return st
		end
		local results = {}
		async._run(function()
			local victim = async.spawn(function()
				local c <close> = saver("cancelled")
				async.sleep(100)
			end)
			async.yield()
			victim:cancel() -- its <close> runs on its coroutine while this task is current
			results.cancelled = parked_status("cancelled")
			async.spawn(function()
				local c <close> = saver("failed")
				error("x", 0) -- its <close> runs after the task ended, with no task current
			end):join()
			results.failed = parked_status("failed")
		end)
		T.eq(results.cancelled, "dead", "coroutine saved during a cancel")
		T.eq(results.failed, "dead", "coroutine saved during a failure")
	end },

	{ "debug hooks do not cross coroutine reuse", function(T)
		local async = T.start()
		local counts = { a = 0 }
		local leaked, fresh_mask, child_mask
		async._run(function()
			-- A task that ends with a hook still set on its coroutine.
			async.spawn(function()
				debug.sethook(function() counts.a = counts.a + 1 end, "l")
				local x = 1
				x = x + 1
			end):join()
			local before = counts.a
			async.spawn(function()
				local y = 1
				y = y + 1
				y = y + 1
			end):join()
			leaked = counts.a - before
		end)
		-- A new run starts with an empty pool. A spawner with a hook: the new task
		-- must start with the same hook state as a fresh coroutine (the C hook
		-- and mask are inherited).
		async._run(function()
			async.spawn(function() end):join() -- leaves a clean coroutine in the pool
			debug.sethook(function() end, "l")
			local fresh = coroutine.create(function() return select(2, debug.gethook()) end)
			fresh_mask = select(2, coroutine.resume(fresh))
			local child = async.spawn(function() child_mask = select(2, debug.gethook()) end)
			debug.sethook()
			child:join()
		end)
		T.eq(leaked, 0, "a hook set by one task did not fire in the next")
		T.eq(fresh_mask, "l")
		T.eq(child_mask, fresh_mask, "a task spawned under a hook starts like a fresh coroutine")
	end },
}

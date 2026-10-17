-- Reader slab, more than 32 tasks, lock waits beyond MAX_LOCK_WAITS.

local lib = require("testlib")

return {
	{ "each awaited channel takes one reader until its task ends", function(T)
		local async, F = T.start()
		local a, b = F.channel("a"), F.channel("b")
		local n = {}
		async._run(function()
			a:await(0)
			n[#n + 1] = lib.readers()
			a:await(0) -- same channel, same reader
			n[#n + 1] = lib.readers()
			b:await(0)
			n[#n + 1] = lib.readers()
			local h = async.spawn(function() a:await(20) end)
			async.yield() -- h parks on a with its own reader
			n[#n + 1] = lib.readers()
			h:join()
			n[#n + 1] = lib.readers()
		end)
		n[#n + 1] = lib.readers()
		T.same(n, { 1, 1, 2, 3, 2, 0 })
	end },

	{ "cancelled and failed tasks return their readers", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local n = {}
		async._run(function()
			local waiting = async.spawn(function() ch:await() end)
			local failing = async.spawn(function()
				ch:await(0)
				error("x", 0)
			end)
			async.yield()
			n[#n + 1] = lib.readers()
			waiting:cancel()
			failing:join()
			n[#n + 1] = lib.readers()
		end)
		T.same(n, { 1, 0 })
	end },

	{ "await raises when the slab is empty; a finished task frees its readers", function(T)
		local async, F = T.start({ max_readers = 2 })
		local err, later
		async._run(function()
			async.spawn(function()
				F.channel("a"):await(0)
				F.channel("b"):await(0)
				err = select(2, pcall(function() return F.channel("c"):await(0) end))
			end):join()
			async.spawn(function() later = F.channel("c"):await(0) end):join()
		end)
		T.matches(err, "too many readers")
		T.eq(later, nil) -- returned nil, -EAGAIN instead of raising
		T.eq(lib.readers(), 0)
	end },

	{ "more than 32 waiters and joiners wake in park order", function(T)
		local async, F = T.start({ max_tasks = 64 })
		local ch = F.channel("c")
		local woke, joined = {}, {}
		async._run(function()
			local hs = {}
			for i = 1, 63 do
				hs[i] = async.spawn(function()
					local m = ch:await(1000)
					woke[#woke + 1] = i
					return m.i
				end)
			end
			async.yield() -- every task parks on ch
			F.publish("c", { i = 7 })
			for i = 1, 63 do
				T.eq(select(2, hs[i]:join()), 7)
			end

			local target = async.spawn(function() async.sleep(10); return "t" end)
			for i = 1, 62 do
				async.spawn(function()
					local r = select(2, target:join()) -- park before indexing `joined`
					joined[#joined + 1] = i .. r
				end)
			end
			target:join()
		end)
		local want_woke, want_joined = {}, {}
		for i = 1, 63 do want_woke[i] = i end
		for i = 1, 62 do want_joined[i] = i .. "t" end
		T.same(woke, want_woke)
		T.same(joined, want_joined)
	end },

	{ "a lock beyond MAX_LOCK_WAITS is rechecked every tick", function(T)
		local async, F = T.start({ max_locks = 1 })
		local a, b = F.channel("a"), F.channel("b")
		local at
		async._run(function()
			F.claim("a")      -- held by this thread until the end
			F.hold("b", 10)   -- the system workqueue frees it at 10 ms
			async.spawn(function() a:pub_async({ v = "a" }, 1000) end) -- takes the one event
			async.spawn(function()
				T.eq(b:pub_async({ v = "b" }, 1000), true)
				at = F.now
			end)
			async.sleep(50)
			F.finish("a")
		end)
		T.ok(at ~= nil and at >= 10 and at < 50, "b published at " .. tostring(at))
	end },
}

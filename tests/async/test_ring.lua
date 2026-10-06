-- Watching, broadcast ring, cursors, drops.

local z = require("zephyr")

return {
	{ "first await watches; earlier publishes are not seen", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local got
		async._run(function()
			F.publish("c", { v = "before" })
			async.spawn(function()
				local m = ch:await(100)
				got = m and m.v
			end)
			async.yield()
			F.publish("c", { v = "after" })
		end)
		T.eq(got, "after")
	end },

	{ "broadcast to every parked waiter, each its own table", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local tables = {}
		async._run(function()
			for i = 1, 3 do
				async.spawn(function()
					tables[i] = ch:await()
				end)
			end
			async.yield()
			F.publish("c", { v = 1 })
		end)
		T.eq(tables[1].v, "1"); T.eq(tables[2].v, "1"); T.eq(tables[3].v, "1")
		T.ok(tables[1] ~= tables[2] and tables[2] ~= tables[3], "distinct tables")
	end },

	{ "a task that isn't parked still gets messages via its cursor", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local fast, slow = {}, {}
		async._run(function()
			async.spawn(function()
				for _ = 1, 3 do fast[#fast + 1] = ch:await().v end
			end)
			async.spawn(function()
				ch:await(0) -- create cursor, nothing yet
				async.sleep(50) -- busy while messages arrive
				for _ = 1, 3 do slow[#slow + 1] = ch:await(0).v end
			end)
			async.yield()
			for i = 1, 3 do F.publish("c", { v = i }); async.sleep(5) end
		end)
		T.same(fast, { "1", "2", "3" })
		T.same(slow, { "1", "2", "3" })
	end },

	{ "a slow task skips to the oldest held message and counts drops", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local got, dropped = {}, nil
		async._run(function()
			ch:queue_depth(2)
			async.spawn(function()
				ch:await(0)
				async.sleep(50)
				for _ = 1, 2 do got[#got + 1] = ch:await(0).v end
				dropped = ch:dropped()
				T.eq(select(2, ch:await(0)), -z.EAGAIN)
			end)
			async.yield()
			for i = 1, 5 do F.publish("c", { v = i }) end
		end)
		T.same(got, { "4", "5" })
		T.eq(dropped, 3)
	end },

	{ "dropped() includes overwritten messages before the next read", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local before, shrunk, got, after
		async._run(function()
			ch:queue_depth(2)
			async.spawn(function()
				ch:await(0) -- cursor at head
				async.sleep(50) -- 5 arrive; the ring keeps 4 and 5
				before = ch:dropped()
				ch:queue_depth(1) -- discards unread 4
				shrunk = ch:dropped()
				got = ch:await(0).v
				after = ch:dropped()
			end)
			async.yield()
			for i = 1, 5 do F.publish("c", { v = i }) end
		end)
		T.eq(before, 3)
		T.eq(shrunk, 4)
		T.eq(got, "5")
		T.eq(after, 4, "the read does not count the gap again")
	end },

	{ "queue_depth: per-channel override, shrink discards oldest", function(T)
		local async, F = T.start({ depth = 4 })
		local ch = F.channel("c")
		local got, d0, dropped = {}, nil, nil
		async._run(function()
			d0 = ch:queue_depth()
			async.spawn(function()
				ch:await(0)
				async.sleep(50)
				while true do
					local m = ch:await(0)
					if not m then break end
					got[#got + 1] = m.v
				end
				dropped = ch:dropped()
			end)
			async.yield()
			for i = 1, 4 do F.publish("c", { v = i }) end
			F.advance(1)
			async.yield() -- deliver into the ring
			T.eq(ch:queue_depth(1), 1)
		end)
		T.eq(d0, 4)
		T.same(got, { "4" })
		T.eq(dropped, 3)
		T.raises(function() ch:queue_depth(0) end, "depth must be")
	end },

	{ "await timeout and await(0) return nil, timeout", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local a1, a2, b1, b2, waited
		async._run(function()
			local t0 = F.now
			a1, a2 = ch:await(30)
			waited = F.now - t0
			b1, b2 = ch:await(0)
		end)
		T.eq(a1, nil); T.eq(a2, -z.EAGAIN); T.eq(waited, 30)
		T.eq(b1, nil); T.eq(b2, -z.EAGAIN)
	end },

	{ "channel identity uses keys, not userdata", function(T)
		local async, F = T.start()
		local got
		async._run(function()
			async.spawn(function() got = F.channel("c"):await(100) end)
			async.yield()
			F.channel("c"):queue_depth(2) -- another proxy, same channel
			F.publish("c", { v = "k" })
		end)
		T.eq(got.v, "k")
	end },

	{ "await on a channel without a descriptor raises", function(T)
		local async, F = T.start()
		local ch = F.channel("raw", false)
		local err
		async._run(function()
			local ok, e = pcall(ch.await, ch, 10)
			err = e
		end)
		T.matches(err, "channel 't_raw' has no message descriptor")
	end },

	{ "pub_async waits for a held lock without blocking other tasks", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local log, got = {}, nil
		async._run(function()
			async.spawn(function() got = ch:await(1000) end)
			async.yield() -- the receiver watches
			T.eq(F.claim("c"), 0)
			async.spawn(function()
				local ok = ch:pub_async({ v = "late" }, z.K_FOREVER)
				log[#log + 1] = "published@" .. F.now .. ":" .. tostring(ok)
			end)
			async.spawn(function()
				for _ = 1, 3 do
					log[#log + 1] = "tick"
					async.sleep(5)
				end
			end)
			async.sleep(20)
			F.finish("c")
		end)
		T.same(log, { "tick", "tick", "tick", "published@20:true" })
		T.eq(got.v, "late")
	end },

	{ "a lock freed by another thread wakes the idle scheduler", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local ok, at
		async._run(function()
			F.hold("c", 20) -- the system workqueue holds the lock for 20 ms
			ok = ch:pub_async({ v = "x" }, z.K_FOREVER) -- nothing else is scheduled
			at = F.now
		end)
		T.eq(ok, true); T.eq(at, 20)
	end },

	{ "pub_async gives up at its timeout and delivers nothing", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local r1, r2, t, m, merr
		async._run(function()
			local recv = async.spawn(function() return ch:await(50) end)
			async.yield()
			F.claim("c")
			r1, r2 = ch:pub_async({ v = "lost" }, 10)
			t = F.now
			F.finish("c")
			m, merr = select(2, recv:join())
		end)
		T.eq(r1, nil); T.eq(r2, -z.EAGAIN); T.eq(t, 10)
		T.eq(m, nil); T.eq(merr, -z.EAGAIN)
	end },

	{ "K_NO_WAIT on a held lock returns -EBUSY; read_async reads the value", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local b1, b2, r1, r2, cur
		async._run(function()
			T.eq(ch:pub_async({ v = "a" }, z.K_NO_WAIT), true)
			F.claim("c")
			b1, b2 = ch:pub_async({ v = "b" }, z.K_NO_WAIT)
			r1, r2 = ch:read_async(z.K_NO_WAIT)
			F.finish("c")
			cur = ch:read_async().v
		end)
		T.eq(b1, nil); T.eq(b2, -z.EBUSY)
		T.eq(r1, nil); T.eq(r2, -z.EBUSY)
		T.eq(cur, "a")
	end },

	{ "read_async waits for a held lock", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local got, at
		async._run(function()
			T.eq(ch:pub_async({ v = "held" }, z.K_NO_WAIT), true)
			F.claim("c")
			async.spawn(function()
				got = ch:read_async(z.K_FOREVER).v
				at = F.now
			end)
			async.sleep(15)
			F.finish("c")
		end)
		T.eq(got, "held"); T.eq(at, 15)
	end },

	{ "pub_async needs a task, a descriptor, and no user coroutine when it waits", function(T)
		local async, F = T.start()
		local ch, raw = F.channel("c"), F.channel("raw")
		T.raises(function() ch:pub_async({ v = "x" }) end, "not running under the scheduler")
		local e1, e2
		async._run(function()
			e1 = select(2, pcall(raw.pub_async, raw, { v = "x" }))
			F.claim("c")
			e2 = select(2, pcall(coroutine.wrap(function() return ch:pub_async({ v = "x" }) end)))
			F.finish("c")
		end)
		T.matches(e1, "has no message descriptor")
		T.matches(e2, "cannot park inside a coroutine")
	end },

	{ "the scheduler unsubscribes even when a channel lock is busy at exit", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		async._run(function()
			ch:await(0) -- watch
			F.hold("c", 200) -- the lock is busy while the run ends
		end)
		local ok, err
		async._run(function()
			async.sleep(250) -- past the hold
			ok, err = pcall(ch.await, ch, 0)
		end)
		T.ok(ok, "the next run can watch the channel again: " .. tostring(err))
	end },

	{ "await(K_NO_WAIT) returns a message the task just published", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local r, m, e
		async._run(function()
			ch:await(z.K_NO_WAIT) -- watch and take a read position
			r = ch:pub_async({ v = "mine" }, z.K_NO_WAIT)
			m, e = ch:await(z.K_NO_WAIT)
		end)
		T.eq(r, true)
		T.eq(m and m.v, "mine"); T.eq(e, nil)
	end },

	{ "a draining await wakes other channels' waiters in the next pass", function(T)
		local async, F = T.start()
		local a, b = F.channel("a"), F.channel("b")
		local log = {}
		async._run(function()
			local w = async.spawn(function()
				local m = b:await(100)
				log[#log + 1] = "b:" .. m.v
			end)
			a:await(z.K_NO_WAIT)
			async.yield() -- w parks on b
			b:pub_async({ v = "1" }, z.K_NO_WAIT)
			a:pub_async({ v = "2" }, z.K_NO_WAIT)
			log[#log + 1] = "a:" .. a:await(z.K_NO_WAIT).v
			w:join()
		end)
		T.same(log, { "a:2", "b:1" })
	end },

	{ "a first await skips the ring but receives what is still queued", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local m1, m2, e2
		async._run(function()
			async.spawn(function() ch:await(100) end) -- watches c
			async.yield()
			F.publish("c", { v = "in ring" })
			async.yield() -- the next pass moves it into the ring
			ch:pub_async({ v = "queued" }, z.K_NO_WAIT)
			m1 = ch:await(z.K_NO_WAIT) -- this task's first await on c
			m2, e2 = ch:await(z.K_NO_WAIT)
		end)
		T.eq(m1 and m1.v, "queued")
		T.eq(m2, nil); T.eq(e2, -z.EAGAIN)
	end },

	{ "dropped() outside a task raises", function(T)
		local _, F = T.start()
		T.raises(function() F.channel("c"):dropped() end, "not running under the scheduler")
	end },
}

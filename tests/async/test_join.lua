-- handle:join

local z = require("zephyr")

return {
	{ "join returns true plus results", function(T)
		local async = T.start()
		local r
		async._run(function()
			local h = async.spawn(function(x)
				async.sleep(10)
				return x * 2, "ok", nil
			end, 21)
			r = table.pack(h:join())
		end)
		T.eq(r.n, 4, "result count (trailing nil kept)")
		T.eq(r[1], true); T.eq(r[2], 42); T.eq(r[3], "ok"); T.eq(r[4], nil)
	end },

	{ "join on a finished task returns at once, repeatedly", function(T)
		local async, F = T.start()
		local a, b, t
		async._run(function()
			local h = async.spawn(function() return "v" end)
			async.yield()
			local t0 = F.now
			a = select(2, h:join())
			b = select(2, h:join(0))
			t = F.now - t0
		end)
		T.eq(a, "v"); T.eq(b, "v"); T.eq(t, 0, "no time passed")
	end },

	{ "join reports failure and cancellation", function(T)
		local async = T.start()
		local f1, f2, c1, c2
		async._run(function()
			local bad = async.spawn(function() async.sleep(5); error("boom", 0) end)
			f1, f2 = bad:join()
			local victim = async.spawn(function() async.sleep(100) end)
			async.spawn(function() async.sleep(10); victim:cancel() end)
			c1, c2 = victim:join()
		end)
		T.eq(f1, false); T.eq(f2, "boom")
		T.eq(c1, false); T.eq(c2, -z.ECANCELED)
	end },

	{ "join timeout returns nil, timeout and leaves the target alone", function(T)
		local async = T.start()
		local r1, r2, later
		async._run(function()
			local h = async.spawn(function() async.sleep(100); return "late" end)
			r1, r2 = h:join(30)
			later = select(2, h:join())
		end)
		T.eq(r1, nil); T.eq(r2, -z.EAGAIN); T.eq(later, "late")
	end },

	{ "join(0) on a live task returns timeout immediately", function(T)
		local async = T.start()
		local r1, r2
		async._run(function()
			local h = async.spawn(function() async.sleep(10) end)
			r1, r2 = h:join(0)
		end)
		T.eq(r1, nil); T.eq(r2, -z.EAGAIN)
	end },

	{ "several joiners all receive the result", function(T)
		local async = T.start()
		local got = {}
		async._run(function()
			local h = async.spawn(function() async.sleep(10); return "r" end)
			for i = 1, 3 do
				async.spawn(function()
					local v = select(2, h:join()) -- park before indexing `got`
					got[#got + 1] = i .. v
				end)
			end
		end)
		T.same(got, { "1r", "2r", "3r" })
	end },

	{ "joining itself raises", function(T)
		local async = T.start()
		local err
		local h
		async._run(function()
			h = async.spawn(function()
				local ok, e = pcall(h.join, h)
				err = e
			end)
		end)
		T.matches(err, "cannot join itself")
	end },
}

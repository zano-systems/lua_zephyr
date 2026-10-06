-- coroutine library untouched, channel limit, handle lifetime.

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
}

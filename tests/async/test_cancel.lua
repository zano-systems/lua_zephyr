-- handle:cancel

local z = require("zephyr")

local function closer(log, name)
	return setmetatable({}, { __close = function() log[#log + 1] = "close:" .. name end })
end

local BAD_TOSTRING = {
	throws = function() error("tostring failed") end,
	["returns a table"] = function() return {} end,
}

local function cancel_with_bad_close(T, tostring_fn, protected)
	local async, F = T.start()
	local bad = setmetatable({}, { __tostring = tostring_fn })
	local r, status, j1, j2, other_done
	async._run(function()
		local target = async.spawn(function()
			local c <close> = setmetatable({}, { __close = function() error(bad) end })
			async.sleep(1000)
		end)
		local joiner = async.spawn(function() return target:join() end)
		async.spawn(function() async.sleep(20); other_done = true end)
		async.yield() -- target and joiner park
		if protected then
			r = select(2, pcall(target.cancel, target))
		else
			r = target:cancel()
		end
		status = target:status()
		j1, j2 = select(2, joiner:join())
	end)
	T.eq(r, true, "cancel result")
	T.eq(status, "cancelled")
	T.eq(j1, false); T.eq(j2, -z.ECANCELED)
	T.ok(other_done, "unrelated task finished")
	T.eq(#F.reports, 1, "one report")
	T.matches(F.reports[1], "error while cancelling task: %(error object is a table value%)")
end

return {
	{ "cancel a sleeping task runs __close and never resumes it", function(T)
		local async = T.start()
		local log = {}
		async._run(function()
			local h = async.spawn(function()
				local c <close> = closer(log, "task")
				async.sleep(100)
				log[#log + 1] = "resumed"
			end)
			async.sleep(10)
			T.eq(h:cancel(), true)
			T.eq(h:status(), "cancelled")
			T.eq(h:cancel(), false, "second cancel")
		end)
		T.same(log, { "close:task" })
	end },

	{ "cancelling the only other ready task in the same pass", function(T)
		local async = T.start()
		local log = {}
		local victim
		async._run(function()
			async.spawn(function() victim:cancel(); log[#log + 1] = "canceller" end)
			victim = async.spawn(function() log[#log + 1] = "victim" end)
		end)
		T.same(log, { "canceller" })
	end },

	{ "a canceller that yields waits for the next pass", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local got, victim
		async._run(function()
			async.spawn(function()
				ch:await(0) -- watch; cursor at head
				victim:cancel()
				F.publish("c", { v = "next pass" })
				async.yield()
				local m = ch:await(0) -- messages are delivered between passes
				got = m and m.v
			end)
			victim = async.spawn(function() end)
		end)
		T.eq(got, "next pass")
	end },

	{ "pcall in the target cannot stop a cancel", function(T)
		local async = T.start()
		local log = {}
		async._run(function()
			local h = async.spawn(function()
				local ok = pcall(async.sleep, 100)
				log[#log + 1] = "after-pcall:" .. tostring(ok)
			end)
			async.yield()
			h:cancel()
		end)
		T.same(log, {})
	end },

	{ "cancel a waiting task releases it from the channel", function(T)
		local async, F = T.start()
		local ch = F.channel("c")
		local got = {}
		async._run(function()
			local h = async.spawn(function() ch:await() end)
			async.spawn(function()
				local m = ch:await(1000)
				got[#got + 1] = m and m.v
			end)
			async.yield()
			h:cancel()
			F.publish("c", { v = 7 })
		end)
		T.same(got, { "7" })
	end },

	{ "cancel a joining task removes it from the target's joiners", function(T)
		local async = T.start()
		local log = {}
		async._run(function()
			local target = async.spawn(function() async.sleep(50); return 1 end)
			local j = async.spawn(function()
				target:join()
				log[#log + 1] = "joiner resumed"
			end)
			async.yield()
			j:cancel()
			T.eq(target:status(), "sleeping")
			select(2, target:join())
		end)
		T.same(log, {})
	end },

	{ "cancel a not-yet-started task", function(T)
		local async = T.start()
		local ran = false
		async._run(function()
			local h = async.spawn(function() ran = true end)
			h:cancel()
		end)
		T.eq(ran, false)
	end },

	{ "children survive their parent's cancellation", function(T)
		local async = T.start()
		local child_done = false
		async._run(function()
			local parent = async.spawn(function()
				async.spawn(function() async.sleep(50); child_done = true end)
				async.sleep(1000)
			end)
			async.sleep(10)
			parent:cancel()
		end)
		T.ok(child_done)
	end },

	{ "a __close that parks during cancel is reported, chain still closed", function(T)
		local async, F = T.start()
		local log = {}
		async._run(function()
			local h = async.spawn(function()
				local a <close> = closer(log, "outer")
				local b <close> = setmetatable({}, { __close = function()
					async.sleep(1)
					log[#log + 1] = "unreachable"
				end })
				async.sleep(100)
			end)
			async.yield()
			h:cancel()
		end)
		T.same(log, { "close:outer" })
		T.eq(#F.reports, 1, "one report")
		T.matches(F.reports[1], "error while cancelling task")
	end },

	{ "cancel completes when the close error's __tostring throws", function(T)
		cancel_with_bad_close(T, BAD_TOSTRING.throws, false)
	end },

	{ "cancel completes when the close error's __tostring returns a table", function(T)
		cancel_with_bad_close(T, BAD_TOSTRING["returns a table"], false)
	end },

	{ "cancel under pcall completes when the close error's __tostring throws", function(T)
		cancel_with_bad_close(T, BAD_TOSTRING.throws, true)
	end },

	{ "cancel under pcall completes when the close error's __tostring returns a table", function(T)
		cancel_with_bad_close(T, BAD_TOSTRING["returns a table"], true)
	end },

	{ "cancelling itself raises", function(T)
		local async = T.start()
		local err
		local h
		async._run(function()
			h = async.spawn(function()
				local ok, e = pcall(h.cancel, h)
				err = e
			end)
		end)
		T.matches(err, "cannot cancel itself")
	end },
}

-- Task errors and scheduler faults.

local function fail_with_bad_close(T, tostring_fn)
	local async, F = T.start()
	local bad = setmetatable({}, { __tostring = tostring_fn })
	local h, status, other_done, err
	local ok = async._run(function()
		h = async.spawn(function()
			local c <close> = setmetatable({}, { __close = function() error(bad) end })
			error("boom")
		end)
		async.spawn(function() async.sleep(20); other_done = true end)
		async.sleep(5)
		status = h:status()
		err = select(2, h:join())
	end)
	T.ok(ok, "not a scheduler fault")
	T.eq(status, "failed")
	T.ok(rawequal(err, bad), "join gets the close error")
	T.ok(other_done, "unrelated task finished")
	T.eq(#F.reports, 1, "one report")
	T.matches(F.reports[1], "^async task error: %(error object is a table value%)")
end

return {
	{ "unjoined task error is printed once, others keep running", function(T)
		local async, F = T.start()
		local other = 0
		async._run(function()
			async.spawn(function() async.sleep(5); error("kaboom") end)
			async.spawn(function()
				for _ = 1, 3 do
					async.sleep(4)
					other = other + 1
				end
			end)
		end)
		T.eq(other, 3)
		T.eq(#F.reports, 1)
		T.matches(F.reports[1], "^async task error: .*kaboom")
		T.matches(F.reports[1], "stack traceback", "traceback included")
	end },

	{ "error delivered to a waiting joiner is not printed", function(T)
		local async, F = T.start()
		local e
		async._run(function()
			local h = async.spawn(function() async.sleep(5); error("handled", 0) end)
			e = select(2, h:join())
		end)
		T.eq(e, "handled")
		T.eq(#F.reports, 0)
	end },

	{ "late joiner still gets an error that was printed", function(T)
		local async, F = T.start()
		local e
		async._run(function()
			local h = async.spawn(function() error("early", 0) end)
			async.sleep(10)
			e = select(2, h:join())
		end)
		T.eq(e, "early")
		T.eq(#F.reports, 1)
	end },

	{ "task-0 error is reported and the loop continues", function(T)
		local async, F = T.start()
		local done = false
		local ok = async._run(function()
			async.spawn(function() async.sleep(10); done = true end)
			error("body failed")
		end)
		T.ok(ok, "not a scheduler fault")
		T.ok(done)
		T.matches(F.reports[1], "body failed")
	end },

	{ "a failed task's close error with a throwing __tostring is reported", function(T)
		fail_with_bad_close(T, function() error("tostring failed") end)
	end },

	{ "a failed task's close error with a table-returning __tostring is reported", function(T)
		fail_with_bad_close(T, function() return {} end)
	end },

	{ "failed task's __close handlers run", function(T)
		local async = T.start()
		local closed = false
		async._run(function()
			async.spawn(function()
				local c <close> = setmetatable({}, { __close = function() closed = true end })
				error("x")
			end)
		end)
		T.ok(closed)
	end },
}

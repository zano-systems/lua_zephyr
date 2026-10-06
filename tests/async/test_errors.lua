-- Task errors and scheduler faults.

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

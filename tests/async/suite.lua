-- Runs every test_*.lua case, each under a fresh scheduler (luaz_async_run).

-- Libraries are preloaded, not global; the test files expect them as globals.
string = require("string")
table = require("table")
debug = require("debug")
coroutine = require("coroutine")

local zbus = require("zephyr").zbus
local lib = require("testlib")

local cfg = lib.config()
local DEFAULTS = {
	max_tasks = 8, max_chans = 8, depth = 4, max_drain = 16, node_pool = 0,
	max_readers = 16, max_locks = 4,
}

lib.run(function() end) -- installs the zephyr.async preload
local async = require("zephyr.async")

local T = {}

-- Recorded as well as raised: inside a task, the scheduler catches the error.
local failures = {}
local SKIP = {}

local function fail(msg, level)
	local where = debug.traceback("assertion failed: " .. msg, (level or 1) + 2)
	failures[#failures + 1] = where
	error("assertion failed: " .. msg, (level or 1) + 2)
end

local function show(v)
	if type(v) == "string" then
		return string.format("%q", v)
	end
	return tostring(v)
end

function T.eq(actual, expected, what)
	if actual ~= expected then
		fail((what or "value") .. ": expected " .. show(expected) .. ", got " .. show(actual))
	end
end

function T.ok(v, what)
	if not v then
		fail(what or "expected truthy value")
	end
end

function T.same(actual, expected, what)
	local a = table.concat(actual, ",")
	local e = table.concat(expected, ",")
	if a ~= e then
		fail((what or "sequence") .. ":\n  expected " .. e .. "\n  got      " .. a)
	end
end

function T.matches(s, pattern, what)
	if type(s) ~= "string" or not s:find(pattern) then
		fail((what or "string") .. ": " .. show(s) .. " does not match " .. show(pattern))
	end
end

function T.raises(fn, pattern, what)
	local ok, err = pcall(fn)
	if ok then
		fail((what or "call") .. ": expected an error matching " .. show(pattern))
	end
	T.matches(tostring(err), pattern, what or "error")
	return err
end

T.config = cfg

local function channel(name)
	return zbus.channel_declare("t_" .. name)
end

-- Scheduler reports are printk lines "[thread] msg"; tracebacks continue them.
local function parse_reports(text, into)
	for line in text:gmatch("[^\n]+") do
		local body = line:match("^%[[%w_]+%] (.*)")
		if body then
			into[#into + 1] = body
		elseif #into > 0 then
			into[#into] = into[#into] .. "\n" .. line
		end
	end
end

-- Cases that pass limits run only on a build with those values; the others
-- only on the default build.
function T.start(needs)
	local want = needs or DEFAULTS
	for k, v in pairs(want) do
		if cfg[k] ~= v then
			error(SKIP)
		end
	end
	if needs == nil then
		for k, v in pairs(DEFAULTS) do
			if cfg[k] ~= v then
				error(SKIP)
			end
		end
	end

	lib.captured()
	lib.align()
	local t0 = lib.now()
	local reports = {}
	local F = setmetatable({
		channel = channel,
		publish = function(name, value)
			local err = channel(name):pub(value, 100)
			if err ~= 0 then
				error("pub on " .. name .. " failed: " .. err, 2)
			end
		end,
		advance = lib.busy,
		claim = function(name) return lib.claim("t_" .. name) end,
		finish = function(name) return lib.finish("t_" .. name) end,
		hold = function(name, ms) lib.hold("t_" .. name, ms) end,
	}, {
		__index = function(_, k)
			if k == "now" then
				return lib.now() - t0
			elseif k == "reports" then
				parse_reports(lib.captured(), reports)
				return reports
			end
		end,
	})
	local A = setmetatable({ _run = lib.run }, { __index = async })
	return A, F
end

local passed, failed, skipped = 0, 0, 0
for _, file in ipairs(lib.files()) do
	local short, src = file[1], file[2]
	local tests = assert(load(src, "@" .. short .. ".lua"))()
	for _, case in ipairs(tests) do
		local name, fn = case[1], case[2]
		collectgarbage() -- the heap peak then follows live data, not collector timing
		failures = {}
		local ok, err = xpcall(fn, function(e)
			return e == SKIP and SKIP or debug.traceback(e)
		end, T)
		if err == SKIP then
			skipped = skipped + 1
		else
			if ok and #failures > 0 then
				ok, err = false, failures[1]
			end
			if ok then
				passed = passed + 1
			else
				failed = failed + 1
				print("FAIL " .. short .. ": " .. name .. "\n  " .. tostring(err):gsub("\n", "\n  "))
			end
		end
	end
end
print(string.format("async tests: %d passed, %d failed, %d skipped", passed, failed, skipped))

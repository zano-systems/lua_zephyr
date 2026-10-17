-- Every awaiting task sees every message; a slow one loses the oldest.
local z = require("zephyr")
local async = require("zephyr.async")
local zbus = z.zbus

local tick = zbus.channel_declare("chan_tick")
local idle = zbus.channel_declare("chan_idle")
local raw = zbus.channel_declare("chan_raw")

tick:queue_depth(8)

local function fast()
	local n = 0
	for _ = 1, 12 do
		local m = tick:await()
		n = n + 1
		if m.seq ~= n then
			z.printk("fast: out of order " .. m.seq)
		end
	end
	z.printk("fast got " .. n .. " dropped " .. tick:dropped())
end

local function slow()
	local first = tick:await()
	async.sleep(600)
	local got = 0
	while tick:await(0) do
		got = got + 1
	end
	z.printk("slow first " .. first.seq .. " later " .. got .. " dropped " .. tick:dropped())
end

local function idle_fn()
	local m, err = idle:await(50)
	z.printk("idle " .. tostring(m) .. " " .. (err == -z.EAGAIN and "EAGAIN" or err))
end

local function blocker()
	for _ = 1, 3 do
		z.msleep(1)
		async.yield()
	end
end

async.spawn(fast)
async.spawn(slow)
async.spawn(idle_fn)
async.spawn(blocker)

local ok, err = pcall(raw.await, raw, 10)
z.printk("raw " .. tostring(ok) .. " " .. err)

-- Three tasks sharing one thread. The script ends when all of them finish.

local z = require("zephyr")
local async = require("zephyr.async")

local function worker(name, interval, rounds)
	for i = 1, rounds do
		z.printk(name .. " round " .. i)
		async.sleep(interval)
	end
	z.printk(name .. " done")
end

z.printk("loop start")

async.spawn(worker, "alpha", 100, 3)
async.spawn(worker, "beta", 150, 3)
async.spawn(worker, "gamma", 200, 3)

z.printk("tasks spawned")

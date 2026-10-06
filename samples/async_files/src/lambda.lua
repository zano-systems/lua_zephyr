-- One task of the workers thread; all 16 files share its scheduler.

local z = require("zephyr")
local async = require("zephyr.async")

for i = 1, 3 do
	z.printk("lambda round " .. i)
	async.sleep(150)
end
z.printk("lambda done")

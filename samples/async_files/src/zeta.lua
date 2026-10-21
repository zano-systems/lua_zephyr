-- One task of the workers thread; all 16 files share its scheduler.

local z = require("zephyr")
local async = require("zephyr.async")

for i = 1, 3 do
	z.printk("zeta round " .. i)
	async.sleep(100)
end
z.printk("zeta done")

-- One task of the workers thread; all 16 files share its scheduler.

local z = require("zephyr")
local async = require("zephyr.async")

for i = 1, 3 do
	z.printk("gamma round " .. i)
	async.sleep(70)
end
z.printk("gamma done")
